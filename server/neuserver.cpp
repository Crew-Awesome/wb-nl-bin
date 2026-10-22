#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include "lib/json/json.hpp"
#include "settings.h"
#include "helpers.h"
#include "errors.h"
#include "extensions_loader.h"
#include "server/neuserver.h"
#include "server/router.h"
#include "auth/authbasic.h"
#include "api/debug/debug.h"
#include "api/events/events.h"
#include "api/app/app.h"
#include "api/fs/fs.h"


using namespace std;
using json = nlohmann::json;

typedef websocketpp::server<websocketpp::config::asio> websocketserver;
typedef map<string, websocketpp::connection_hdl> wsclientsMap;
typedef set<websocketpp::connection_hdl, owner_less<websocketpp::connection_hdl>> wsclientsSet;

namespace neuserver {

websocketserver *server;
wsclientsSet appConnections;
wsclientsMap extConnections;
map<websocketpp::connection_hdl,
    websocketpp::frame::opcode::value,
    owner_less<websocketpp::connection_hdl>> wsMode;
mutex connectionMutex;
mutex ioLifecycleMutex;
bool ioStopRequested = false;

// Native calls are deliberately decoupled from the WebSocket IO loop. The
// limits are part of the transport contract: they provide bounded memory and
// TCP backpressure instead of letting promises accumulate indefinitely.
constexpr size_t MAX_NATIVE_REQUESTS = 2048;
constexpr size_t MAX_NATIVE_REQUESTS_PER_CONNECTION = 1024;
constexpr size_t RESUME_NATIVE_REQUESTS_PER_CONNECTION = 256;
constexpr size_t MAX_NATIVE_PAYLOAD_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_NATIVE_RESPONSE_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_NATIVE_RESPONSE_QUEUE_BYTES = 16 * 1024 * 1024;
constexpr size_t WORKER_COUNT = 4;

struct NativeRequestTask {
    websocketpp::connection_hdl handler;
    websocketpp::frame::opcode::value responseOpcode;
    router::NativeMessage request;
    size_t payloadBytes;
    bool windowOrdered = false;
    uint64_t windowSequence = 0;
};

deque<NativeRequestTask> nativeRequestQueue;
condition_variable nativeRequestCondition;
mutex nativeRequestMutex;
map<websocketpp::connection_hdl, size_t,
    owner_less<websocketpp::connection_hdl>> pendingByConnection;
set<websocketpp::connection_hdl,
    owner_less<websocketpp::connection_hdl>> pausedConnections;
set<websocketpp::connection_hdl,
    owner_less<websocketpp::connection_hdl>> pauseIssuedConnections;
size_t pendingNativeRequests = 0;
size_t pendingNativePayloadBytes = 0;
size_t pendingNativeResponseBytes = 0;
condition_variable nativeResponseCondition;
vector<thread> nativeWorkers;
thread serverThread;
mutex windowOrderMutex;
condition_variable windowOrderCondition;
uint64_t nextWindowSequence = 0;
uint64_t windowSequenceToExecute = 0;
bool workersStarted = false;
bool acceptingRequests = false;
atomic<bool> stopping(false);
atomic<bool> listenerStopped(false);

atomic<bool> initialized(false);
bool applyConfigHeaders = false;

bool __isExtensionEndpoint(const string &url);

bool __isWindowMethod(const string &method) {
    return method.rfind("window.", 0) == 0;
}

bool __sameConnection(const websocketpp::connection_hdl &left,
                      const websocketpp::connection_hdl &right) {
    owner_less<websocketpp::connection_hdl> less;
    return !less(left, right) && !less(right, left);
}

size_t __getAppConnectionCount() {
    lock_guard<mutex> guard(connectionMutex);
    return appConnections.size();
}


void __sendNow(const websocketpp::connection_hdl &handler,
               const string &payload,
               websocketpp::frame::opcode::value opcode) {
    if(!server) {
        return;
    }

    websocketpp::lib::error_code error;
    server->send(handler, payload, opcode, error);
    if(error) {
        debug::log(debug::LogTypeError,
            errors::makeErrorMsg(errors::NE_SR_UNBSEND, error.message()));
    }
}

bool __postToIO(const function<void()> &callback) {
    lock_guard<mutex> lifecycleGuard(ioLifecycleMutex);
    if(!server || ioStopRequested) {
        return false;
    }
    try {
        auto &ioService = server->get_io_service();
        if(ioService.stopped()) {
            return false;
        }
        ioService.post(callback);
        return true;
    }
    catch(const exception &e) {
        debug::log(debug::LogTypeError,
            errors::makeErrorMsg(errors::NE_SR_UNBSEND, e.what()));
        return false;
    }
}

void __stopListeningOnIO() {
    if(!server || listenerStopped.exchange(true)) {
        return;
    }
    websocketpp::lib::error_code error;
    server->stop_listening(error);
    if(error) {
        debug::log(debug::LogTypeError,
            errors::makeErrorMsg(errors::NE_SR_UNBSEND, error.message()));
    }
}

bool __isAcceptingRequests() {
    lock_guard<mutex> guard(nativeRequestMutex);
    return acceptingRequests && !stopping;
}

bool __isIOUnavailable() {
    lock_guard<mutex> lifecycleGuard(ioLifecycleMutex);
    return !server || ioStopRequested || server->get_io_service().stopped();
}

bool __reserveNativeResponse(size_t payloadBytes, bool waitForCapacity) {
    if(payloadBytes > MAX_NATIVE_RESPONSE_BYTES) {
        return false;
    }

    unique_lock<mutex> lock(nativeRequestMutex);
    auto hasCapacity = [payloadBytes]() {
        return pendingNativeResponseBytes <=
            MAX_NATIVE_RESPONSE_QUEUE_BYTES - payloadBytes;
    };

    while(!hasCapacity()) {
        if(!waitForCapacity || __isIOUnavailable()) {
            return false;
        }
        nativeResponseCondition.wait_for(lock, chrono::milliseconds(100));
    }
    pendingNativeResponseBytes += payloadBytes;
    return true;
}

void __releaseNativeResponse(size_t payloadBytes) {
    {
        lock_guard<mutex> guard(nativeRequestMutex);
        if(pendingNativeResponseBytes >= payloadBytes) {
            pendingNativeResponseBytes -= payloadBytes;
        }
        else {
            pendingNativeResponseBytes = 0;
        }
    }
    nativeResponseCondition.notify_all();
}

void __sendResponse(const websocketpp::connection_hdl &handler,
                    websocketpp::frame::opcode::value opcode,
                    const router::NativeMessage &response,
                    const function<void()> &onSent = nullptr,
                    bool waitForOutput = true) {
    string payload;
    try {
        json responseMessage;
        responseMessage["id"] = response.id;
        responseMessage["method"] = response.method;
        responseMessage["data"] = response.data;
        payload = helpers::jsonToString(responseMessage);
        if(payload.size() > MAX_NATIVE_RESPONSE_BYTES) {
            debug::log(debug::LogTypeError,
                errors::makeErrorMsg(errors::NE_SR_RSPTOOL,
                    to_string(payload.size())));
            responseMessage["data"] = json::object();
            responseMessage["data"]["error"] =
                errors::makeErrorPayload(errors::NE_SR_RSPTOOL,
                    to_string(payload.size()));
            payload = helpers::jsonToString(responseMessage);
        }
    }
    catch(const exception &e) {
        debug::log(debug::LogTypeError,
            errors::makeErrorMsg(errors::NE_SR_UNBSEND, e.what()));
        if(onSent) {
            onSent();
        }
        return;
    }

    const size_t responseBytes = payload.size();
    if(!__reserveNativeResponse(responseBytes, waitForOutput)) {
        if(onSent) {
            onSent();
        }
        return;
    }

    const function<void()> releaseResponse =
        [responseBytes, onSent]() {
            __releaseNativeResponse(responseBytes);
            if(onSent) {
                onSent();
            }
        };
    if(!__postToIO([handler, opcode, payload, releaseResponse]() {
        __sendNow(handler, payload, opcode);
        releaseResponse();
    })) {
        // A failed post cannot reach the IO callback. Complete the request
        // here so shutdown cannot wait forever for a response that was never
        // submitted to the transport.
        releaseResponse();
    }
}

void __finishNativeRequest(
    const websocketpp::connection_hdl &handler,
    size_t payloadBytes) {
    bool shouldResume = false;
    {
        lock_guard<mutex> guard(nativeRequestMutex);
        // Keep all accounting until the response has been submitted to the IO
        // service. This bounds both native work and response callbacks.
        if(pendingNativeRequests > 0) {
            --pendingNativeRequests;
        }
        if(pendingNativePayloadBytes >= payloadBytes) {
            pendingNativePayloadBytes -= payloadBytes;
        }

        auto pending = pendingByConnection.find(handler);
        if(pending != pendingByConnection.end()) {
            if(pending->second > 0) {
                --pending->second;
            }
            if(!stopping && pending->second <= RESUME_NATIVE_REQUESTS_PER_CONNECTION &&
                    pausedConnections.erase(handler) > 0 &&
                    pauseIssuedConnections.erase(handler) > 0) {
                shouldResume = true;
            }
            if(pending->second == 0) {
                pendingByConnection.erase(pending);
            }
        }

    }

    if(shouldResume) {
        __postToIO([handler]() {
            if(!server) {
                return;
            }
            websocketpp::lib::error_code error;
            server->resume_reading(handler, error);
            if(error) {
                debug::log(debug::LogTypeError,
                    errors::makeErrorMsg(errors::NE_SR_UNBSEND, error.message()));
            }
        });
    }
}

void __sendError(const websocketpp::connection_hdl &handler,
                 websocketpp::frame::opcode::value opcode,
                 const string &id,
                 const string &method,
                 errors::StatusCode code,
                 const string &param = "") {
    router::NativeMessage response;
    response.id = id;
    response.method = method;
    response.data["error"] = errors::makeErrorPayload(code, param);
    // Error responses are produced by the IO thread itself in overload and
    // parse-error paths. Never block that thread waiting for output capacity.
    __sendResponse(handler, opcode, response, nullptr, false);
}

websocketpp::frame::opcode::value __getResponseOpcode(
    const websocketpp::connection_hdl &handler,
    const string &url,
    const websocketserver::message_ptr &msg) {
    if(!__isExtensionEndpoint(url)) {
        return websocketpp::frame::opcode::text;
    }

    lock_guard<mutex> guard(connectionMutex);
    auto mode = wsMode.find(handler);
    if(mode != wsMode.end()) {
        return mode->second;
    }

    auto opcode = msg->get_opcode();
    if(opcode != websocketpp::frame::opcode::binary) {
        opcode = websocketpp::frame::opcode::text;
    }
    wsMode[handler] = opcode;
    return opcode;
}

void __pauseConnection(const websocketpp::connection_hdl &handler) {
    lock_guard<mutex> guard(nativeRequestMutex);
    const bool shouldPause = !stopping &&
        pausedConnections.find(handler) != pausedConnections.end() &&
        pauseIssuedConnections.insert(handler).second;
    if(!shouldPause || !server) {
        return;
    }

    // pause_reading() itself posts its transport operation. Keep the state
    // mutex held until it has enqueued that operation, so a worker cannot post
    // resume_reading() first and leave the connection paused forever.
    websocketpp::lib::error_code error;
    server->pause_reading(handler, error);
    if(error) {
        pausedConnections.erase(handler);
        pauseIssuedConnections.erase(handler);
        debug::log(debug::LogTypeError,
            errors::makeErrorMsg(errors::NE_SR_UNBSEND, error.message()));
    }
}

void __nativeWorkerLoop(size_t workerIndex) {
    helpers::setThreadName(settings::getProcessName() + "-wrk-" + to_string(workerIndex + 1));
    while(true) {
        NativeRequestTask task;
        {
            unique_lock<mutex> lock(nativeRequestMutex);
            nativeRequestCondition.wait(lock, []() {
                return stopping || !nativeRequestQueue.empty();
            });
            if(nativeRequestQueue.empty()) {
                return;
            }
            task = std::move(nativeRequestQueue.front());
            nativeRequestQueue.pop_front();
        }

        router::NativeMessage response;
        response.id = task.request.id;
        response.method = task.request.method;
        unique_lock<mutex> windowLock(windowOrderMutex, defer_lock);
        if(task.windowOrdered) {
            windowLock.lock();
            windowOrderCondition.wait(windowLock, [&task]() {
                return stopping || task.windowSequence == windowSequenceToExecute;
            });
        }
        try {
            response = router::executeNativeMethod(task.request);
        }
        catch(const exception &e) {
            debug::log(debug::LogTypeError, e.what());
            response.data["error"] = errors::makeErrorPayload(errors::NE_RT_NATEXCP);
        }
        catch(...) {
            response.data["error"] = errors::makeErrorPayload(errors::NE_RT_NATEXCP);
        }
        if(task.windowOrdered) {
            ++windowSequenceToExecute;
            windowLock.unlock();
            windowOrderCondition.notify_all();
        }

        // Keep accounting for the request until the response has been
        // submitted on the WebSocket IO thread. This is required for bounded
        // output and for an orderly shutdown.
        const auto handler = task.handler;
        const size_t payloadBytes = task.payloadBytes;
        __sendResponse(task.handler, task.responseOpcode, response,
            [handler, payloadBytes]() {
                __finishNativeRequest(handler, payloadBytes);
            });
    }
}

void __startNativeWorkers() {
    lock_guard<mutex> guard(nativeRequestMutex);
    if(workersStarted) {
        return;
    }
    workersStarted = true;
    acceptingRequests = true;
    stopping = false;
    for(size_t i = 0; i < WORKER_COUNT; ++i) {
        nativeWorkers.emplace_back(__nativeWorkerLoop, i);
    }
}

void __stopNativeWorkers() {
    {
        lock_guard<mutex> guard(nativeRequestMutex);
        if(stopping) {
            return;
        }
        acceptingRequests = false;
        stopping = true;
        for(const auto &task: nativeRequestQueue) {
            if(pendingNativeRequests > 0) {
                --pendingNativeRequests;
            }
            if(pendingNativePayloadBytes >= task.payloadBytes) {
                pendingNativePayloadBytes -= task.payloadBytes;
            }
            auto pending = pendingByConnection.find(task.handler);
            if(pending != pendingByConnection.end()) {
                if(pending->second > 0) {
                    --pending->second;
                }
                if(pending->second == 0) {
                    pendingByConnection.erase(pending);
                }
            }
        }
        nativeRequestQueue.clear();
        pausedConnections.clear();
        pauseIssuedConnections.clear();
    }
    nativeRequestCondition.notify_all();
    windowOrderCondition.notify_all();
}

bool __tryEnqueueNativeRequest(NativeRequestTask &&task, bool &shouldPause) {
    lock_guard<mutex> guard(nativeRequestMutex);
    shouldPause = false;
    if(!acceptingRequests || stopping ||
        pendingNativeRequests >= MAX_NATIVE_REQUESTS ||
        task.payloadBytes > MAX_NATIVE_PAYLOAD_BYTES ||
        pendingNativePayloadBytes > MAX_NATIVE_PAYLOAD_BYTES - task.payloadBytes) {
        return false;
    }

    auto pending = pendingByConnection[task.handler];
    if(pending >= MAX_NATIVE_REQUESTS_PER_CONNECTION) {
        return false;
    }

    task.windowOrdered = __isWindowMethod(task.request.method);
    if(task.windowOrdered) {
        task.windowSequence = nextWindowSequence++;
    }
    nativeRequestQueue.emplace_back(std::move(task));
    ++pendingNativeRequests;
    pendingNativePayloadBytes += nativeRequestQueue.back().payloadBytes;
    const size_t connectionPending = ++pendingByConnection[nativeRequestQueue.back().handler];
    const bool reachedHighWatermark =
        connectionPending >= MAX_NATIVE_REQUESTS_PER_CONNECTION ||
        pendingNativeRequests >= MAX_NATIVE_REQUESTS - WORKER_COUNT;
    shouldPause = reachedHighWatermark && pausedConnections.insert(
        nativeRequestQueue.back().handler).second;
    nativeRequestCondition.notify_one();
    return true;
}

bool __isExtensionEndpoint(const string &url) {
    return regex_match(url, regex(".*extensionId=.*"));
}

bool __hasConnectToken(const string &url) {
    return regex_match(url, regex(".*connectToken=.*"));
}

fs::FileReaderOptions __getFileReaderOptionsFromHeaders(const websocketpp::http::parser::header_list &headers) {
    fs::FileReaderOptions fileReaderOptions;
    auto rangeIt = headers.find("Range");
    if(rangeIt == headers.end()) {
        return fileReaderOptions;
    }

    smatch match;
    if(regex_match(rangeIt->second, match, regex(R"(bytes=(\d+)-(\d*)?)"))) {
        fileReaderOptions.pos = stoll(match[1].str());
        if(match[2].matched && match[2].str() != "") {
            long long end = stoll(match[2].str());
            if(end >= fileReaderOptions.pos) {
                fileReaderOptions.size = end - fileReaderOptions.pos + 1;
            }
        }
    }
    return fileReaderOptions;
}

void __applyConfigHeaders(websocketserver::connection_ptr con) {
    json jHeaders = settings::getOptionForCurrentMode("serverHeaders");
    for(const auto &it: jHeaders.items()) {
        con->replace_header(it.key(), it.value().get<string>());
    }
}

string __getParamValueFromUrl(const string &url, const string &param) {
    string val = "";
    smatch matches;
    if(regex_search(url, matches, regex(param + "=([\\w.\\-_]+)"))) {
        if(matches.size() >= 2) {
            val = matches[1].str();
        }
    }
    return val;
}

string __getExtensionIdFromUrl(const string &url) {
    return __getParamValueFromUrl(url, "extensionId");
}

string __getConnectTokenFromUrl(const string &url) {
    return __getParamValueFromUrl(url, "connectToken");
}

void __exitProcessIfIdle() {
    thread exitCheckThread([]() {
        helpers::setThreadName(settings::getProcessName() + "-idle");
        this_thread::sleep_for(10s);
        if(__getAppConnectionCount() == 0) {
            app::exit();
        }
    });
    exitCheckThread.detach();
}

uint16_t __deriveStablePort(const string &appId) {
    uint32_t hash = 2166136261u;
    for(unsigned char c: appId) {
        hash ^= c;
        hash *= 16777619u;
    }
    return static_cast<uint16_t>(19000 + (hash % 29000));
}

string init() {
    int port = 0;
    json jPort = settings::getOptionForCurrentMode("port");
    if(!jPort.is_null()) {
        port = jPort.get<int>();
    }
    server = new websocketserver();

    server->set_message_handler([&](websocketpp::connection_hdl handler, websocketserver::message_ptr msg) {
        neuserver::handleMessage(handler, msg);
    });

    server->set_http_handler([&](websocketpp::connection_hdl handler) {
        neuserver::handleHTTP(handler);
    });

    server->set_open_handler([&](websocketpp::connection_hdl handler) {
        neuserver::handleConnect(handler);
    });

    server->set_close_handler([&](websocketpp::connection_hdl handler) {
        neuserver::handleDisconnect(handler);
    });

    server->set_validate_handler([&](websocketpp::connection_hdl handler) {
        return neuserver::handleValidate(handler);
    });

    server->set_access_channels(websocketpp::log::alevel::none);
    server->set_error_channels(websocketpp::log::elevel::none);

    server->init_asio();
    #if !defined(_WIN32)
    server->set_reuse_addr(true);
    #else
    server->set_reuse_addr(port != 0);
    #endif
    server->set_max_message_size(MAX_NATIVE_PAYLOAD_BYTES);

    string hostAddress = "127.0.0.1";
    if(settings::getMode() == settings::AppModeCloud) {
        hostAddress = "0.0.0.0";
    }
    auto bindAddr = websocketpp::lib::asio::ip::address::from_string(hostAddress);
    websocketpp::lib::asio::ip::tcp::endpoint endpoint(bindAddr, port);

    if(port == 0) {
        // Keep a consistent localhost port when port=0 so WebView localStorage
        // and IndexedDB don't get wiped by a random origin on every launch.
        uint16_t basePort = __deriveStablePort(settings::getAppId());
        bool bound = false;
        for(uint16_t offset = 0; offset < 8; ++offset) {
            websocketpp::lib::asio::ip::tcp::endpoint candidate(
                bindAddr, static_cast<uint16_t>(basePort + offset));
            websocketpp::lib::error_code listenEc;
            server->listen(candidate, listenEc);
            if(!listenEc) {
                bound = true;
                break;
            }
        }
        if(!bound) {
            server->listen(endpoint);
        }
    }
    else {
        server->listen(endpoint);
    }
    listenerStopped = false;
    {
        lock_guard<mutex> lifecycleGuard(ioLifecycleMutex);
        ioStopRequested = false;
    }
    server->start_accept();

    websocketpp::lib::asio::error_code error;
    int actualPort = server->get_local_endpoint(error).port();

    if(port != actualPort) {
        port = actualPort;
        settings::setPort(port);
    }

    string navigationUrl = "http://127.0.0.1:" + to_string(port);
    json jUrl = settings::getOptionForCurrentMode("url");

    if(!jUrl.is_null()) {
        string url = jUrl.get<string>();
        if (regex_match(url, regex("^/.*")))
            navigationUrl += url;
        else
            navigationUrl = url;
    }
    initialized = true;

    json jHeaders = settings::getOptionForCurrentMode("serverHeaders");
    if(!jHeaders.is_null()) {
        applyConfigHeaders = true;
    }

    return navigationUrl;
}

bool isInitialized() {
    return initialized;
}

void startAsync() {
    __startNativeWorkers();
    serverThread = thread([](){
        helpers::setThreadName(settings::getProcessName() + "-ipc");
        server->run();
    });
}

void stop() {
    // Stop admitting native work and new TCP connections, but keep the IO
    // service alive long enough for workers to publish in-flight responses.
    __stopNativeWorkers();
    initialized = false;
    __postToIO([]() {
        __stopListeningOnIO();
    });
}

void stopIO() {
    lock_guard<mutex> lifecycleGuard(ioLifecycleMutex);
    if(!server || ioStopRequested) {
        return;
    }
    ioStopRequested = true;
    nativeResponseCondition.notify_all();
    try {
        auto &ioService = server->get_io_service();
        if(ioService.stopped()) {
            return;
        }
        ioService.post([]() {
            if(!server) {
                return;
            }
            __stopListeningOnIO();
            server->stop();
        });
    }
    catch(const exception &e) {
        debug::log(debug::LogTypeError,
            errors::makeErrorMsg(errors::NE_SR_UNBSEND, e.what()));
    }
}

void waitForNativeWorkers() {
    // A worker can be waiting for its response callback to run on the IO
    // thread. Waiting for response accounting here before stopping IO can
    // deadlock shutdown: the callback is the thing that decrements that
    // counter. Joining the workers is sufficient; each worker has finished
    // its native call before it can be joined.
    for(auto &worker: nativeWorkers) {
        if(worker.joinable()) {
            worker.join();
        }
    }
    nativeWorkers.clear();
}

void waitForServer() {
    if(serverThread.joinable() &&
        serverThread.get_id() != this_thread::get_id()) {
        serverThread.join();
    }
}

void handleMessage(websocketpp::connection_hdl handler, websocketserver::message_ptr msg) {
    // During shutdown, leave already accepted requests to drain but do not
    // parse or enqueue any new frontend work.
    if(!__isAcceptingRequests()) {
        return;
    }

    websocketserver::connection_ptr con = server->get_con_from_hdl(handler);
    string url = con->get_resource();
    auto responseOpcode = __getResponseOpcode(handler, url, msg);
    string id;
    string method;

    try {
        const string payload = msg->get_payload();
        if(payload.size() > MAX_NATIVE_PAYLOAD_BYTES) {
            __sendError(handler, responseOpcode, id, method, errors::NE_SR_RPCOVFL);
            return;
        }

        json nativeMessage = json::parse(payload);
        if(nativeMessage.is_object()) {
            if(nativeMessage.contains("id") && nativeMessage["id"].is_string()) {
                id = nativeMessage["id"].get<string>();
            }
            if(nativeMessage.contains("method") && nativeMessage["method"].is_string()) {
                method = nativeMessage["method"].get<string>();
            }
        }
        if(!nativeMessage.is_object() ||
            !nativeMessage.contains("id") || !nativeMessage["id"].is_string() ||
            !nativeMessage.contains("method") || !nativeMessage["method"].is_string() ||
            !nativeMessage.contains("accessToken") ||
                !nativeMessage["accessToken"].is_string()) {
            __sendError(handler, responseOpcode, id, method, errors::NE_SR_UNBPARS);
            return;
        }

        id = nativeMessage["id"].get<string>();
        method = nativeMessage["method"].get<string>();
        string accessToken = nativeMessage["accessToken"].get<string>();
        json data = nativeMessage.contains("data") ? nativeMessage["data"] : json(nullptr);

        NativeRequestTask task{
            handler,
            responseOpcode,
            {id, method, accessToken, data},
            payload.size()
        };
        bool shouldPause = false;
        if(!__tryEnqueueNativeRequest(std::move(task), shouldPause)) {
            // A shutdown race must not create an unbounded stream of overload
            // responses while the transport is draining.
            if(__isAcceptingRequests()) {
                // The request was not admitted, so do not mark the connection
                // as paused: there may be no accepted task left to resume it.
                __sendError(handler, responseOpcode, id, method,
                    errors::NE_SR_RPCOVFL);
            }
        }
        else if(shouldPause) {
            __pauseConnection(handler);
        }
    }
    catch(const exception &e) {
        debug::log(debug::LogTypeError,
            errors::makeErrorMsg(errors::NE_SR_UNBPARS, e.what()));
        __sendError(handler, responseOpcode, id, method, errors::NE_SR_UNBPARS);
    }
}

void handleHTTP(websocketpp::connection_hdl handler) {
    websocketserver::connection_ptr con = server->get_con_from_hdl(handler);
    string resource = con->get_resource();
    string documentRoot = neuserver::getDocumentRoot();
    if(!documentRoot.empty()) {
        resource = documentRoot + resource;
    }
    fs::FileReaderOptions fileReaderOptions =
        __getFileReaderOptionsFromHeaders(con->get_request().get_headers());
    router::Response routerResponse = router::serve(resource, fileReaderOptions);
    con->set_status(routerResponse.status);
    con->set_body(routerResponse.data);
    con->replace_header("Content-Type",routerResponse.contentType);
    for(const auto &[header, value]: routerResponse.headers) {
        con->replace_header(header, value);
    }
    
    if(applyConfigHeaders){
        __applyConfigHeaders(con);
    }
}

void handleConnect(websocketpp::connection_hdl handler) {
    websocketserver::connection_ptr con = server->get_con_from_hdl(handler);
    string url = con->get_resource();
    bool extension = __isExtensionEndpoint(url);
    string extensionId;
    size_t appCount;
    size_t connectionCount;

    {
        lock_guard<mutex> guard(connectionMutex);
        if(extension) {
            extensionId = __getExtensionIdFromUrl(url);
            extConnections[extensionId] = handler;
        }
        else {
            appConnections.insert(handler);
        }
        appCount = appConnections.size();
        connectionCount = appCount + extConnections.size();
    }

    if(extension) {
        events::dispatch("extClientConnect", extensionId);
    }
    else {
        events::dispatch("appClientConnect", appCount);
    }
    events::dispatch("clientConnect", connectionCount);
}

void handleDisconnect(websocketpp::connection_hdl handler) {
    websocketserver::connection_ptr con = server->get_con_from_hdl(handler);
    string url = con->get_resource();
    bool extension = __isExtensionEndpoint(url);
    string extensionId;
    size_t appCount;
    size_t connectionCount;

    {
        lock_guard<mutex> guard(connectionMutex);
        wsMode.erase(handler);
        if(extension) {
            extensionId = __getExtensionIdFromUrl(url);
            auto connected = extConnections.find(extensionId);
            if(connected != extConnections.end() &&
                __sameConnection(connected->second, handler)) {
                extConnections.erase(connected);
            }
        }
        else {
            appConnections.erase(handler);
        }
        appCount = appConnections.size();
        connectionCount = appCount + extConnections.size();
    }

    {
        lock_guard<mutex> guard(nativeRequestMutex);
        pendingByConnection.erase(handler);
        pausedConnections.erase(handler);
        pauseIssuedConnections.erase(handler);
    }

    if(extension) {
        events::dispatch("extClientDisconnect", extensionId);
    }
    else {
        settings::AppMode mode = settings::getMode();
        if(mode == settings::AppModeBrowser || mode == settings::AppModeChrome) {
            __exitProcessIfIdle();
        }
        events::dispatch("appClientDisconnect", appCount);
    }
    events::dispatch("clientDisconnect", connectionCount);
}

bool handleValidate(websocketpp::connection_hdl handler) {
    websocketserver::connection_ptr con = server->get_con_from_hdl(handler);
    string url = con->get_resource();
    string host = con->get_host();

    if(!(host == "localhost" || host == "127.0.0.1") && settings::getMode() != settings::AppModeCloud) {
        return false;
    }

    if(!__hasConnectToken(url)) {
        return false;
    }

    string connectToken = __getConnectTokenFromUrl(url);
    if(!authbasic::verifyConnectToken(connectToken)) {
        return false;
    }

    if(__isExtensionEndpoint(url)) {
        string extensionId = __getExtensionIdFromUrl(url);
        return extensions::isLoaded(extensionId);
    }

    return true;
}

void broadcast(const json &message) {
    neuserver::broadcastToAllApps(message);
    neuserver::broadcastToAllExtensions(message); 
}

bool sendToExtension(const string &extensionId, const json &message) {
    websocketpp::connection_hdl handler;
    websocketpp::frame::opcode::value opcode = websocketpp::frame::opcode::text;
    {
        lock_guard<mutex> guard(connectionMutex);
        auto connection = extConnections.find(extensionId);
        if(connection == extConnections.end()) {
            return false;
        }
        handler = connection->second;
        auto mode = wsMode.find(handler);
        if(mode != wsMode.end()) {
            opcode = mode->second;
        }
    }

    string payload = helpers::jsonToString(message);
    __postToIO([handler, opcode, payload]() {
        __sendNow(handler, payload, opcode);
    });
    return true;
}

void broadcastToAllExtensions(const json &message) {
    vector<pair<websocketpp::connection_hdl,
        websocketpp::frame::opcode::value>> targets;
    {
        lock_guard<mutex> guard(connectionMutex);
        for(const auto &[extensionId, connection]: extConnections) {
            auto opcode = websocketpp::frame::opcode::text;
            auto mode = wsMode.find(connection);
            if(mode != wsMode.end()) {
                opcode = mode->second;
            }
            targets.emplace_back(connection, opcode);
        }
    }

    string payload = helpers::jsonToString(message);
    __postToIO([targets = std::move(targets), payload]() {
        for(const auto &[handler, opcode]: targets) {
            __sendNow(handler, payload, opcode);
        }
    });
}

void broadcastToAllApps(const json &message) {
    vector<websocketpp::connection_hdl> targets;
    {
        lock_guard<mutex> guard(connectionMutex);
        targets.assign(appConnections.begin(), appConnections.end());
    }

    string payload = helpers::jsonToString(message);
    __postToIO([targets = std::move(targets), payload]() {
        for(const auto &handler: targets) {
            __sendNow(handler, payload, websocketpp::frame::opcode::text);
        }
    });
}

vector<string> getConnectedExtensions() {
    vector<string> extensions;
    lock_guard<mutex> guard(connectionMutex);
    for(const auto &[extensionId, _]: extConnections) {
        extensions.push_back(extensionId);
    }
    return extensions;
}

string getDocumentRoot() {
    string documentRoot = "";
    json jDocumentRoot = settings::getOptionForCurrentMode("documentRoot");
    if(!jDocumentRoot.is_null()) {
        documentRoot = jDocumentRoot.get<string>();
        
        if(documentRoot.back() == '/') {
            documentRoot.pop_back();
        }
    }
    return documentRoot;
}

} // namespace neuserver
