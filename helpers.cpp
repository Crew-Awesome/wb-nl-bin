#include <vector>
#include <string>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <ctype.h>
#include <random>
#include <optional>
#include <regex>

#include "helpers.h"
#include "lib/json/json.hpp"

#if defined(__linux__)
#include <sys/prctl.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <cstring>
#elif defined(__APPLE__)
#include <pthread.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <cstdlib>
#import <Foundation/Foundation.h>
#elif defined(__FreeBSD__)
#include <pthread.h>
#include <pthread_np.h>
#include <unistd.h>
#include <limits.h>
#elif defined(_WIN32)
#include <string>
#include <windows.h>
#endif

using namespace std;
using json = nlohmann::json;

namespace helpers {

vector<string> split(const string &s, char delim, unsigned int stopAfter) {
    stringstream ss(s);
    string item;
    vector<string> tokens;
    while (getline(ss, item, delim)) {
        tokens.push_back(item);
        if(stopAfter !=-1 && tokens.size() == stopAfter - 1) {
            delim = '\n';
        }
    }
    return tokens;
}

vector<string> splitTwo(const string &s, char delim) {
    return split(s, delim, 2);
}

string trimRight(string &s) {
    s.erase(find_if(s.rbegin(), s.rend(),
        [](unsigned char c) {
            return !isspace(c);
    }).base(), s.end());
    return s;
}

string generateToken() {
    static const string characters =
        "-_"
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    const int length = 96;

    string token;
    token.reserve(length);

    random_device rd;
    mt19937 generator(rd()); 
    uniform_int_distribution<int> distribution(0, characters.size() - 1);

    for (int i = 0; i < length; ++i) {
        if (i == 47) {
            token += ".";
        }
        token += characters[distribution(generator)];
    }
    
    return token;
}

/*
* https://stackoverflow.com/a/14530993 - mini url decoder
*/
void urldecode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a')
                a -= 'a' - 'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a')
                b -= 'a' - 'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        }
        else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

char* cStrCopy(const string &str) {
    char *text = new char[str.size() + 1];
    copy(str.begin(), str.end(), text);
    text[str.size()] = '\0';
    // delete[] text from the initiator
    return text;
}

bool hasRequiredFields(const json &input, const vector<string> &keys) {
    for(const string &key: keys) {
        if(!helpers::hasField(input, key)) {
            return false;
        }
    }
    return true;
}

optional<string> missingRequiredField(const json &input, const vector<string> &keys) {
    for(const string &key: keys) {
        if(!helpers::hasField(input, key)) {
            return key;
        }
    }
    return {};
}

bool hasField(const json &input, const string &key) {
    return input.contains(key) && !input[key].is_null();
}

vector<string> getModes() {
    return {"window", "browser", "cloud", "chrome"};
}

string appModeToStr(settings::AppMode mode) {
    switch(mode) {
        case settings::AppModeWindow:
            return "window";
        case settings::AppModeBrowser:
            return "browser";
        case settings::AppModeCloud:
            return "cloud";
        case settings::AppModeChrome:
            return "chrome";
        default:
            return "invalid";
    }
}

string getCurrentTimestamp() {
    auto now = chrono::system_clock::now();
    auto nowTimeT = chrono::system_clock::to_time_t(now);
    auto nowMs = chrono::duration_cast<chrono::milliseconds>(
                      now.time_since_epoch())
                      .count() % 1000;

    ostringstream oss;
    oss << put_time(gmtime(&nowTimeT), "%Y-%m-%dT%H:%M:%S")
        << "." << setfill('0') << setw(3) << nowMs << "Z";

    return oss.str();
}

string normalizePath(string &path) {
    #if defined(_WIN32)
    replace(path.begin(), path.end(), '\\', '/');
    #endif
    if(path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

string unNormalizePath(string &path) {
    #if defined(_WIN32)
    replace(path.begin(), path.end(), '/', '\\');
    #endif
    return path;
}

string jsonToString(const json &obj) {
    return obj.dump(-1, ' ', false, json::error_handler_t::replace);
}

#if defined(_WIN32)
wstring str2wstr(const string &str) {
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    wstring ret(len, '\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), (LPWSTR)ret.data(), (int)ret.size());
    return ret;
}

string wstr2str(const wstring &str) {
    int len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0, nullptr, nullptr);
    string ret(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.size(), (LPSTR)ret.data(), (int)ret.size(), nullptr, nullptr);
    return ret;
}

string wcstr2str(const wchar_t* wstr) {
    int count = WideCharToMultiByte(CP_UTF8, 0, wstr, wcslen(wstr), NULL, 0, NULL, NULL);
    string str(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], count, NULL, NULL);
    return str;
}
#endif

bool globalMatch(const string &value, const string &pattern) {
    string regexStr = "^";
    for(const char c: pattern) {
        switch(c) {
            case '*': regexStr += ".*"; break;
            case '?': regexStr += "."; break;
            case '.': case '\\': case '+': case '(': case ')':
            case '[': case ']': case '{': case '}': case '|':
            case '^': case '$': case '#': regexStr += "\\"; [[fallthrough]];
            default: regexStr += c; break;
        }
    }
    regexStr += "$";
    try {
        return regex_match(value, regex(regexStr));
    }
    catch(const regex_error &) {
        return false;
    }
}

vector<string> tokenizeCommand(const string &command) {
    vector<string> tokens;
    string current;
    bool inSingle = false;
    bool inDouble = false;

    auto flushToken = [&](void) {
        if(!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };

    for(size_t i = 0; i < command.size(); i++) {
        unsigned char c = static_cast<unsigned char>(command[i]);
        if(!inSingle && !inDouble) {
            if(isspace(c)) {
                flushToken();
            }
            else if(c == '\'') {
                inSingle = true;
            }
            else if(c == '"') {
                inDouble = true;
            }
            else if(strchr(";|&><$`(){}\\\n\r", c) != nullptr) {
                tokens.clear();
                return tokens;
            }
            else {
                current += static_cast<char>(c);
            }
        }
        else if(inSingle) {
            if(c == '\'') {
                inSingle = false;
            }
            else {
                current += static_cast<char>(c);
            }
        }
        else {
            if(c == '"') {
                inDouble = false;
            }
            else {
                current += static_cast<char>(c);
            }
        }
    }
    if(inSingle || inDouble) {
        tokens.clear();
        return tokens;
    }
    flushToken();
    return tokens;
}

void setThreadName(const string &name) {
    if(name.empty()) {
        return;
    }
    #if defined(__linux__)
    string shortName = name.substr(0, 15);
    prctl(PR_SET_NAME, shortName.c_str(), 0, 0, 0);
    pthread_setname_np(pthread_self(), shortName.c_str());
    #elif defined(__APPLE__)
    string shortName = name.substr(0, 63);
    pthread_setname_np(shortName.c_str());
    #elif defined(__FreeBSD__)
    string shortName = name.substr(0, 15);
    pthread_set_name_np(pthread_self(), shortName.c_str());
    #elif defined(_WIN32)
    using SetThreadDescriptionFunc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static auto setThreadDesc = reinterpret_cast<SetThreadDescriptionFunc>(
        GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "SetThreadDescription"));
    if(setThreadDesc) {
        wstring wname = str2wstr(name);
        setThreadDesc(GetCurrentThread(), wname.c_str());
    }
    #endif
}

void setProcessName(const string &name, int argc, char **argv) {
    if(name.empty()) {
        return;
    }
    setThreadName(name);

    #if defined(__linux__)
    if(argc > 0 && argv && argv[0]) {
        string currentArg0(argv[0]);
        string lowerArg0 = currentArg0;
        transform(lowerArg0.begin(), lowerArg0.end(), lowerArg0.begin(),
            [](unsigned char c) { return static_cast<char>(tolower(c)); });
        if(lowerArg0.find("neutralino") != string::npos) {
            size_t availLen = strlen(argv[0]);
            if(availLen > 0) {
                memset(argv[0], 0, availLen);
                size_t copyLen = min(name.size(), availLen);
                memcpy(argv[0], name.data(), copyLen);
            }
        }
    }
    #elif defined(__APPLE__)
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSString *nsName = [NSString stringWithUTF8String:name.c_str()];
        [[NSProcessInfo processInfo] setProcessName:nsName];
    }
    #elif defined(__FreeBSD__)
    (void)argc;
    (void)argv;
    setproctitle("-%s", name.c_str());
    #else
    (void)argc;
    (void)argv;
    #endif
}

string getExecutableDir() {
    #if defined(__linux__) || defined(__FreeBSD__)
    char buf[PATH_MAX] = {0};
    #if defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    #else
    ssize_t len = readlink("/proc/curproc/file", buf, sizeof(buf) - 1);
    #endif
    if(len > 0) {
        buf[len] = '\0';
        string fullPath(buf);
        size_t slashPos = fullPath.find_last_of('/');
        if(slashPos != string::npos) {
            string dir = slashPos == 0 ? "/" : fullPath.substr(0, slashPos);
            return normalizePath(dir);
        }
    }
    return "";
    #elif defined(__APPLE__)
    char buf[PATH_MAX] = {0};
    uint32_t size = sizeof(buf);
    if(_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX] = {0};
        const char *pathPtr = realpath(buf, resolved) ? resolved : buf;
        string fullPath(pathPtr);
        size_t slashPos = fullPath.find_last_of('/');
        if(slashPos != string::npos) {
            string dir = slashPos == 0 ? "/" : fullPath.substr(0, slashPos);
            return normalizePath(dir);
        }
    }
    return "";
    #elif defined(_WIN32)
    wchar_t wbuf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
    if(len > 0 && len < MAX_PATH) {
        string fullPath = wstr2str(wstring(wbuf, len));
        fullPath = normalizePath(fullPath);
        size_t slashPos = fullPath.find_last_of('/');
        if(slashPos != string::npos) {
            string dir = fullPath.substr(0, slashPos);
            return normalizePath(dir);
        }
    }
    return "";
    #endif
}

} // namespace helpers
