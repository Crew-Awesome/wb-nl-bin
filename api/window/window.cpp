#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>

#if defined(__linux__) || defined(__FreeBSD__)
#include <type_traits>
#include <dlfcn.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "lib/platformfolders/platform_folders.h"

#elif defined(__APPLE__)
#include <objc/objc-runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGDisplayConfiguration.h>
#include <CoreGraphics/CGWindow.h>
#include <dlfcn.h>

#if defined(__APPLE__) && MAC_OS_X_VERSION_MIN_REQUIRED >= 120300
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#endif

#define NSBaseWindowLevel 0
#define NSFloatingWindowLevel 5
#define NSWindowStyleMaskFullScreen 16384
#define NSWindowStyleMaskTitled 1
#define NSPNGFileType 4
#define NSDragOperationCopy 1

#define kCGWindowListOptionIncludingWindow 8
#define kCGWindowImageBoundsIgnoreFraming 1

#elif defined(_WIN32)
#define _WINSOCKAPI_
#include <windows.h>
#include <gdiplus.h>
#include <winuser.h>
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "WebView2LoaderStatic.lib")
#include "webview2.h"
#include <wrl.h>
#include <ShlObj_core.h>
#include <ShObjIdl_core.h>
#include <shellapi.h>
#endif

#include "lib/json/json.hpp"
#include "lib/webview/webview.h"
#include "settings.h"
#include "resources.h"
#include "helpers.h"
#include "errors.h"
#include "server/neuserver.h"
#include "auth/permission.h"
#include "api/app/app.h"
#include "api/window/window.h"
#include "api/events/events.h"
#include "api/fs/fs.h"
#include "api/debug/debug.h"
#include "api/computer/computer.h"
#include "api/os/os.h"

using namespace std;
using json = nlohmann::json;
#if defined(_WIN32)
using namespace Gdiplus;
#endif

#if defined(__linux__)
// Route WebKit helper processes through runtime symlinks so they show up
// under the application's process name instead of WebKit*Process.
extern "C" GSubprocess* g_subprocess_launcher_spawnv(
        GSubprocessLauncher *launcher,
        const gchar * const *argv,
        GError **error) {
    using SpawnvFn = GSubprocess* (*)(GSubprocessLauncher*, const gchar * const *, GError**);
    static SpawnvFn realSpawnv = reinterpret_cast<SpawnvFn>(
        dlsym(RTLD_NEXT, "g_subprocess_launcher_spawnv"));
    if(!realSpawnv) {
        return nullptr;
    }
    if(!argv || !argv[0]) {
        return realSpawnv(launcher, argv, error);
    }

    string binPath(argv[0]);
    string baseName = filesystem::path(binPath).filename().string();
    string procName = settings::getProcessName();
    string suffix;
    if(baseName == "WebKitWebProcess") {
        suffix = "-web";
    }
    else if(baseName == "WebKitNetworkProcess") {
        suffix = "-net";
    }
    else if(baseName == "WebKitGPUProcess") {
        suffix = "-gpu";
    }
    else if(baseName.find("WebKit") == 0) {
        suffix = "-wk";
    }

    if(suffix.empty()) {
        return realSpawnv(launcher, argv, error);
    }

    const char *runtimeDir = getenv("XDG_RUNTIME_DIR");
    string linkDir = (runtimeDir && runtimeDir[0] != '\0')
        ? string(runtimeDir) + "/" + procName + "-bin"
        : sago::getDataHome() + "/" + settings::getAppId() + "/.bin";

    std::error_code ec;
    filesystem::create_directories(linkDir, ec);
    string linkPath = linkDir + "/" + procName + suffix;
    if(filesystem::exists(linkPath, ec) || filesystem::is_symlink(linkPath, ec)) {
        filesystem::remove(linkPath, ec);
    }
    filesystem::create_symlink(binPath, linkPath, ec);

    if(!ec && access(linkPath.c_str(), X_OK) == 0) {
        vector<const gchar*> modifiedArgv;
        modifiedArgv.push_back(linkPath.c_str());
        for(size_t i = 1; argv[i] != nullptr; ++i) {
            modifiedArgv.push_back(argv[i]);
        }
        modifiedArgv.push_back(nullptr);
        return realSpawnv(launcher, modifiedArgv.data(), error);
    }

    return realSpawnv(launcher, argv, error);
}
#endif

namespace window {

webview::webview *nativeWindow;
#if defined(__linux__) || defined(__FreeBSD__)
bool isGtkWindowFullScreen = false;
bool isGtkWindowMinimized = false;
GtkWidget *menuContainer;

bool __hasDesktopEntry(const string &desktopId) {
    if(desktopId.empty()) {
        return false;
    }
    std::error_code ec;
    string fileName = desktopId + ".desktop";
    vector<string> dirs = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        sago::getDataHome() + "/applications"
    };
    for(const auto &dir : dirs) {
        if(filesystem::exists(dir + "/" + fileName, ec)) {
            return true;
        }
    }
    return false;
}

string __resolveDesktopAppId() {
    vector<string> candidates = {
        settings::getProcessName(),
        settings::getAppId(),
        "weekbox"
    };
    for(const auto &id : candidates) {
        if(__hasDesktopEntry(id)) {
            return id;
        }
    }
    string procName = settings::getProcessName();
    return procName.empty() ? "weekbox" : procName;
}

#elif defined(_WIN32)
#define ID_MENU_FIRST 20000
bool isWinWindowFullScreen = false;
DWORD savedStyle;
DWORD savedStyleX;
RECT savedRect;
HMENU windowMenu;
int windowMenuItemId = ID_MENU_FIRST;
WNDPROC originalWndProc = nullptr;
HICON windowIcon = nullptr;
#endif

window::WindowOptions windowProps;
bool savedState = false;
NEU_W_HANDLE windowHandle;
std::thread::id uiThreadId;
std::mutex windowLifecycleMutex;

namespace handlers {

void windowStateChange(int state) {
    switch(state) {
        case WEBVIEW_WINDOW_CLOSE:
            if(windowProps.exitProcessOnClose ||
                !neuserver::isInitialized() || !permission::hasAPIAccess()) {
                app::exit();
            }
            else {
                events::dispatch("windowClose", nullptr);
            }
            break;
        case WEBVIEW_WINDOW_FOCUS:
            events::dispatch("windowFocus", nullptr);
            break;
        case WEBVIEW_WINDOW_BLUR:
            events::dispatch("windowBlur", nullptr);
            break;
        case WEBVIEW_WINDOW_FULLSCREEN:
            #if defined(__linux__) || defined(__FreeBSD__)
                isGtkWindowFullScreen = true;
            #endif
            events::dispatch("windowFullScreenEnter", nullptr);
            break;
        case WEBVIEW_WINDOW_UNFULLSCREEN:
            #if defined(__linux__) || defined(__FreeBSD__)
                isGtkWindowFullScreen = false;
            #endif
            events::dispatch("windowFullScreenExit", nullptr);
            break;
        case WEBVIEW_WINDOW_MINIMIZED:
            #if defined(__linux__) || defined(__FreeBSD__)
                isGtkWindowMinimized = true;
            #endif
            events::dispatch("windowMinimize", nullptr);
            break;
        case WEBVIEW_WINDOW_RESTORED:
            #if defined(__linux__) || defined(__FreeBSD__)
                isGtkWindowMinimized = false;
            #endif
            events::dispatch("windowRestore", nullptr);
            break;
        case WEBVIEW_WINDOW_SHOW:
            events::dispatch("windowShow", nullptr);
            break;
        case WEBVIEW_WINDOW_HIDE:
            events::dispatch("windowHide", nullptr);
            break;
        case WEBVIEW_WINDOW_MAXIMIZE:
            events::dispatch("windowMaximize", nullptr);
            break;
        case WEBVIEW_WINDOW_ACTIVATE:
            window::focus();
            break;
    }
}

bool decideNewWindowPolicy(const std::string& url) {
    switch(windowProps.newWindowPolicy) {
        case window::NewWindowPolicySystem:
            return false;
        case window::NewWindowPolicyBrowser:
            os::open(url);
            return true;
        case window::NewWindowPolicyCustom:
            events::dispatch("newWindowRequest", url);
            return true;               
    }
    return false;
}

} // namespace handlers


pair<int, int> __getCenterPos(bool useConfigSizes = false) {
    int x = 0, y = 0;
    int width = 0, height = 0;
    if(useConfigSizes) {
        width = windowProps.sizeOptions.width;
        height = windowProps.sizeOptions.height;
    }
    else {
        window::SizeOptions opt = window::getSize();
        width = opt.width;
        height = opt.height;
    }
    #if defined(__linux__) || defined(__FreeBSD__)
    GdkRectangle screen = {0};
	GdkDisplay *display = gdk_display_get_default();
    if(display){
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if(!monitor) {
			monitor = gdk_display_get_monitor(display, 0);
		}
        if(monitor) {
			gdk_monitor_get_workarea(monitor, &screen);
		}
    }
    x = (screen.width - width) / 2;
    y = (screen.height - height) / 2;
    #elif defined(__APPLE__)
    auto displayId = CGMainDisplayID();
    x = (CGDisplayPixelsWide(displayId) - width) / 2;
    y = (CGDisplayPixelsHigh(displayId) - height) / 2;
    #elif defined(_WIN32)
	RECT screen;
	GetWindowRect(GetDesktopWindow(), &screen);
    x = ((screen.right - screen.left) - width) / 2;
    y = ((screen.bottom - screen.top) - height) / 2;
    #endif
    return make_pair(x, y);
}

#if defined(__APPLE__)
CGRect __getWindowRect() {
    CGRect winPos = ((CGRect (*)(id, SEL))objc_msgSend)(
        (id) windowHandle, "frame"_sel);
    return winPos;
}
#endif

#if defined(_WIN32)
bool __isFakeHidden() {
	// Checks whether the window is on the screen viewport
	RECT winPos;

	if(GetWindowRect( windowHandle, &winPos)) {
		return winPos.left > 9999;
	}
	return false;
}

void __undoFakeHidden() {
    int x = windowProps.x;
    int y = windowProps.y;
    if(!window::isSavedStateLoaded() && windowProps.center) {
        pair<int, int> pos = __getCenterPos(true);
        x = pos.first;
        y = pos.second;
    }
	ShowWindow(windowHandle, SW_HIDE);
	SetWindowLong(windowHandle, GWL_EXSTYLE, nativeWindow->m_originalStyleEx);
	SetWindowPos(windowHandle, nullptr,
        x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
	ShowWindow(windowHandle, SW_SHOW);
}

bool __getEncoderClsid(const WCHAR *format, CLSID *pClsid) {
    UINT num = 0;
    UINT size = 0;

    GetImageEncodersSize(&num, &size);
    if(size == 0) return false; // Failure

    ImageCodecInfo *pImageCodecInfo = (ImageCodecInfo *)(malloc(size));
    if(pImageCodecInfo == NULL) return false;

    GetImageEncoders(num, size, pImageCodecInfo);
    for(UINT i = 0; i < num; ++i) {
        if(wcscmp(pImageCodecInfo[i].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[i].Clsid;
            free(pImageCodecInfo);
            return true;
        }
    }
    free(pImageCodecInfo);
    return false;
}
#endif

double __getScaleFactor() {
	#if defined(_WIN32)
    return GetDpiForSystem() / 96.0;

	#elif defined(__APPLE__)
    id screen = ((id (*)(id, SEL))objc_msgSend)(
        "NSScreen"_cls, "mainScreen"_sel);
    return ((double (*)(id, SEL))objc_msgSend)(
        screen, "backingScaleFactor"_sel);

	#elif defined(__linux__) || defined(__FreeBSD__)
    GdkDisplay* display = gdk_display_get_default();
    GdkMonitor* monitor = gdk_display_get_primary_monitor(display);
    return gdk_monitor_get_scale_factor(monitor);

	#else
    return 1.0;
	#endif
}

json __sizeOptionsToJson(const window::SizeOptions &opt) {
    json output = {
        {"width", opt.width},
        {"height", opt.height},
        {"minWidth", opt.minWidth},
        {"minHeight", opt.minHeight},
        {"maxWidth", opt.maxWidth},
        {"maxHeight", opt.maxHeight},
        {"resizable", opt.resizable}
    };
    return output;
}

void __saveWindowProps() {
    windowProps.sizeOptions = window::getSize();
    json options = __sizeOptionsToJson(windowProps.sizeOptions);
    pair<int, int> pos = window::getPosition();
    options["x"] = pos.first;
    options["y"] = pos.second;
    options["maximize"] = window::isMaximized();
    
    #if defined(_WIN32)
    if(IsZoomed(windowHandle)) {
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        GetWindowPlacement(windowHandle, &wp);
        options["width"] = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        options["height"] = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        options["x"] = wp.rcNormalPosition.left;
        options["y"] = wp.rcNormalPosition.top;
    }
    #endif

    filesystem::create_directories(CONVSTR(settings::joinAppDataPath("/.tmp")));
    fs::FileWriterOptions writerOptions = { settings::joinAppDataPath(NEU_WIN_CONFIG_FILE), helpers::jsonToString(options) };
    fs::writeFile(writerOptions);
}

bool __loadSavedWindowProps() {
    fs::FileReaderResult readerResult = fs::readFile(settings::joinAppDataPath(NEU_WIN_CONFIG_FILE));
    if(readerResult.status != errors::NE_ST_OK) {
        return false;
    }

    try {
        json options = json::parse(readerResult.data);
        windowProps.x = options["x"].get<int>();
        windowProps.y = options["y"].get<int>();
        windowProps.maximize = options["maximize"].get<bool>();
        windowProps.sizeOptions.width = options["width"].get<int>();
        windowProps.sizeOptions.height = options["height"].get<int>();

        #if defined(_WIN32)
        WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
        wp.rcNormalPosition.left = windowProps.x;
        wp.rcNormalPosition.top = windowProps.y;
        wp.rcNormalPosition.right = windowProps.x + windowProps.sizeOptions.width;
        wp.rcNormalPosition.bottom = windowProps.y + windowProps.sizeOptions.height;
        wp.showCmd = SW_SHOWNORMAL;

        SetWindowPlacement(windowHandle, &wp);
        #endif
    }
    catch(const exception& e) {
        debug::log(debug::LogTypeError, errors::makeErrorMsg(errors::NE_CF_UNBLWCF, string(NEU_WIN_CONFIG_FILE)));
        return false;
    }
    return true;
}

void __handleMainMenuItem(window::WindowMenuItem *item) {
    (void)item;
    json eventData;
    eventData["id"] = item->id;
    eventData["text"] = item->text;
    eventData["isChecked"] = item->checked;
    eventData["isDisabled"] = item->disabled;
    events::dispatch("mainMenuItemClicked", eventData);
}

#if defined(_WIN32)
HMENU __createMenu(const json &menu, bool root) {
    HMENU hMenu = root ? CreateMenu() : CreatePopupMenu();
    for(const auto &jMenuItem : menu) {
        window::WindowMenuItem *menuItem = new window::WindowMenuItem;

        if(helpers::hasField(jMenuItem, "id")) {
            menuItem->id = jMenuItem["id"].get<string>();
        }

        if(helpers::hasField(jMenuItem, "text")) {
            menuItem->text = jMenuItem["text"].get<string>();
        } 

        if(helpers::hasField(jMenuItem, "isDisabled")) {
            menuItem->disabled = jMenuItem["isDisabled"].get<bool>();
        }

        if(helpers::hasField(jMenuItem, "isChecked")) {
            menuItem->checked = jMenuItem["isChecked"].get<bool>();
        }  

        if(helpers::hasField(jMenuItem, "shortcut")) {
            menuItem->shortcut = jMenuItem["shortcut"].get<string>();
            menuItem->text += "\t\t" + menuItem->shortcut;
        }

        menuItem->cb = __handleMainMenuItem;

        if(menuItem->text == "-") {
            InsertMenu(hMenu, windowMenuItemId, MF_SEPARATOR, 1, L"");
        } 
        else {
            MENUITEMINFO item;
            memset(&item, 0, sizeof(item));
            item.cbSize = sizeof(MENUITEMINFO);
            item.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
            item.fType = 0;
            item.fState = 0;
            if (helpers::hasField(jMenuItem, "menuItems")) {
                item.fMask = item.fMask | MIIM_SUBMENU;
                item.hSubMenu = __createMenu(jMenuItem["menuItems"], false);
                SendMessage(windowHandle, WM_WINDOW_PASS_MENU_REFS, (WPARAM)item.hSubMenu, 0);
            }
            if(menuItem->disabled) {
                item.fState |= MFS_DISABLED;
            }
            if (menuItem->checked) {
                item.fState |= MFS_CHECKED;
            }
            item.wID = windowMenuItemId;
            wstring wtext = helpers::str2wstr(menuItem->text);
            item.dwTypeData = (LPWSTR)wtext.data();
            item.dwItemData = (ULONG_PTR)menuItem;

            InsertMenuItem(hMenu, windowMenuItemId, 1, &item);

            windowMenuItemId++;
        }   
    }

    return hMenu;
}
#elif defined(__APPLE__)
id __createMenu(const json &menu) {
    id nsMenu = ((id (*)(id, SEL))objc_msgSend)("NSMenu"_cls, "new"_sel);
                ((id (*)(id, SEL, id))objc_msgSend)(
                    nsMenu,
                    "initWithTitle:"_sel,
                    ((id (*)(id, SEL, const char *))objc_msgSend)("NSString"_cls, "stringWithUTF8String:"_sel, "")
                );

    ((id (*)(id, SEL))objc_msgSend)(nsMenu, "autorelease"_sel);
    ((id (*)(id, SEL, bool))objc_msgSend)(nsMenu, "setAutoenablesItems:"_sel, false);
    for(const auto &jMenuItem : menu) {
        window::WindowMenuItem *menuItem = new window::WindowMenuItem;

        if(helpers::hasField(jMenuItem, "id")) {
            menuItem->id = jMenuItem["id"].get<string>();
        }

        if(helpers::hasField(jMenuItem, "text")) {
            menuItem->text = jMenuItem["text"].get<string>();
        } 

        if(helpers::hasField(jMenuItem, "isDisabled")) {
            menuItem->disabled = jMenuItem["isDisabled"].get<bool>();
        }

        if(helpers::hasField(jMenuItem, "isChecked")) {
            menuItem->checked = jMenuItem["isChecked"].get<bool>();
        }  

        if(helpers::hasField(jMenuItem, "action")) {
            menuItem->action = jMenuItem["action"].get<string>();
        }

        if(helpers::hasField(jMenuItem, "shortcut")) {
            menuItem->shortcut = jMenuItem["shortcut"].get<string>();
        } 

        menuItem->cb = __handleMainMenuItem;

        if(menuItem->text == "-") {
            id separatorItem = ((id (*)(id, SEL))objc_msgSend)(
                    "NSMenuItem"_cls,
                    "separatorItem"_sel);

            ((id (*)(id, SEL, id))objc_msgSend)(nsMenu, "addItem:"_sel, separatorItem);
        } 
        else {
            id nsMenuItem = ((id (*)(id, SEL))objc_msgSend)("NSMenuItem"_cls, "alloc"_sel);

            ((id (*)(id, SEL))objc_msgSend)(nsMenuItem, "autorelease"_sel);

            ((id (*)(id, SEL, id, SEL, id))objc_msgSend)(
                nsMenuItem,
                "initWithTitle:action:keyEquivalent:"_sel,
                ((id (*)(id, SEL, const char *))objc_msgSend)("NSString"_cls, "stringWithUTF8String:"_sel, menuItem->text.c_str()),
                sel_registerName(menuItem->action.c_str()),
                ((id (*)(id, SEL, const char *))objc_msgSend)("NSString"_cls, "stringWithUTF8String:"_sel, menuItem->shortcut.c_str()));

            ((id (*)(id, SEL, bool))objc_msgSend)(nsMenuItem, "setEnabled:"_sel, !menuItem->disabled);
            ((id (*)(id, SEL, bool))objc_msgSend)(nsMenuItem, "setState:"_sel, menuItem->checked);

            ((id (*)(id, SEL, id))objc_msgSend)(
                nsMenuItem,
                "setRepresentedObject:"_sel,
                ((id (*)(id, SEL, void*))objc_msgSend)("NSValue"_cls, "valueWithPointer:"_sel, menuItem));

            ((id (*)(id, SEL, id))objc_msgSend)(nsMenu, "addItem:"_sel, nsMenuItem);

            if (helpers::hasField(jMenuItem, "menuItems")) {
                id nsSubmenu = __createMenu(jMenuItem["menuItems"]);
                ((id (*)(id, SEL, id, id))objc_msgSend)(
                    nsMenu,
                    "setSubmenu:forItem:"_sel,
                    nsSubmenu,
                    nsMenuItem);
                ((id (*)(id, SEL, id))objc_msgSend)(
                    nsSubmenu,
                    "setTitle:"_sel,
                    ((id (*)(id, SEL, const char *))objc_msgSend)("NSString"_cls, "stringWithUTF8String:"_sel, menuItem->text.c_str())
                );
            }
        }   
    }

    return nsMenu;
}
#elif defined(__linux__) || defined(__FreeBSD__)
static void __menuCallback(GtkMenuItem *item, gpointer data) {
  (void)item;
  window::WindowMenuItem *menuItem = (window::WindowMenuItem *)data;
  menuItem->cb(menuItem);
}

GtkMenuShell* __createMenu(const json &menu, bool root) {
    GtkMenuShell *gMenu = root ? (GtkMenuShell *)gtk_menu_bar_new() : (GtkMenuShell *)gtk_menu_new() ;
    for(const auto &jMenuItem : menu) {
        window::WindowMenuItem *menuItem = new window::WindowMenuItem;

        if(helpers::hasField(jMenuItem, "id")) {
            menuItem->id = jMenuItem["id"].get<string>();
        }

        if(helpers::hasField(jMenuItem, "text")) {
            menuItem->text = jMenuItem["text"].get<string>();
        } 

        if(helpers::hasField(jMenuItem, "isDisabled")) {
            menuItem->disabled = jMenuItem["isDisabled"].get<bool>();
        }

        if(helpers::hasField(jMenuItem, "isChecked")) {
            menuItem->checked = jMenuItem["isChecked"].get<bool>();
        }  

        if(helpers::hasField(jMenuItem, "shortcut")) {
            menuItem->shortcut = jMenuItem["shortcut"].get<string>();
            menuItem->text += "\t\t" + menuItem->shortcut;
        }

        menuItem->cb = __handleMainMenuItem;

        GtkWidget *item;
        if(menuItem->text == "-") {
            item = gtk_separator_menu_item_new();
        } 
        else {
            if (helpers::hasField(jMenuItem, "menuItems")) {
                item = gtk_menu_item_new_with_label(menuItem->text.c_str());
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(item),
                                        GTK_WIDGET(__createMenu(jMenuItem["menuItems"], false)));
            } 
            else if(menuItem->checked) {
                item = gtk_check_menu_item_new_with_label(menuItem->text.c_str());
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), menuItem->checked);
            } 
            else {
                item = gtk_menu_item_new_with_label(menuItem->text.c_str());
            }
            gtk_widget_set_sensitive(item, !menuItem->disabled);
            if (menuItem->cb) {
                g_signal_connect(item, "activate", G_CALLBACK(__menuCallback), menuItem);
            }
        }
        gtk_widget_show(item);
        gtk_menu_shell_append(gMenu, item);   
    }

    return gMenu;
}
#endif

window::SizeOptions __jsonToSizeOptions(const json &input, bool useDefaultRect = false) {
    window::SizeOptions sizeOptions;

    if(useDefaultRect) {
        sizeOptions.width = 800;
        sizeOptions.height = 600;
    }

    if(helpers::hasField(input, "width"))
        sizeOptions.width = input["width"].get<int>();

    if(helpers::hasField(input, "height"))
        sizeOptions.height = input["height"].get<int>();

    if(helpers::hasField(input, "minWidth"))
        sizeOptions.minWidth = input["minWidth"].get<int>();

    if(helpers::hasField(input, "minHeight"))
        sizeOptions.minHeight = input["minHeight"].get<int>();

    if(helpers::hasField(input, "maxWidth"))
        sizeOptions.maxWidth = input["maxWidth"].get<int>();

    if(helpers::hasField(input, "maxHeight"))
        sizeOptions.maxHeight = input["maxHeight"].get<int>();

    if(helpers::hasField(input, "resizable"))
        sizeOptions.resizable = input["resizable"].get<bool>();
    return sizeOptions;
}

void __injectClientLibrary() {
    json options = settings::getConfig();
    json jClientLibrary = options["cli"]["clientLibrary"];
    if(!jClientLibrary.is_null()) {
        string clientLibPath = jClientLibrary.get<string>();
        fs::FileReaderResult fileReaderResult = resources::getFile(clientLibPath);
        if(fileReaderResult.status == errors::NE_ST_OK) {
            nativeWindow->init(settings::getGlobalVars() + "var NL_CINJECTED = true;" + 
                fileReaderResult.data);
        }
    }
}

void __injectScript() {
    json jInjectScript = settings::getOptionForCurrentMode("injectScript");
    if(!jInjectScript.is_null()) {
        string injectScript = jInjectScript.get<string>();
        fs::FileReaderResult fileReaderResult = resources::getFile(injectScript);
        if(fileReaderResult.status == errors::NE_ST_OK) {
            nativeWindow->init("var NL_SINJECTED = true;" + fileReaderResult.data);
        }
    }
}

struct SynchronousTaskState {
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
    bool started = false;
    bool cancelled = false;
    std::exception_ptr exception;
};

void dispatchSync(const std::function<void()> &task) {
    webview::webview *window = nullptr;
    std::thread::id uiThread;
    {
        std::lock_guard<std::mutex> guard(windowLifecycleMutex);
        window = nativeWindow;
        uiThread = uiThreadId;
    }
    if(!window) {
        throw std::runtime_error("Native window is not initialized");
    }

    if(std::this_thread::get_id() == uiThread) {
        task();
        return;
    }

    auto state = std::make_shared<SynchronousTaskState>();
    auto dispatchTask = [task, state]() {
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            if(state->cancelled) {
                state->completed = true;
                state->condition.notify_one();
                return;
            }
            state->started = true;
        }

        try {
            task();
        }
        catch(...) {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->exception = std::current_exception();
        }

        {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->completed = true;
        }
        state->condition.notify_one();
    };

    if(!window->dispatch(dispatchTask)) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->cancelled = true;
        state->completed = true;
        state->exception = std::make_exception_ptr(
            std::runtime_error("Unable to dispatch task to the native UI thread"));
        state->condition.notify_one();
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    while(!state->completed) {
        if(state->condition.wait_for(lock, std::chrono::seconds(5)) ==
                std::cv_status::timeout && !state->started) {
            state->cancelled = true;
            state->completed = true;
            state->exception = std::make_exception_ptr(
                std::runtime_error("Timed out waiting for the native UI thread"));
            break;
        }
    }
    if(state->exception) {
        std::rethrow_exception(state->exception);
    }
}


bool __createWindow() {
    savedState = windowProps.useSavedState && __loadSavedWindowProps();

    #if defined(__linux__) || defined(__FreeBSD__)
    string desktopAppId = __resolveDesktopAppId();
    g_set_prgname(desktopAppId.c_str());
    g_set_application_name(windowProps.title.c_str());
    #endif

    webview::webview *createdWindow = new webview::webview(
        windowProps.enableInspector, windowProps.openInspectorOnStartup,
        nullptr, windowProps.transparent, windowProps.webviewArgs,
        windowProps.emitDropEvents);

    if(createdWindow->get_init_code() == 1) {
        delete createdWindow;
        return false;
    }

    // Publish the fully constructed WebView only after its backend is ready.
    // The server can receive RPCs before the UI enters its event loop.
    {
        std::lock_guard<std::mutex> guard(windowLifecycleMutex);
        nativeWindow = createdWindow;
    }

    nativeWindow->set_title(windowProps.title);
    if(windowProps.extendUserAgentWith != "") {
        nativeWindow->extend_user_agent(windowProps.extendUserAgentWith);
    }

    int width = windowProps.sizeOptions.width;
    int height = windowProps.sizeOptions.height;
    if(windowProps.useLogicalPixels) {
        double scale = __getScaleFactor();
        if(width > 0)  width  = (int)(width  * scale);
        if(height > 0) height = (int)(height * scale);
    }

    nativeWindow->set_size(
    width,
    height,
    windowProps.sizeOptions.minWidth,
    windowProps.sizeOptions.minHeight,
    windowProps.sizeOptions.maxWidth,
    windowProps.sizeOptions.maxHeight,
    windowProps.sizeOptions.resizable
);

    nativeWindow->setEventHandler(&window::handlers::windowStateChange);
    nativeWindow->setNewWindowHandler(&window::handlers::decideNewWindowPolicy);
    nativeWindow->setFileDropHandler([](const vector<string>& droppedPaths) {
        events::dispatch("filesDropped", droppedPaths);
    });

    if(windowProps.injectGlobals) 
        nativeWindow->init(settings::getGlobalVars() + "var NL_GINJECTED = true;");

    if(windowProps.injectClientLibrary)
        __injectClientLibrary();

    if(windowProps.injectScript != "")
        __injectScript();

    #if defined(__linux__) || defined(__FreeBSD__)
    windowHandle = (GtkWidget*) nativeWindow->window();

    #elif defined(__APPLE__)
    windowHandle = (id) nativeWindow->window();
    ((void (*)(id, SEL, bool))objc_msgSend)((id) windowHandle,
                "setHasShadow:"_sel, true);

    #elif defined(_WIN32)
    windowHandle = (HWND) nativeWindow->window();
    #endif

    #if !defined(_WIN32)
    if(!window::isSavedStateLoaded() && windowProps.center)
        window::center(true);
    else
        window::move(windowProps.x, windowProps.y);
    #endif

    if(windowProps.hidden)
        window::hide();

    #if defined(_WIN32)
    if (!windowProps.hidden && __isFakeHidden())
		__undoFakeHidden();
    #endif

    if(windowProps.maximize)
        window::maximize();

    if(windowProps.fullScreen)
        window::setFullScreen();

    if(windowProps.icon != "" && !window::setIcon(windowProps.icon)) {
        debug::log(debug::LogTypeError,
            errors::makeErrorMsg(errors::NE_WI_UNBSICO, windowProps.icon));
    }

    if(windowProps.alwaysOnTop)
        window::setAlwaysOnTop(true);

    if(windowProps.borderless)
        window::setBorderless(true);

    if(windowProps.skipTaskbar)
        window::setSkipTaskbar(true);

    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_widget_realize(windowHandle);
    if(!windowProps.hidden) {
        gtk_widget_show_all(windowHandle);
    }
    #endif

    nativeWindow->navigate(windowProps.url);

    return true;
}

void __closeWindowOnUI(int exitCode) {
    os::cleanupTray();
    if(windowProps.useSavedState) {
        __saveWindowProps();
    }
    #if defined(_WIN32)
    if(windowHandle && originalWndProc) {
        DragAcceptFiles(windowHandle, FALSE);
        SetWindowLongPtr(windowHandle, GWLP_WNDPROC,
                         (LONG_PTR)originalWndProc);
        originalWndProc = nullptr;
    }
    if(windowIcon) {
        SendMessage(windowHandle, WM_SETICON, ICON_SMALL, 0);
        SendMessage(windowHandle, WM_SETICON, ICON_BIG, 0);
        DestroyIcon(windowIcon);
        windowIcon = nullptr;
    }
    #endif
    nativeWindow->terminate(exitCode);
}

void _close(int exitCode) {
    {
        std::lock_guard<std::mutex> guard(windowLifecycleMutex);
        if(!nativeWindow) {
            return;
        }
    }

    // GTK, Cocoa, Win32 and WebKit objects belong to the UI thread. In
    // particular, app.exit may arrive from a native worker after RPC
    // dispatching.
    dispatchSync([exitCode]() {
        webview::webview *window = nullptr;
        {
            std::lock_guard<std::mutex> guard(windowLifecycleMutex);
            window = nativeWindow;
        }
        if(!window) {
            return;
        }
        __closeWindowOnUI(exitCode);
        // Destroy the webview object on the same thread that owns its native
        // toolkit objects. GTK/Cocoa normally terminate the process from the
        // destroy callback before this line is reached.
        delete window;
        {
            std::lock_guard<std::mutex> guard(windowLifecycleMutex);
            if(nativeWindow == window) {
                nativeWindow = nullptr;
            }
        }
    });

    #if defined(_WIN32)
    FreeConsole();
    #endif
}

bool isSavedStateLoaded() {
  return savedState;
}

bool isMaximized() {
    json output;
    #if defined(__linux__) || defined(__FreeBSD__)
    return gtk_window_is_maximized(GTK_WINDOW(windowHandle)) == 1;
    #elif defined(_WIN32)
    return  IsZoomed(windowHandle) == 1;
    #elif defined(__APPLE__)
    return ((bool (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
        "isZoomed"_sel, NULL);
    #endif
}

void maximize() {
    if(window::isMaximized())
        return;
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_maximize(GTK_WINDOW(windowHandle));
    #elif defined(_WIN32)
    ShowWindow(windowHandle, SW_MAXIMIZE);
    SetForegroundWindow(windowHandle);
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
        "zoom:"_sel, NULL);
    #endif
}

void unmaximize() {
    if(!window::isMaximized())
        return;
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_unmaximize(GTK_WINDOW(windowHandle));
    #elif defined(_WIN32)
    ShowWindow(windowHandle, SW_RESTORE);
    SetForegroundWindow(windowHandle);
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
        "zoom:"_sel, NULL);
    #endif
}

void unminimize() {
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_present(GTK_WINDOW(windowHandle));
    #elif defined(_WIN32)
    ShowWindow(windowHandle, SW_RESTORE);
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
        "deminiaturize:"_sel, NULL);
    #endif
}

bool isMinimized() {
    #if defined(__linux__) || defined(__FreeBSD__)
    return isGtkWindowMinimized;
    #elif defined(_WIN32)
    return IsIconic(windowHandle) == 1;
    #elif defined(__APPLE__)
    return ((bool (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
        "isMiniaturized"_sel, NULL);
    #endif
}

bool isVisible() {
    #if defined(__linux__) || defined(__FreeBSD__)
    return gtk_widget_is_visible(windowHandle) == 1;
    #elif defined(__APPLE__)
    return ((bool (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
        "isVisible"_sel, NULL);
    return true;
    #elif defined(_WIN32)
    return IsWindowVisible(windowHandle) == 1;
    #endif
}

void show() {
    if(window::isVisible())
        return;
    
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_widget_show(windowHandle);
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, bool))objc_msgSend)((id) windowHandle,
                "setIsVisible:"_sel, true);
    #elif defined(_WIN32)
    ShowWindow(windowHandle, SW_SHOW);
    SetWindowPos(windowHandle, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    if (!SetForegroundWindow(windowHandle)) {
        FLASHWINFO fi;
        fi.cbSize = sizeof(FLASHWINFO);
        fi.hwnd = windowHandle;
        fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        fi.uCount = 0; // Flash indefinitely until the window comes to the foreground
        fi.dwTimeout = 0;
        FlashWindowEx(&fi);
    }

    if (__isFakeHidden())
        __undoFakeHidden();
    #endif

    window::handlers::windowStateChange(WEBVIEW_WINDOW_SHOW);
}

void hide() {
    if(!window::isVisible())
        return;
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_widget_hide(windowHandle);
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, bool))objc_msgSend)((id) windowHandle,
                "setIsVisible:"_sel, false);
    #elif defined(_WIN32)
    ShowWindow(windowHandle, SW_HIDE);
    #endif

    window::handlers::windowStateChange(WEBVIEW_WINDOW_HIDE);
}

void focus() {
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_present(GTK_WINDOW(windowHandle));
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, BOOL))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)("NSApplication"_cls, "sharedApplication"_sel),
        "activateIgnoringOtherApps:"_sel, 1);
    if(window::isMinimized()) {
        window::unminimize();
    }
    else {
        ((void (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
                "makeKeyAndOrderFront:"_sel, NULL);
    }
    #elif defined(_WIN32)
    SetForegroundWindow(windowHandle);
    #endif
}

bool isFullScreen() {
    json output;
    #if defined(__linux__) || defined(__FreeBSD__)
    return isGtkWindowFullScreen;
    #elif defined(__APPLE__)
    unsigned long windowStyleMask = ((unsigned long (*)(id, SEL))objc_msgSend)(
        (id) windowHandle, "styleMask"_sel);
    return (windowStyleMask & NSWindowStyleMaskFullScreen) == NSWindowStyleMaskFullScreen;
    #elif defined(_WIN32)
    return isWinWindowFullScreen;
    #endif
}

void setFullScreen() {
    if(window::isFullScreen())
        return;
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_fullscreen(GTK_WINDOW(windowHandle));
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
            "toggleFullScreen:"_sel, NULL);
    #elif defined(_WIN32)
    savedStyle = GetWindowLong(windowHandle, GWL_STYLE);
    savedStyleX = GetWindowLong(windowHandle, GWL_EXSTYLE);
    GetWindowRect(windowHandle, &savedRect);

    MONITORINFO monitor_info;
    DWORD newStyle = savedStyle & ~(WS_CAPTION | WS_THICKFRAME);
    DWORD newStyleX = savedStyleX & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                        WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLong(windowHandle, GWL_STYLE, newStyle);
    SetWindowLong(windowHandle, GWL_EXSTYLE, newStyleX);
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfo(MonitorFromWindow(windowHandle, MONITOR_DEFAULTTONEAREST),
                &monitor_info);
    RECT r;
    r.left = monitor_info.rcMonitor.left;
    r.top = monitor_info.rcMonitor.top;
    r.right = monitor_info.rcMonitor.right;
    r.bottom = monitor_info.rcMonitor.bottom;
    SetWindowPos(windowHandle, NULL, r.left, r.top, r.right - r.left,
                r.bottom - r.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    isWinWindowFullScreen = true;
    window::handlers::windowStateChange(WEBVIEW_WINDOW_FULLSCREEN);
    #endif
}

void exitFullScreen() {
    if(!window::isFullScreen())
        return;
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_unfullscreen(GTK_WINDOW(windowHandle));
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
            "toggleFullScreen:"_sel, NULL);
    #elif defined(_WIN32)
    SetWindowLong(windowHandle, GWL_STYLE, savedStyle);
    SetWindowLong(windowHandle, GWL_EXSTYLE, savedStyleX);
    SetWindowPos(windowHandle, NULL, savedRect.left, savedRect.top, savedRect.right - savedRect.left,
                savedRect.bottom - savedRect.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    isWinWindowFullScreen = false;
    window::handlers::windowStateChange(WEBVIEW_WINDOW_UNFULLSCREEN);
    #endif
}

bool setIcon(const string &iconFile) {
    if(!windowHandle) {
        return false;
    }
    fs::FileReaderResult fileReaderResult = resources::getFile(iconFile);
    if(fileReaderResult.status != errors::NE_ST_OK || fileReaderResult.data.empty()) {
        return false;
    }

    string iconDataStr = fileReaderResult.data;
    #if defined(__linux__) || defined(__FreeBSD__)
    GError *error = nullptr;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    if(!loader) {
        return false;
    }

    const auto *iconData = reinterpret_cast<const guchar *>(iconDataStr.data());
    gboolean written = gdk_pixbuf_loader_write(
        loader, iconData, iconDataStr.size(), &error);
    gboolean closed = written && gdk_pixbuf_loader_close(loader, &error);
    GdkPixbuf *icon = closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
    bool valid = icon != nullptr;
    if(valid) {
        // Wayland compositors ignore _NET_WM_ICON and resolve the window icon
        // through the xdg_toplevel app_id -> .desktop entry and hicolor theme.
        string desktopAppId = __resolveDesktopAppId();
        string dataHome = sago::getDataHome();
        if(!dataHome.empty() && !desktopAppId.empty()) {
            std::error_code ec;
            string iconThemeDir = dataHome + "/icons";
            string appsIconDir = iconThemeDir + "/hicolor/256x256/apps";
            filesystem::create_directories(appsIconDir, ec);
            string iconPath = appsIconDir + "/" + desktopAppId + ".png";
            gdk_pixbuf_save(icon, iconPath.c_str(), "png", nullptr, nullptr);

            string appId = settings::getAppId();
            if(!appId.empty() && appId != desktopAppId) {
                string altIconPath = appsIconDir + "/" + appId + ".png";
                gdk_pixbuf_save(icon, altIconPath.c_str(), "png", nullptr, nullptr);
            }

            GtkIconTheme *theme = gtk_icon_theme_get_default();
            if(theme) {
                gtk_icon_theme_append_search_path(theme, iconThemeDir.c_str());
            }

            if(!__hasDesktopEntry(desktopAppId)) {
                string desktopDir = dataHome + "/applications";
                filesystem::create_directories(desktopDir, ec);
                string desktopFilePath = desktopDir + "/" + desktopAppId + ".desktop";
                string appTitle = windowProps.title.empty() ? "WeekBox" : windowProps.title;
                string desktopContent =
                    "[Desktop Entry]\n"
                    "Type=Application\n"
                    "Name=" + appTitle + "\n"
                    "Icon=" + iconPath + "\n"
                    "StartupWMClass=" + desktopAppId + "\n"
                    "NoDisplay=true\n";
                fs::FileWriterOptions writerOpts = { desktopFilePath, desktopContent };
                fs::writeFile(writerOpts);
            }

            gtk_window_set_default_icon_name(desktopAppId.c_str());
            gtk_window_set_icon_name(GTK_WINDOW(windowHandle), desktopAppId.c_str());
        }

        gtk_window_set_default_icon(icon);
        gtk_window_set_icon(GTK_WINDOW(windowHandle), icon);
    }
    if(error) {
        g_error_free(error);
    }
    g_object_unref(loader);
    return valid;

    #elif defined(__APPLE__)
    const char *iconData = iconDataStr.data();
    id nsIconData = ((id (*)(id, SEL, const char*, int))objc_msgSend)("NSData"_cls,
                "dataWithBytes:length:"_sel, iconData, iconDataStr.length());
    if(!nsIconData) {
        return false;
    }

    id icon = ((id (*)(id, SEL))objc_msgSend)("NSImage"_cls, "alloc"_sel);
    icon = ((id (*)(id, SEL, id))objc_msgSend)(icon, "initWithData:"_sel, nsIconData);
    if(!icon) {
        return false;
    }
    ((void (*)(id, SEL, id))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)("NSApplication"_cls,
                                "sharedApplication"_sel),
                "setApplicationIconImage:"_sel, icon);
    // NSApplication retains the image; release the manually allocated object
    // to keep repeated window.setIcon calls leak-free without ARC.
    ((void (*)(id, SEL))objc_msgSend)(icon, "release"_sel);
    return true;

    #elif defined(_WIN32)
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if(GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL) != Gdiplus::Ok) {
        return false;
    }

    const auto *iconData = reinterpret_cast<const BYTE *>(iconDataStr.data());
    IStream *pStream = SHCreateMemStream(iconData, iconDataStr.size());
    if(!pStream) {
        GdiplusShutdown(gdiplusToken);
        return false;
    }

    Gdiplus::Bitmap *bitmap = Gdiplus::Bitmap::FromStream(pStream);
    HICON icon = nullptr;
    bool valid = bitmap && bitmap->GetLastStatus() == Gdiplus::Ok &&
        bitmap->GetHICON(&icon) == Gdiplus::Ok && icon != nullptr;
    if(valid) {
        SendMessage(windowHandle, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessage(windowHandle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        if(windowIcon && windowIcon != icon) {
            DestroyIcon(windowIcon);
        }
        windowIcon = icon;
    }
    else if(icon) {
        DestroyIcon(icon);
    }

    delete bitmap;
    pStream->Release();
    GdiplusShutdown(gdiplusToken);
    return valid;
    #endif
}

void setBadge(int count) {
    if(count < 0) {
        count = 0;
    }

    #if defined(__linux__) || defined(__FreeBSD__)
    (void)count;

    #elif defined(__APPLE__)
    id app = ((id(*)(id, SEL))objc_msgSend)("NSApplication"_cls,
                                            "sharedApplication"_sel);
    id dockTile = ((id(*)(id, SEL))objc_msgSend)(app, "dockTile"_sel);
    id label = nullptr;

    if(count > 0) {
        string badge = to_string(count);
        label = ((id(*)(id, SEL, const char*))objc_msgSend)(
            "NSString"_cls, "stringWithUTF8String:"_sel, badge.c_str());
    }

    ((void(*)(id, SEL, id))objc_msgSend)(dockTile, "setBadgeLabel:"_sel, label);

    #elif defined(_WIN32)
    Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
    if(FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&taskbar))) ||
        FAILED(taskbar->HrInit()))
        return;

    if(count == 0) {
        taskbar->SetOverlayIcon(windowHandle, nullptr, L"");
        return;
    }

    string badge = count > 99 ? "99+" : to_string(count);

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    HICON icon = nullptr;
    {
        Bitmap bitmap(32, 32, PixelFormat32bppARGB);
        Graphics graphics(&bitmap);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.Clear(Color(0, 0, 0, 0));

        SolidBrush badgeBrush(Color(255, 255, 193, 7));
        graphics.FillEllipse(&badgeBrush, 0, 0, 31, 31);

        FontFamily fontFamily(L"Arial");
        Font font(&fontFamily, badge.length() > 2 ? 11.0f : 16.0f, FontStyleBold, UnitPixel);
        SolidBrush textBrush(Color(255, 255, 255, 255));
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);

        wstring wBadge = helpers::str2wstr(badge);
        RectF rect(0.0f, badge.length() > 2 ? 1.0f : 0.0f, 32.0f, 31.0f);
        graphics.DrawString(wBadge.c_str(), -1, &font, rect, &format, &textBrush);
        bitmap.GetHICON(&icon);
    }

    taskbar->SetOverlayIcon(windowHandle, icon, L"badge");

    if(icon) {
        DestroyIcon(icon);
    }
    GdiplusShutdown(gdiplusToken);
    #endif
}

void move(int x, int y) {
    windowProps.x = x;
    windowProps.y = y;
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_move(GTK_WINDOW(windowHandle), x, y);
    #elif defined(__APPLE__)
    auto displayId = CGMainDisplayID();
    int height = CGDisplayPixelsHigh(displayId);
    ((void (*)(id, SEL, CGPoint))objc_msgSend)(
        (id) windowHandle, "setFrameTopLeftPoint:"_sel,
        CGPointMake(x, height - y));
    #elif defined(_WIN32)
    RECT winPos;
    GetWindowRect(windowHandle, &winPos);
    MoveWindow(windowHandle, x, y, winPos.right - winPos.left,
                winPos.bottom - winPos.top, true);
    #endif
}

void beginDragNative() {
	#if defined(__linux__) || defined(__FreeBSD__)
    auto mousePos = computer::getMousePosition();
    gtk_window_begin_move_drag(GTK_WINDOW(windowHandle), 1, mousePos.first, mousePos.second, GDK_CURRENT_TIME);

	#elif defined(_WIN32)
    POINT startPos;
    if (GetCursorPos(&startPos)) {
        ReleaseCapture();
        PostMessage(windowHandle, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
    }

	#elif defined(__APPLE__)
    ((void (*)(id, SEL, id))objc_msgSend)(windowHandle,
        "performWindowDragWithEvent:"_sel, 
        ((id (*)(id, SEL))objc_msgSend)(windowHandle,
        "currentEvent"_sel)
        );
	#endif
}

window::SizeOptions getSize() {
    int width, height = 0;
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_get_size(GTK_WINDOW(windowHandle),
                        &width, &height);
    #elif defined(__APPLE__)
    CGRect frameRect = __getWindowRect();

    width = frameRect.size.width;
    height = frameRect.size.height;

    #elif defined(_WIN32)
    if(IsIconic(windowHandle)) {
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        GetWindowPlacement(windowHandle, &wp);
        width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
    else {
        RECT winPos;
        GetWindowRect(windowHandle, &winPos);
        width = winPos.right - winPos.left;
        height = winPos.bottom - winPos.top;
    }
    #endif
    if(windowProps.useLogicalPixels) {
        double scale = __getScaleFactor();
        width = (int)(width / scale);
        height = (int)(height / scale);
   }

    windowProps.sizeOptions.width = width;
    windowProps.sizeOptions.height = height;
    return windowProps.sizeOptions;
}

pair<int, int> getPosition() {
    int x = windowProps.x, y = windowProps.y;
    #if defined(__linux__) || defined(__FreeBSD__)
    GdkWindow *gdkWin = gtk_widget_get_window(windowHandle);
    if(gdkWin) {
        gdk_window_get_root_origin(gdkWin, &x, &y);
        if(x == 0 && y == 0 && (windowProps.x != 0 || windowProps.y != 0)) {
            x = windowProps.x;
            y = windowProps.y;
        }
    }

    #elif defined(__APPLE__)
    CGRect frameRect = __getWindowRect();
    auto displayId = CGMainDisplayID();
    int height = CGDisplayPixelsHigh(displayId);

    x = frameRect.origin.x;
    y = height - frameRect.origin.y - frameRect.size.height;

    #elif defined(_WIN32)
    if(IsIconic(windowHandle)) {
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        GetWindowPlacement(windowHandle, &wp);
        x = wp.rcNormalPosition.left;
        y = wp.rcNormalPosition.top;
    }
    else {
        RECT winPos;
        GetWindowRect(windowHandle, &winPos);
        x = winPos.left;
        y = winPos.top;
    }
    #endif

    return make_pair(x, y);
}

void center(bool useConfigSizes = false) {
    pair<int, int> pos = __getCenterPos(useConfigSizes);
    window::move(pos.first, pos.second);
}

void setAlwaysOnTop(bool onTop) {
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_set_keep_above(GTK_WINDOW(windowHandle), onTop);
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, int))objc_msgSend)((id) windowHandle,
            "setLevel:"_sel, onTop ? NSFloatingWindowLevel : NSBaseWindowLevel);
    #elif defined(_WIN32)
    SetWindowPos(windowHandle, onTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    #endif
}

void setBorderless(bool borderless) {
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_set_decorated(GTK_WINDOW(windowHandle), !borderless);
    #elif defined(__APPLE__)
    unsigned long windowStyleMask = ((unsigned long (*)(id, SEL))objc_msgSend)(
        (id) windowHandle, "styleMask"_sel);
    windowStyleMask = borderless ? (windowStyleMask & ~NSWindowStyleMaskTitled) : 
                    (windowStyleMask | NSWindowStyleMaskTitled);
    ((void (*)(id, SEL, int))objc_msgSend)((id) windowHandle,
            "setStyleMask:"_sel, windowStyleMask);
    #elif defined(_WIN32)
    DWORD currentStyle = GetWindowLong(windowHandle, GWL_STYLE);
    currentStyle = borderless ? (currentStyle & ~(WS_CAPTION | WS_THICKFRAME)) : 
                    (currentStyle | (WS_CAPTION | WS_THICKFRAME));
    SetWindowLong(windowHandle, GWL_STYLE, currentStyle);
    SetWindowPos(windowHandle, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    #endif
}

void setSkipTaskbar(bool skip) {
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(windowHandle), skip);
    GdkWindow *gdkWin = gtk_widget_get_window(windowHandle);
    if(gdkWin) {
        gdk_window_set_skip_taskbar_hint(gdkWin, skip);
    }
    #elif defined(__APPLE__)
    id app = ((id(*)(id, SEL))objc_msgSend)("NSApplication"_cls,
                                            "sharedApplication"_sel);
    ((void (*)(id, SEL, long))objc_msgSend)(
        app, "setActivationPolicy:"_sel, skip ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular);
    #elif defined(_WIN32)
    Microsoft::WRL::ComPtr<ITaskbarList> taskbar;
    if(FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&taskbar))) ||
        FAILED(taskbar->HrInit()))
        return;
    if(skip)
        taskbar->DeleteTab(windowHandle);
    else
        taskbar->AddTab(windowHandle);
    #endif
}

bool snapshot(const string &filename) {
    #if defined(__linux__) || defined(__FreeBSD__)
    int width, height, x, y;
    GdkWindow *window = gtk_widget_get_window(windowHandle);
    gdk_window_get_geometry(window, &x, &y, &width, &height);
    GdkPixbuf *screenshot = gdk_pixbuf_get_from_window(window, x, y, width, height);
    return gdk_pixbuf_save(screenshot, filename.c_str(), "png", nullptr, nullptr);

    #elif defined(__APPLE__)
    CGRect frameRect = __getWindowRect();
    CGRect clientRect =
            ((CGRect (*)(id, SEL, CGRect))objc_msgSend)(windowHandle, "contentRectForFrameRect:"_sel, frameRect);
    clientRect.origin.y +=  frameRect.size.height - clientRect.size.height;

    long winId = ((long(*)(id, SEL))objc_msgSend)(windowHandle, "windowNumber"_sel);
    
    CGImageRef imgRef = nil;

    #if defined(__APPLE__) && MAC_OS_X_VERSION_MIN_REQUIRED >= 120300

    // Modern ScreenCaptureKit API (macOS 12.3+)
    __block CGImageRef screenshotImage = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:
        [SCWindow windowWithWindowID:winId]];
    
    SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
    config.scalesToFit = NO;
    
    [SCScreenshotManager captureImageWithFilter:filter
                                  configuration:config
                              completionHandler:^(CGImageRef capturedImage, NSError *error) {
        if (error == nil && capturedImage != NULL) {
            
            screenshotImage = CGImageCreateWithImageInRect(capturedImage, clientRect);
        }
        dispatch_semaphore_signal(semaphore);  
    }];
    
    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC));
    imgRef = screenshotImage;

    #else
        // Fallback for macOS < 12.3
        imgRef = CGWindowListCreateImage(clientRect, kCGWindowListOptionIncludingWindow, winId, kCGWindowImageBoundsIgnoreFraming);
    #endif

      
    id screenshot =
        ((id (*)(id, SEL))objc_msgSend)("NSBitmapImageRep"_cls, "alloc"_sel);
    ((void (*)(id, SEL, CGImageRef))objc_msgSend)(screenshot, "initWithCGImage:"_sel, imgRef);
    id screenshotData =
        ((id (*)(id, SEL, int, id))objc_msgSend)(screenshot, "representationUsingType:properties:"_sel, NSPNGFileType, nullptr);
    bool status = ((bool (*)(id, SEL, id, bool))objc_msgSend)(screenshotData, "writeToFile:atomically:"_sel, 
            ((id(*)(id, SEL, const char *))objc_msgSend)("NSString"_cls, "stringWithUTF8String:"_sel, filename.c_str())
    , true);
    
    return status;

    #elif defined(_WIN32)
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    RECT rect;
    GetClientRect(windowHandle, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    HDC hdcWindow = GetDC(windowHandle);
    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcWindow, width, height);

    SelectObject(hdcMem, hBitmap);

    if(!PrintWindow(windowHandle, hdcMem, PW_CLIENTONLY | PW_RENDERFULLCONTENT)) return false;

    Bitmap *bitmap = Bitmap::FromHBITMAP(hBitmap, nullptr);
    CLSID clsid;
    if(!__getEncoderClsid(L"image/png", &clsid)) return false;

    bool status = bitmap->Save(CONVSTR(filename).c_str(), &clsid, nullptr) == Gdiplus::Ok;

    DeleteDC(hdcMem);
    ReleaseDC(windowHandle, hdcWindow);
    DeleteObject(hBitmap);
    GdiplusShutdown(gdiplusToken);

    return status;
    #endif
}

void setMainMenu(const json &menu) {
    #if defined(_WIN32)
    SendMessage(windowHandle, WM_WINDOW_DELETE_MENU_REFS, 0, 0);
    HMENU prevMenu = windowMenu;
    windowMenuItemId = ID_MENU_FIRST;
    windowMenu = __createMenu(menu, true);
    SetMenu(windowHandle, windowMenu);

    if(prevMenu) {
        DestroyMenu(prevMenu);
    }

    #elif defined(__APPLE__)
    id app = ((id(*)(id, SEL))objc_msgSend)("NSApplication"_cls,
                                            "sharedApplication"_sel);
    ((void (*)(id, SEL, id))objc_msgSend)(app, "mainMenu"_sel, nullptr);
    ((void (*)(id, SEL, id))objc_msgSend)(app, "setMainMenu:"_sel, __createMenu(menu));

    #elif defined(__linux__) || defined(__FreeBSD__)
    if(menuContainer) {
        gtk_widget_destroy(menuContainer);
    }

    menuContainer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkMenuShell *windowMenu = __createMenu(menu, true);
    GtkWidget *parentContainer = gtk_bin_get_child(GTK_BIN(windowHandle));

    gtk_box_pack_start(GTK_BOX(parentContainer), menuContainer, false, false, 0);
    gtk_box_reorder_child(GTK_BOX(parentContainer), menuContainer, 0);
    gtk_box_pack_start(GTK_BOX(menuContainer), GTK_WIDGET(windowMenu), false, false, 0);
    gtk_widget_show_all(menuContainer);

    #endif
}

bool init(const json &windowOptions) {
    json output;
    {
        std::lock_guard<std::mutex> guard(windowLifecycleMutex);
        uiThreadId = std::this_thread::get_id();
    }

    windowProps.sizeOptions = __jsonToSizeOptions(windowOptions, true);

    if(helpers::hasField(windowOptions, "x"))
        windowProps.x = windowOptions["x"].get<int>();

    if(helpers::hasField(windowOptions, "useLogicalPixels")) {
        windowProps.useLogicalPixels = windowOptions["useLogicalPixels"].get<bool>();
    }


    if(helpers::hasField(windowOptions, "y"))
        windowProps.y = windowOptions["y"].get<int>();

    if(helpers::hasField(windowOptions, "fullScreen"))
        windowProps.fullScreen = windowOptions["fullScreen"].get<bool>();

    if(helpers::hasField(windowOptions, "alwaysOnTop"))
        windowProps.alwaysOnTop = windowOptions["alwaysOnTop"].get<bool>();

    if(helpers::hasField(windowOptions, "title"))
        windowProps.title = windowOptions["title"].get<string>();

    if(helpers::hasField(windowOptions, "url"))
        windowProps.url = windowOptions["url"].get<string>();

    if(helpers::hasField(windowOptions, "icon"))
        windowProps.icon = windowOptions["icon"].get<string>();

    if(helpers::hasField(windowOptions, "extendUserAgentWith"))
        windowProps.extendUserAgentWith = windowOptions["extendUserAgentWith"].get<string>();

    if(helpers::hasField(windowOptions, "injectScript"))
        windowProps.injectScript = windowOptions["injectScript"].get<string>();

    if(helpers::hasField(windowOptions, "enableInspector"))
        windowProps.enableInspector = windowOptions["enableInspector"].get<bool>();

    if(helpers::hasField(windowOptions, "openInspectorOnStartup"))
        windowProps.openInspectorOnStartup = windowOptions["openInspectorOnStartup"].get<bool>();

    if(helpers::hasField(windowOptions, "borderless"))
        windowProps.borderless = windowOptions["borderless"].get<bool>();

    if(helpers::hasField(windowOptions, "maximize"))
        windowProps.maximize = windowOptions["maximize"].get<bool>();

    if(helpers::hasField(windowOptions, "hidden"))
        windowProps.hidden = windowOptions["hidden"].get<bool>();

    if(helpers::hasField(windowOptions, "center"))
        windowProps.center = windowOptions["center"].get<bool>();

    if(helpers::hasField(windowOptions, "transparent"))
        windowProps.transparent = windowOptions["transparent"].get<bool>();

    if(helpers::hasField(windowOptions, "exitProcessOnClose"))
        windowProps.exitProcessOnClose = windowOptions["exitProcessOnClose"].get<bool>();

    if(helpers::hasField(windowOptions, "useSavedState"))
        windowProps.useSavedState = windowOptions["useSavedState"].get<bool>();

    if(helpers::hasField(windowOptions, "injectGlobals"))
        windowProps.injectGlobals = windowOptions["injectGlobals"].get<bool>();

    if(helpers::hasField(windowOptions, "injectClientLibrary"))
        windowProps.injectClientLibrary = windowOptions["injectClientLibrary"].get<bool>();

    if(helpers::hasField(windowOptions, "webviewArgs"))
        windowProps.webviewArgs = windowOptions["webviewArgs"].get<string>();

    if(helpers::hasField(windowOptions, "skipTaskbar"))
        windowProps.skipTaskbar = windowOptions["skipTaskbar"].get<bool>();

    if(helpers::hasField(windowOptions, "emitDropEvents"))
        windowProps.emitDropEvents = windowOptions["emitDropEvents"].get<bool>();

    if(helpers::hasField(windowOptions, "newWindowPolicy")) {
        string policy = windowOptions["newWindowPolicy"].get<string>();
        if(policy == "browser")
            windowProps.newWindowPolicy = window::NewWindowPolicyBrowser;
        else if(policy == "custom")
            windowProps.newWindowPolicy = window::NewWindowPolicyCustom;
            
    }

    if(!__createWindow()) {
        return false;
    }

    nativeWindow->run();
    return true;
}

NEU_W_HANDLE getHandle() {
    return windowHandle;
}

namespace controllers {

json setTitle(const json &input) {
    json output;
    string title = "";
    if(helpers::hasField(input, "title")) {
        title = input["title"].get<string>();
    }
    nativeWindow->set_title(title);
    output["success"] = true;
    return output;
}

json getTitle(const json &input) {
    json output;
    output["returnValue"] = nativeWindow->get_title();
    output["success"] = true;
    return output;
}

json maximize(const json &input) {
    json output;
    window::maximize();
    output["success"] = true;
    return output;
}

json unmaximize(const json &input) {
    json output;
    window::unmaximize();
    output["success"] = true;
    return output;
}

json isMaximized(const json &input) {
    json output;
    output["returnValue"] = window::isMaximized();
    output["success"] = true;
    return output;
}

json minimize(const json &input) {
    json output;
    #if defined(__linux__) || defined(__FreeBSD__)
    gtk_window_iconify(GTK_WINDOW(windowHandle));
    #elif defined(_WIN32)
    ShowWindow(windowHandle, SW_MINIMIZE);
    #elif defined(__APPLE__)
    ((void (*)(id, SEL, id))objc_msgSend)((id) windowHandle,
        "miniaturize:"_sel, NULL);
    #endif
    output["success"] = true;
    return output;
}

json unminimize(const json &input) {
    json output;
    window::unminimize();
    output["success"] = true;
    return output;
}

json isMinimized(const json &input) {
    json output;
    output["returnValue"] = window::isMinimized();
    output["success"] = true;
    return output;
}

json show(const json &input) {
    json output;
    window::show();
    output["success"] = true;
    return output;
}

json hide(const json &input) {
    json output;
    window::hide();
    output["success"] = true;
    return output;
}

json isVisible(const json &input) {
    json output;
    output["returnValue"] = window::isVisible();
    output["success"] = true;
    return output;
}

json setFullScreen(const json &input) {
    json output;
    window::setFullScreen();
    output["success"] = true;
    return output;
}

json exitFullScreen(const json &input) {
    json output;
    window::exitFullScreen();
    output["success"] = true;
    return output;
}

json isFullScreen(const json &input) {
    json output;
    output["returnValue"] = window::isFullScreen();
    output["success"] = true;
    return output;
}

json focus(const json &input) {
    json output;
    window::focus();
    output["success"] = true;
    return output;
}

json setIcon(const json &input) {
    json output;
    if(!helpers::hasRequiredFields(input, {"icon"})) {
        output["error"] = errors::makeMissingArgErrorPayload("icon");
        return output;
    }
    string icon = input["icon"].get<string>();
    if(!window::setIcon(icon)) {
        output["error"] = errors::makeErrorPayload(errors::NE_WI_UNBSICO, icon);
        return output;
    }
    output["success"] = true;
    return output;
}

json setBadge(const json &input) {
    json output;
    if(!helpers::hasRequiredFields(input, {"count"}) ||
        !input["count"].is_number_integer()) {
        output["error"] = errors::makeMissingArgErrorPayload("count");
        return output;
    }
    int count = input["count"].get<int>();
    if(count < 0) {
        output["error"] = errors::makeMissingArgErrorPayload("count");
        return output;
    }
    window::setBadge(count);
    output["success"] = true;
    return output;
}

json move(const json &input) {
    json output;
    const auto missingRequiredField = helpers::missingRequiredField(input, {"x", "y"});
    if(missingRequiredField) {
        output["error"] = errors::makeMissingArgErrorPayload(missingRequiredField.value());
        return output;
    }
    int x = input["x"].get<int>();
    int y = input["y"].get<int>();
    window::move(x, y);
    output["success"] = true;
    return output;
}

json center(const json &input) {
    json output;
    window::center();
    output["success"] = true;
    return output;
}

json setSize(const json &input) {
    json output;
    windowProps.sizeOptions = __jsonToSizeOptions(input);

    int width = windowProps.sizeOptions.width;
    int height = windowProps.sizeOptions.height;

    if(windowProps.useLogicalPixels) {
        double scale = __getScaleFactor();
        if(width > 0)  width  = (int)(width  * scale);
        if(height > 0) height = (int)(height * scale);
    }

    nativeWindow->set_size(
        width,
        height,
        windowProps.sizeOptions.minWidth,
        windowProps.sizeOptions.minHeight,
        windowProps.sizeOptions.maxWidth,
        windowProps.sizeOptions.maxHeight,
        windowProps.sizeOptions.resizable
    );

    output["success"] = true;
    return output;
}


json getSize(const json &input) {
    json output;
    windowProps.sizeOptions = window::getSize();

    output["returnValue"] = __sizeOptionsToJson(windowProps.sizeOptions);
    output["success"] = true;
    return output;
}

json setAlwaysOnTop(const json &input) {
    json output;
    bool onTop = true;
    if(helpers::hasField(input, "onTop")) {
        onTop = input["onTop"].get<bool>();
    }
    window::setAlwaysOnTop(onTop);
    output["success"] = true;
    return output;
}

json setBorderless(const json &input) {
    json output;
    bool borderless = true;
    if(helpers::hasField(input, "borderless")) {
        borderless = input["borderless"].get<bool>();
    }
    window::setBorderless(borderless);
    output["success"] = true;
    return output;
}

json getPosition(const json &input) {
    json output;
    json posRes;
    pair<int, int> pos = window::getPosition();
    posRes["x"] = pos.first;
    posRes["y"] = pos.second;
    output["returnValue"] = posRes;
    output["success"] = true;
    return output;
}

json snapshot(const json &input) {
    json output;
    if (!helpers::hasRequiredFields(input, {"path"})) {
        output["error"] = errors::makeMissingArgErrorPayload("path");
        return output;
    }

    string imageFile = input["path"].get<string>();
    if(window::snapshot(imageFile)) {
        output["success"] = true;
    }
    else {
        output["error"] = errors::makeErrorPayload(errors::NE_WI_UNBSWSR, imageFile);
    }
    return output;
}

json setMainMenu(const json &input) {
    json output;

    window::setMainMenu(input);

    output["success"] = true;
    return output;
}


json beginDrag(const json &input) {
    json output;

    #if defined(_WIN32) || defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
    // The router already entered the UI thread for window.* methods.
    window::beginDragNative();
    #endif

    output["success"] = true;
    return output;
}

json print(const json &input) {
    json output;

    #if defined(__linux__) || defined(__FreeBSD__)
    void *dlib = nativeWindow->dl();
    webkit_print_operation_new_func webkit_print_operation_new = (webkit_print_operation_new_func)(dlsym(dlib, "webkit_print_operation_new"));
    webkit_print_operation_run_dialog_func webkit_print_operation_run_dialog = (webkit_print_operation_run_dialog_func)(dlsym(dlib, "webkit_print_operation_run_dialog"));

    WebKitPrintOperation *printOp = webkit_print_operation_new((WebKitWebView*)(nativeWindow->wv()));
    webkit_print_operation_run_dialog((WebKitPrintOperation*)printOp, GTK_WINDOW(windowHandle));

    #elif defined(__APPLE__)
    id sharedPrintInfo = ((id(*)(id, SEL))objc_msgSend)("NSPrintInfo"_cls,
                                            "sharedPrintInfo"_sel);
    id printInfo = ((id(*)(id, SEL, id))objc_msgSend)((id)nativeWindow->wv(),
                                            "printOperationWithPrintInfo:"_sel, sharedPrintInfo);
    ((void(*)(id, SEL, id, id, SEL, void(*)))objc_msgSend)(printInfo,
        "runOperationModalForWindow:delegate:didRunSelector:contextInfo:"_sel, windowHandle, nullptr, nullptr, nullptr);  
    #elif defined(_WIN32)
    ICoreWebView2 *webview = (ICoreWebView2*)nativeWindow->wv();
    webview->ExecuteScript(L"window.print()", nullptr);

    #endif
    output["success"] = true;
    return output;
}


} // namespace controllers

} // namespace window
