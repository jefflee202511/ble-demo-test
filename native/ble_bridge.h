// ble_bridge.h
// C interface of ble_bridge. These are the only things visible from outside the DLL.
// The declarations in main.js (koffi) must match this file exactly.
#pragma once
#include <stdint.h>

#if defined(_WIN32)
  #define BLE_API __declspec(dllexport)
#else
  #define BLE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- DLL -> JS: callback types (JS registers these with ble_init) ----
typedef void (*ble_log_cb)(const char* text);                        // status / log text
typedef void (*ble_rx_cb)(const uint8_t* data, uint32_t len);        // data received from the device
typedef void (*ble_found_cb)(const char* name, const char* address); // device found by scan
typedef void (*ble_config_cb)(const char* json);                     // current settings, after every change

// ---- JS -> DLL: functions (0 = accepted, negative = rejected) ----

// Register the callbacks and start. Call once at app start.
// The current settings are reported once through config_cb.
BLE_API int ble_init(ble_log_cb log_cb, ble_rx_cb rx_cb, ble_found_cb found_cb, ble_config_cb config_cb);

// Change settings. json holds only the items to change, e.g. {"rest_api": false}
// Applied before this function returns; then the new settings go to ble_config_cb.
//   0 = applied, -1 = null, -2 = not a JSON object (or too long), -3 = unknown item,
//  -4 = wrong value for an item, -5 = not supported by this DLL
// If anything is wrong, nothing is changed.
BLE_API int ble_config_set(const char* json);

// Start / stop scanning. Found devices are reported through ble_found_cb.
BLE_API int ble_scan_start(void);
BLE_API int ble_scan_stop(void);

// Connect to a device found by scan. After connecting, received data is
// reported through ble_rx_cb and written back to the device (echo).
BLE_API int ble_connect(const char* address);

BLE_API int ble_disconnect(void);

// Send arbitrary data to the connected device
BLE_API int ble_send(const uint8_t* data, uint32_t len);

// Certificate pinning: is this server certificate fingerprint one of the pinned ones?
// sha256_hex is the SHA-256 fingerprint as 64 hex digits (upper or lower case,
// ':' and spaces between the digits are allowed).
//   1 = pinned (trusted), 0 = not pinned, -1 = null, -2 = not a SHA-256 fingerprint
BLE_API int ble_pin_check(const char* sha256_hex);

// Stop and release everything. Call once at app exit.
BLE_API void ble_dispose(void);

#ifdef __cplusplus
}
#endif
