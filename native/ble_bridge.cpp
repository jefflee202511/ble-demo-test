// ble_bridge.cpp
// Thin C wrapper around NativeBLE so that it can be called through FFI.
//
// How to read this file:
//   0. On load         - runs once when the DLL is loaded into memory
//   1. State           - what the DLL remembers
//   2. Worker thread   - the single thread that talks to NativeBLE
//   3. DLL -> JS       - NativeBLE events forwarded to the registered callbacks
//   4. Settings        - the settings manager (current settings kept in memory)
//   5. Pinning         - certificate pinning (which server certificates are trusted)
//   6. JS -> DLL       - the extern "C" functions that JS calls
#include "ble_bridge.h"
#include "NativeBleController.h"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
  #if defined(__APPLE__)
    #include <sys/types.h>
    #include <sys/sysctl.h>
  #else
    #include <sys/utsname.h>
  #endif
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <regex>
#include <string>
#include <thread>

namespace {

// ============================================================
// 0. On load
//    As soon as the DLL is loaded into memory, create model_intf.json
//    in the same folder as the DLL. It describes this module:
//    module name, module version and the OS it is running on.
//    No BLE work is done here, and no callback exists yet, so the result
//    is only remembered and reported later from ble_init().
// ============================================================

const char INTERFACE_FILE_NAME[] = "model_intf.json";

// Identity of this module. Change MODULE_VERSION when the DLL is released.
const char MODULE_NAME[]    = "ble_bridge";
const char MODULE_VERSION[] = "0.1.0";

// Capabilities of this module. Hardcoded for now.
const bool REST_API          = true;
const bool DEBUG_ON_REST_API = false; // debug output for the REST API
const char CRYPTO[]          = "on";
const char* const CRYPTO_TYPES[] = { "tls1.2", "tls1.3", "jwt" }; // supported, in this order

// Certificate pinning: SHA-256 fingerprints of the server public keys this DLL trusts.
// 64 lower-case hex digits each. Built into the DLL on purpose: they cannot be
// changed from outside, only by building a new DLL.
// DEMO VALUES - replace with the fingerprints of the real server.
// Keep at least two: the key in use and a backup key for when it is replaced.
const char* const PINNED_SHA256[] = {
    "667574a81ae67966c0c17f87929dde771189c9f5cbd7ffa2791abb7162487659", // demo: key in use
    "cdb7fdb998fe0d6e3cabdcf2f3682e6f41038f18014ada4bc770d1949ed7cd02", // demo: backup key
};

bool g_interface_file_created = false;

// Any address inside this DLL; used to ask the OS where the DLL file is
const int g_module_anchor = 0;

// Name and version of the OS this DLL is running on ("unknown" if it cannot be read)
void read_os_info(std::string& name, std::string& version) {
    version = "unknown";
#if defined(_WIN32)
    name = "Windows";
    // RtlGetVersion reports the real version. (GetVersionEx can report an older
    // one depending on the application.) ntdll.dll is always loaded already.
    typedef LONG (WINAPI *RtlGetVersionFn)(RTL_OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn rtl_get_version =
        ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    RTL_OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl_get_version && rtl_get_version(&info) == 0) {
        version = std::to_string(info.dwMajorVersion) + "." +
                  std::to_string(info.dwMinorVersion) + "." +
                  std::to_string(info.dwBuildNumber);
    }
#elif defined(__APPLE__)
    name = "macOS";
    char text[64] = {};
    size_t size = sizeof(text) - 1;
    if (sysctlbyname("kern.osproductversion", text, &size, nullptr, 0) == 0) version = text;
#else
    name = "Linux";
    struct utsname info;
    if (uname(&info) == 0) {
        name = info.sysname;
        version = info.release;
    }
#endif
}

// Text -> JSON string literal (with the quotes)
std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", c); out += code; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

// The content of model_intf.json
std::string interface_json() {
    std::string os_name, os_version;
    read_os_info(os_name, os_version);

    std::string json;
    json += "{\n";
    json += "  \"module\": {\n";
    json += "    \"name\": "    + json_string(MODULE_NAME)    + ",\n";
    json += "    \"version\": " + json_string(MODULE_VERSION) + "\n";
    json += "  },\n";
    json += "  \"os\": {\n";
    json += "    \"name\": "    + json_string(os_name)    + ",\n";
    json += "    \"version\": " + json_string(os_version) + "\n";
    json += "  },\n";
    json += std::string("  \"rest_api\": ") + (REST_API ? "true" : "false") + ",\n";
    json += std::string("  \"debug_on_rest_api\": ") + (DEBUG_ON_REST_API ? "true" : "false") + ",\n";
    json += "  \"crypto\": " + json_string(CRYPTO) + ",\n";

    json += "  \"crypto_types\": [";
    bool first = true;
    for (const char* type : CRYPTO_TYPES) {
        if (!first) json += ", ";
        json += json_string(type);
        first = false;
    }
    json += "],\n";

    json += "  \"cert_pinning\": \"on\",\n";
    json += "  \"pinned_sha256\": [\n";
    first = true;
    for (const char* pin : PINNED_SHA256) {
        if (!first) json += ",\n";
        json += "    " + json_string(pin);
        first = false;
    }
    json += "\n  ]\n";

    json += "}\n";
    return json;
}

bool create_interface_file() {
    const std::string content = interface_json();

#if defined(_WIN32)
    // Wide characters, so that folder names with non-ASCII letters work
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_module_anchor), &module)) {
        return false;
    }
    wchar_t dll_path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, dll_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;

    std::wstring path(dll_path, length);
    const size_t slash = path.find_last_of(L"/\\");
    if (slash == std::wstring::npos) return false;
    path.erase(slash + 1);
    for (const char* c = INTERFACE_FILE_NAME; *c; ++c) path += static_cast<wchar_t>(*c);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
#else
    Dl_info info;
    if (!dladdr(&g_module_anchor, &info) || !info.dli_fname) return false;

    std::string path(info.dli_fname);
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return false;
    path.erase(slash + 1);
    path += INTERFACE_FILE_NAME;

    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
#endif
    // The file is rewritten on every load, so it always describes this DLL
    const size_t written = std::fwrite(content.data(), 1, content.size(), file);
    const bool closed = (std::fclose(file) == 0);
    return written == content.size() && closed;
}

// The constructor of a global object runs when the DLL is loaded
struct OnLoad {
    OnLoad() { g_interface_file_created = create_interface_file(); }
};
const OnLoad g_on_load;

// ============================================================
// 1. State
// ============================================================

// Same UUIDs as the GATT server (gatt_echo_server.cpp): Nordic UART Service
const std::string SERVICE_UUID     = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const std::string WRITE_CHAR_UUID  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // app -> device
const std::string NOTIFY_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // device -> app

// Function pointers registered by JS through ble_init().
// The DLL calls these to send data up to JS.
ble_log_cb   g_log   = nullptr;
ble_rx_cb    g_rx    = nullptr;
ble_found_cb g_found = nullptr;
ble_config_cb g_config_cb = nullptr;

// The NativeBLE controller. Created and used on the worker thread only.
std::unique_ptr<NativeBLE::NativeBleController> g_ble;

std::atomic<bool> g_inited{false};
std::atomic<bool> g_connecting{false};
std::atomic<bool> g_connected{false};

void log(const std::string& text) {
    if (g_log) g_log(text.c_str());
}

// ---- Periodic "to nordic UART N" sender ----
// When the connected device's name matches HELLO_DEVICE_NAME, the DLL sends
// "to nordic UART 1", "to nordic UART 2", ... every HELLO_INTERVAL until the device disconnects.
// Sending is switched by the setting "send_repeat" (section 4): it is turned on
// automatically when this device connects, and JS can turn it off / on again.
// The name is compared without case, spaces, '_' and '-', and only has to be
// contained, so "Nordic Uart" also matches "Nordic_UART_Service".
const char HELLO_DEVICE_NAME[] = "Nordic Uart";
const char HELLO_TEXT[]        = "to nordic UART "; // text sent to the Nordic UART device, followed by the count
const char HELLO_SUFFIX[]      = "";   // e.g. "\r\n" if the firmware wants line ends
const std::chrono::milliseconds HELLO_INTERVAL(3000);

// address -> name, remembered from scan results (ble_connect only gets an address)
std::mutex g_names_mutex;
std::map<std::string, std::string> g_names;
std::string g_target_address; // device of the current connection; guarded by g_names_mutex

// ---- Automatic reconnect ----
// A connection attempt can fail before it becomes usable, for example with
// "Could not discover services for device." The DLL then tries the same
// device again by itself, up to CONNECT_RETRY_MAX times, so the user does not
// have to press Disconnect / Connect by hand.
// Not retried: a disconnect the user asked for, and a connection that was
// already working and dropped later.
// The whole feature is switched by the setting "auto_reconnect" (section 4).
// With it off, a failed attempt just ends, as before this feature existed.
const bool AUTO_RECONNECT_DEFAULT = true; // value of the setting when the DLL starts
const int CONNECT_RETRY_MAX = 3;                           // retries after the first attempt
const std::chrono::milliseconds CONNECT_RETRY_DELAY(1000); // pause before each retry
// Call disconnect() before each retry to clear what the failed attempt left behind.
// Set to false if NativeBLE turns out not to accept disconnect() in that state.
const bool RETRY_DISCONNECT_FIRST = true;

std::string g_retry_address;               // device of the last ble_connect(); guarded by g_names_mutex
std::atomic<int>  g_retry_left{0};         // retries still allowed for that device
std::atomic<bool> g_attempt_active{false}; // an attempt is running and has not succeeded or failed yet
std::atomic<bool> g_retry_cancel{false};   // the user asked for something else (disconnect / scan)
std::atomic<bool> g_retry_cleanup{false};  // inside the disconnect() that precedes a retry
std::atomic<bool> g_auto_reconnect{AUTO_RECONNECT_DEFAULT}; // copy of the setting "auto_reconnect"

void schedule_retry(); // defined in section 3

// Stop retrying (called when the user disconnects or starts a scan)
void cancel_retry() {
    g_retry_cancel = true;
    g_attempt_active = false;
    g_retry_left = 0;
}

std::atomic<bool> g_hello_device{false}; // the connected device is the hello device
std::atomic<bool> g_send_repeat{false};  // copy of the setting "send_repeat" (section 4)

// True while the hello messages have to be sent
bool hello_sending() { return g_hello_device && g_send_repeat; }

void set_send_repeat(bool on); // defined in section 4
uint32_t g_hello_count = 0;                         // worker thread only
std::chrono::steady_clock::time_point g_hello_next; // worker thread only

std::string lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// "Nordic_UART Service" -> "nordicuartservice"
std::string name_key(const std::string& name) {
    std::string out;
    for (char c : lower(name)) if (c != ' ' && c != '_' && c != '-') out += c;
    return out;
}

// True if the device being connected is the one that gets the hello messages
bool is_hello_device() {
    std::lock_guard<std::mutex> lock(g_names_mutex);
    const auto it = g_names.find(g_target_address);
    if (it == g_names.end()) return false;
    return name_key(it->second).find(name_key(HELLO_DEVICE_NAME)) != std::string::npos;
}

// ============================================================
// 2. Worker thread
//    Every NativeBLE call is put into a queue and executed here, so that
//    - nothing BLE related runs while the DLL is being loaded
//    - the Electron main thread is never blocked (connect can take long)
// ============================================================

std::thread g_worker;
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::queue<std::function<void()>> g_queue;
bool g_running = false; // guarded by g_queue_mutex

// Ask the worker thread to run a task later
void post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (!g_running) return;
        g_queue.push(std::move(task));
    }
    g_queue_cv.notify_one();
}

// Run one task. A C++ exception must never leave the DLL, so catch everything.
void run_task(const std::function<void()>& task) {
    try {
        task();
    } catch (const std::exception& e) {
        log(std::string("native error: ") + e.what());
    } catch (...) {
        log("native error: unknown exception");
    }
}

void setup_controller(); // defined in section 3
void send_hello();       // defined in section 3

void worker_main() {
    run_task(setup_controller);

    for (;;) {
        std::function<void()> task;
        bool has_task = false;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            const auto ready = [] { return !g_queue.empty() || !g_running; };
            if (hello_sending()) {
                // Also wake up when the next hello message is due
                g_queue_cv.wait_until(lock, g_hello_next, ready);
            } else {
                g_queue_cv.wait(lock, ready);
            }
            if (!g_queue.empty()) {
                task = std::move(g_queue.front());
                g_queue.pop();
                has_task = true;
            } else if (!g_running) {
                break; // stop requested and queue drained
            }
        }
        if (has_task && g_ble) run_task(task);

        // Periodic hello message (only while the hello device is connected)
        if (g_ble && hello_sending() && std::chrono::steady_clock::now() >= g_hello_next) {
            run_task(send_hello);
            g_hello_next = std::chrono::steady_clock::now() + HELLO_INTERVAL;
        }
    }

    // Shutdown
    if (g_ble) {
        run_task([] {
            g_ble->scan_stop();
            if (g_connected) g_ble->disconnect();
            g_ble->dispose();
        });
        g_ble.reset();
    }
}

// ============================================================
// 3. DLL -> JS
//    NativeBLE calls these when something happens. They forward the event
//    to JS by calling the function pointers saved in section 1.
// ============================================================

// Data arrived from the device
void on_notify(const uint8_t* data, uint32_t len) {
    if (g_rx) g_rx(data, len); // -> JS

    // If the data contains printable ASCII characters (0x20..0x7E), show them
    // on their own line, after the line JS prints for the received bytes.
    std::string ascii;
    for (uint32_t i = 0; i < len; ++i) {
        if (data[i] >= 0x20 && data[i] <= 0x7E) ascii += static_cast<char>(data[i]);
    }
    if (!ascii.empty()) log("[ Recv String ] : " + ascii);

    // The hello device echoes everything it receives. Echoing that back again
    // would bounce the same bytes between the two sides forever, so no echo here
    // (also while "send_repeat" is off).
    if (g_hello_device) return;

    // echo: write the same bytes back to the device
    std::string payload(reinterpret_cast<const char*>(data), len);
    post([payload] {
        if (g_connected) g_ble->write_command(SERVICE_UUID, WRITE_CHAR_UUID, payload);
    });
}

// Send the next "to nordic UART N" to the device (worker thread)
void send_hello() {
    if (!g_connected) return;
    const std::string payload = HELLO_TEXT + std::to_string(++g_hello_count) + HELLO_SUFFIX;
    g_ble->write_command(SERVICE_UUID, WRITE_CHAR_UUID, payload);
    log("[ Sent String ] : " + payload);
}

void on_scan_found(NativeBLE::DeviceDescriptor device) {
    if (!device.name.empty()) {
        std::lock_guard<std::mutex> lock(g_names_mutex);
        g_names[lower(device.address)] = device.name; // remembered for is_hello_device()
    }
    if (g_found) g_found(device.name.c_str(), device.address.c_str()); // -> JS
}

// The current connection attempt failed: try the same device again (any thread).
// Does nothing unless an attempt is running, so it is safe to call from both
// the subscribe failure and the disconnect handler of the same attempt.
void schedule_retry() {
    if (!g_attempt_active.exchange(false)) return;
    if (!g_auto_reconnect) return; // switched off: leave the attempt as failed

    std::string address;
    {
        std::lock_guard<std::mutex> lock(g_names_mutex);
        address = g_retry_address;
    }
    const int left = g_retry_left.load();
    if (left <= 0) {
        log("connect failed: gave up after " + std::to_string(CONNECT_RETRY_MAX) + " retries (" + address + ")");
        g_connecting = false;
        return;
    }
    g_retry_left = left - 1;
    const int number = CONNECT_RETRY_MAX - left + 1; // 1, 2, 3

    g_connecting = true; // still busy: refuse other ble_connect() calls meanwhile
    post([address, number] {
        std::this_thread::sleep_for(CONNECT_RETRY_DELAY);
        if (g_retry_cancel) {
            g_connecting = false;
            return;
        }
        log("connect retry " + std::to_string(number) + "/" + std::to_string(CONNECT_RETRY_MAX) + ": " + address);
        if (RETRY_DISCONNECT_FIRST) {
            g_retry_cleanup = true; // keep this internal disconnect out of the log
            try { g_ble->disconnect(); } catch (...) {}
            g_retry_cleanup = false;
        }
        g_connected = false;
        g_connecting = true; // the disconnect above may have cleared it
        g_attempt_active = true;
        g_ble->connect(address);
    });
}

void on_connected() {
    g_connected = true;
    log("connected");

    // Subscribe to notifications from the device.
    // If the device does not have this service/characteristic, NativeBLE
    // fails inside, so catch it and disconnect instead of crashing the app.
    post([] {
        try {
            g_ble->notify(SERVICE_UUID, NOTIFY_CHAR_UUID, on_notify);
            log("notify subscribed");
            g_attempt_active = false; // the connection is usable: nothing to retry
            g_retry_left = 0;

            // Start the periodic hello messages if this is the hello device
            if (is_hello_device()) {
                g_hello_count = 0; // every connection starts again from 1
                g_hello_next = std::chrono::steady_clock::now() + HELLO_INTERVAL;
                g_hello_device = true;
                set_send_repeat(true); // -> JS shows the option as checked
                log(std::string("hello sender started (device name matches \"") + HELLO_DEVICE_NAME + "\")");
            }
        } catch (...) {
            log("notify subscribe failed: service/characteristic UUID not found on this device. disconnecting.");
            try { g_ble->disconnect(); } catch (...) {}
            g_connected = false;
            g_connecting = false;
            schedule_retry();
        }
    });
}

void on_disconnected(std::string reason) {
    if (g_retry_cleanup) return; // caused by the cleanup before a retry: not an event to report
    g_hello_device = false;
    set_send_repeat(false); // -> JS shows the option as unchecked
    g_connected = false;
    g_connecting = false;
    log("disconnected: " + reason);
    schedule_retry(); // only acts if this attempt never became usable
}

// Create the controller and register the handlers above (worker thread)
void setup_controller() {
    g_ble = std::make_unique<NativeBLE::NativeBleController>();

    NativeBLE::CallbackHolder handlers;
    handlers.callback_on_scan_start          = [] { log("scan started"); };
    handlers.callback_on_scan_stop           = [] { log("scan stopped"); };
    handlers.callback_on_scan_found          = on_scan_found;
    handlers.callback_on_device_connected    = on_connected;
    handlers.callback_on_device_disconnected = on_disconnected;

    g_ble->setup(handlers);
    log("BLE controller ready");
}

// ============================================================
// 4. Settings
//    The settings manager. The current settings live in g_config.
//    JS sends a JSON text with the items to change; it is checked and
//    applied here, and the result is reported back through ble_config_cb.
//
//    Demo only: the JSON text is read with regular expressions, not with a
//    real parser. It handles a flat object whose values are true / false.
// ============================================================

struct Config {
    bool rest_api;
    bool debug_on_rest_api;
    bool send_repeat; // periodic message on / off (see section 1)
    bool auto_reconnect; // retry a failed connection attempt by itself (see section 1)
};

std::mutex g_config_mutex;
Config g_config = { REST_API, DEBUG_ON_REST_API, false, AUTO_RECONNECT_DEFAULT }; // starts from the hardcoded values of section 0

const char* const CONFIG_KEYS[] = { "rest_api", "debug_on_rest_api", "send_repeat", "auto_reconnect" };

std::string config_json(const Config& config) {
    std::string json = "{";
    json += std::string("\"rest_api\": ") + (config.rest_api ? "true" : "false") + ", ";
    json += std::string("\"debug_on_rest_api\": ") + (config.debug_on_rest_api ? "true" : "false") + ", ";
    json += std::string("\"send_repeat\": ") + (config.send_repeat ? "true" : "false") + ", ";
    json += std::string("\"auto_reconnect\": ") + (config.auto_reconnect ? "true" : "false");
    json += "}";
    return json;
}

// Send the current settings to JS
void notify_config() {
    std::string json;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        json = config_json(g_config);
    }
    if (g_config_cb) g_config_cb(json.c_str());
}

// Remember the new value of "send_repeat". Returns true if it changed.
// When it is switched on, the first message goes out one interval later.
bool switch_send_repeat(bool on) {
    if (g_send_repeat.exchange(on) == on) return false;
    if (on) post([] { g_hello_next = std::chrono::steady_clock::now() + HELLO_INTERVAL; }); // also wakes the worker
    log(on ? "send repeat: on" : "send repeat: off");
    return true;
}

// Change "send_repeat" from inside the DLL and tell JS, so the screen follows
void set_send_repeat(bool on) {
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_config.send_repeat = on;
    }
    if (switch_send_repeat(on)) notify_config();
}

// Reads  "key": true  or  "key": false  from the text.
//   1 = found (value is set), 0 = the key is not in the text, -1 = the key has another kind of value
int read_bool(const std::string& json, const char* key, bool& value) {
    const std::string quoted = std::string("\"") + key + "\"";
    if (!std::regex_search(json, std::regex(quoted + "\\s*:"))) return 0;

    std::smatch match;
    if (!std::regex_search(json, match, std::regex(quoted + "\\s*:\\s*(true|false)\\s*[,}]"))) return -1;
    value = (match[1] == "true");
    return 1;
}

// Check the JSON text and apply it to g_config. Returns the code of ble_config_set.
int apply_config(const std::string& json) {
    // Settings are small. A limit also keeps the regular expressions cheap.
    if (json.size() > 1024) return -2;
    if (!std::regex_match(json, std::regex("\\s*\\{[\\s\\S]*\\}\\s*"))) return -2;

    // Every key in the text must be a known setting
    const std::regex any_key("\"([^\"]*)\"\\s*:");
    for (std::sregex_iterator it(json.begin(), json.end(), any_key), end; it != end; ++it) {
        bool known = false;
        for (const char* key : CONFIG_KEYS) {
            if ((*it)[1] == key) known = true;
        }
        if (!known) return -3;
    }

    // Work on a copy, so that nothing changes if any item is wrong
    Config next;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        next = g_config;
    }
    if (read_bool(json, "rest_api", next.rest_api) < 0) return -4;
    if (read_bool(json, "debug_on_rest_api", next.debug_on_rest_api) < 0) return -4;
    if (read_bool(json, "send_repeat", next.send_repeat) < 0) return -4;
    if (read_bool(json, "auto_reconnect", next.auto_reconnect) < 0) return -4;

    // A setting may only use what this DLL supports
    if (next.rest_api && !REST_API) return -5;

    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_config = next;
    }
    switch_send_repeat(next.send_repeat);
    if (g_auto_reconnect.exchange(next.auto_reconnect) != next.auto_reconnect) {
        log(next.auto_reconnect ? "auto reconnect: on" : "auto reconnect: off");
        if (!next.auto_reconnect) {
            // Switched off: a retry that is already waiting must not start
            g_retry_cancel = true;
            g_retry_left = 0;
        }
    }
    return 0;
}

// ============================================================
// 5. Pinning
//    Certificate pinning. The DLL trusts only the servers whose public key
//    fingerprint is in PINNED_SHA256 (section 0).
//
//    Demo only: the caller gives the fingerprint as text and it is compared here.
//    There is no TLS connection in this DLL yet; when there is, the fingerprint
//    must be computed inside the DLL from the certificate the server presents.
// ============================================================

// "AB:CD ..." -> "abcd...". Returns false unless the result is exactly 64 hex digits.
bool normalize_fingerprint(const char* text, std::string& out) {
    out.clear();
    for (const char* c = text; *c; ++c) {
        if (*c == ':' || *c == ' ') continue;
        if (out.size() >= 64) return false;
        if (*c >= '0' && *c <= '9') out += *c;
        else if (*c >= 'a' && *c <= 'f') out += *c;
        else if (*c >= 'A' && *c <= 'F') out += static_cast<char>(*c - 'A' + 'a');
        else return false;
    }
    return out.size() == 64;
}

// True if the fingerprint equals one of the pinned ones.
// Every pin is compared in full (no early exit), so the time taken does not
// show how many digits matched.
bool is_pinned(const std::string& fingerprint) {
    bool found = false;
    for (const char* pin : PINNED_SHA256) {
        unsigned char difference = 0;
        for (size_t i = 0; i < 64; ++i) {
            difference |= static_cast<unsigned char>(fingerprint[i] ^ pin[i]);
        }
        if (difference == 0) found = true;
    }
    return found;
}

} // namespace

// ============================================================
// 6. JS -> DLL
//    The functions JS calls through FFI. Each one only puts work into the
//    queue and returns immediately. Results come back through section 3.
//    Return value: 0 = accepted, negative = rejected.
// ============================================================
extern "C" {

// Save the JS callbacks and start the worker thread
int ble_init(ble_log_cb log_cb, ble_rx_cb rx_cb, ble_found_cb found_cb, ble_config_cb config_cb) {
    g_log       = log_cb;
    g_rx        = rx_cb;
    g_found     = found_cb;
    g_config_cb = config_cb;

    // Report what happened at load time (section 0), now that a log callback exists
    log(g_interface_file_created ? std::string(INTERFACE_FILE_NAME) + " created next to the DLL"
                                 : std::string(INTERFACE_FILE_NAME) + " could NOT be created");

    // Tell JS the current settings (section 4)
    notify_config();

    if (g_inited.exchange(true)) return 0; // already started

    { std::lock_guard<std::mutex> lock(g_queue_mutex); g_running = true; }
    g_worker = std::thread(worker_main);
    return 0;
}

int ble_scan_start(void) {
    if (!g_inited) return -1;
    cancel_retry(); // a new scan means the user moved on
    post([] { g_ble->scan_start(); });
    return 0;
}

int ble_scan_stop(void) {
    if (!g_inited) return -1;
    post([] { g_ble->scan_stop(); });
    return 0;
}

int ble_connect(const char* address) {
    if (!g_inited) return -1;
    if (!address) return -2;
    if (g_connected || g_connecting.exchange(true)) return -3; // already connected / connecting

    std::string target = address; // copy: the JS string is gone after this call returns
    {
        std::lock_guard<std::mutex> lock(g_names_mutex);
        g_target_address = lower(target); // used by is_hello_device() once connected
        g_retry_address = target;         // remembered for the automatic retries
    }
    g_retry_cancel = false;
    g_retry_left = CONNECT_RETRY_MAX;
    post([target] {
        g_ble->scan_stop();
        log("connecting: " + target);
        g_attempt_active = true;
        g_ble->connect(target);
    });
    return 0;
}

int ble_disconnect(void) {
    if (!g_inited) return -1;
    cancel_retry(); // right away, so a pending retry does not start
    post([] {
        g_hello_device = false;
        set_send_repeat(false);
        if (g_connected) g_ble->disconnect();
        g_connecting = false;
    });
    return 0;
}

int ble_send(const uint8_t* data, uint32_t len) {
    if (!g_connected) return -1;
    if (!data && len > 0) return -2;

    std::string payload(reinterpret_cast<const char*>(data), len); // copy, same reason as above
    post([payload] {
        if (!g_connected) return;
        g_ble->write_command(SERVICE_UUID, WRITE_CHAR_UUID, payload);

        // Show the printable ASCII part of what was sent, like the periodic message
        std::string ascii;
        for (unsigned char ch : payload) {
            if (ch >= 0x20 && ch <= 0x7E) ascii += static_cast<char>(ch);
        }
        if (!ascii.empty()) log("[ Sent String ] : " + ascii);
    });
    return 0;
}

// Change settings (section 4). Runs on the calling thread and is finished on return.
int ble_config_set(const char* json) {
    if (!json) return -1;

    const std::string text = json; // copy: the JS string is gone after this call returns
    const int result = apply_config(text);
    if (result != 0) {
        log("config rejected (code " + std::to_string(result) + ")");
        return result;
    }
    notify_config();
    return 0;
}

// Certificate pinning check (section 5)
int ble_pin_check(const char* sha256_hex) {
    if (!sha256_hex) return -1;

    std::string fingerprint;
    if (!normalize_fingerprint(sha256_hex, fingerprint)) return -2;

    const bool pinned = is_pinned(fingerprint);
    log(std::string("pin check: ") + (pinned ? "PINNED (trusted)" : "NOT pinned (rejected)"));
    return pinned ? 1 : 0;
}

// Stop the worker thread and forget the JS callbacks
void ble_dispose(void) {
    if (!g_inited.exchange(false)) return;

    { std::lock_guard<std::mutex> lock(g_queue_mutex); g_running = false; }
    g_queue_cv.notify_one();
    if (g_worker.joinable()) g_worker.join();

    g_log   = nullptr;
    g_rx    = nullptr;
    g_found = nullptr;
    g_config_cb = nullptr;
}

} // extern "C"
