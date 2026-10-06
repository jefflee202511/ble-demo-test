#if 0
#include "NativeBleController.h"

#include <iostream>
#include <string>
#include <csignal>
#include <thread>

#define SCAN_TIMEOUT_MS 5000

#define NORDIC_UART_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NORDIC_UART_CHAR_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NORDIC_UART_CHAR_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

NativeBLE::NativeBleController ble;

void signal_handler(int signal) { 
    if (signal == SIGINT) {
        std::cout << std::endl << "User quit program." << std::endl;
    }

    ble.disconnect();
    ble.dispose();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    exit(signal);
}

/**
 * This program will scan for devices and ask you to choose a device to connect to.
 * After connecting, the output of the device will be printed to the terminal, and
 * messages can be sent to the device from the terminal.
 * 
 * Known issues:
 * If while typing, a message is received from the device, your message will be erased
 * from the screen but not from memory, so if you type 'enter' after receiving a message,
 * whatever you were in the process of typing will get sent, even if you can't see it.
 */ 
int main() {
    //Setup signal to disconnect and dispose ble when program is quit.
    std::signal(SIGINT, signal_handler);

    NativeBLE::CallbackHolder                   ble_events;
    std::vector<NativeBLE::DeviceDescriptor>    devices;

    // Setup callback functions
    ble_events.callback_on_device_disconnected = [](std::string msg) {
        std::cout << "Disconnected: " << msg << std::endl;
        return;
    };
    ble_events.callback_on_device_connected = []() {
        std::cout << "Connected" << std::endl;
    };
    ble_events.callback_on_scan_found = [&](NativeBLE::DeviceDescriptor device) {
        static int i = 1;
        std::cout << "\r" << i++ << " devices found.";
        fflush(stdout);
        devices.push_back(device);
    };
    ble_events.callback_on_scan_start = []() {
        std::cout << "Scanning for " << SCAN_TIMEOUT_MS << " ms..." << std::endl;
    };
    ble_events.callback_on_scan_stop = []() {
        std::cout << std::endl << "Scan complete." << std::endl;
    };

    ble.setup(ble_events);
    ble.scan_timeout(SCAN_TIMEOUT_MS);

    std::cout << devices.size() << " devices found:" << std::endl;

    for (int i = 0; i < devices.size(); i++) {
        std::cout << "  " << i << ": " << devices[i].name << " (" << devices[i].address << ")" << std::endl;
    }

    std::cout << "Type index of device to connect to: ";

    int device;
    std::cin >> device;

    if (device >= devices.size()) {
        std::cout << "Device index out of range." << std::endl;
        exit(-1);
    }
    
    ble.connect(devices[device].address);

    // Setup notify for when data is received
    ble.notify(NORDIC_UART_SERVICE_UUID, NORDIC_UART_CHAR_TX, [&](const uint8_t* data, uint32_t length) {
        std::cout << "\r<" << devices[device].name << "> " << "(" << length << ") ";
        for (int i = 0; i < length; i++) { std::cout << data[i]; }
        std::cout << std::endl << " > ";
        fflush(stdout);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    ble.read(NORDIC_UART_SERVICE_UUID, NORDIC_UART_CHAR_TX, [&](const uint8_t* data, uint32_t length) {
        std::cout << "\r<" << devices[device].name << "> " << "(" << length << ") ";
        for (int i = 0; i < length; i++) { std::cout << data[i]; }
        std::cout << std::endl << " > ";
        fflush(stdout);
    });

    // * Endless loop to send/receive BLE UART
    std::string message;
    while (true) {
        getline(std::cin, message);
        ble.write_request(NORDIC_UART_SERVICE_UUID, NORDIC_UART_CHAR_RX, message);
        std::cout << " > ";
    }

    ble.disconnect();
    ble.dispose();
}

#else

#include "NativeBleController.h"

#include <iostream>
#include <vector>
#include <string>
#include <csignal>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>

#define SCAN_TIMEOUT_MS 5000

NativeBLE::NativeBleController ble;
std::atomic<bool> running{true};
std::atomic<bool> connected{false};

std::mutex devices_mutex;
std::vector<NativeBLE::DeviceDescriptor> devices;

void signal_handler(int signal)
{
    running = false;
}

// 안전한 데이터 전송 함수
void send_ble_data(NativeBLE::NativeBleController& ble_inst, 
                   const std::string& service_uuid, 
                   const std::string& char_uuid, 
                   const std::string& data) 
{
    std::cout << "[SEND] Data sending: \"" << data << "\" -> Characteristic: " << char_uuid << std::endl;
    try {
        ble_inst.write_request(service_uuid, char_uuid, data);
        std::cout << "[SEND SUCCESS] Data sent successfully!" << std::endl;
    } 
    catch (const std::exception& e) {
        std::cout << "[SEND ERROR] C++ Exception: " << e.what() << std::endl;
    } 
    catch (...) {
        std::cout << "[SEND ERROR] Unknown error occurred." << std::endl;
    }
}

int main()
{
    std::signal(SIGINT, signal_handler);

    // [스마트폰 시뮬레이터 가상 UUID 설정]
    std::string target_service_uuid = "00001111-0000-1000-8000-00805f9b34fb";
    std::string target_char_uuid    = "00002222-0000-1000-8000-00805f9b34fb";

    NativeBLE::CallbackHolder ble_events;

    ble_events.callback_on_device_connected = []() {
        connected = true;
        std::cout << "\n[CONNECTED] Device connected successfully." << std::endl;
    };

    ble_events.callback_on_device_disconnected = [](std::string msg) {
        connected = false;
        std::cout << "\n[DISCONNECTED] Reason: " << msg << std::endl;
    };

    ble_events.callback_on_scan_found = [](NativeBLE::DeviceDescriptor device) {
        std::lock_guard<std::mutex> lock(devices_mutex);
        devices.push_back(device);
        std::cout << "\r" << devices.size() << " devices found." << std::flush;
    };

    ble_events.callback_on_scan_start = []() {
        std::cout << "Scanning for " << SCAN_TIMEOUT_MS << " ms..." << std::endl;
    };

    ble_events.callback_on_scan_stop = []() {
        std::cout << "\nScan complete." << std::endl;
    };

    ble.setup(ble_events);
    ble.scan_timeout(SCAN_TIMEOUT_MS);

    // 스캔 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(SCAN_TIMEOUT_MS + 500));

    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        if (devices.empty()) {
            std::cout << "No BLE devices found." << std::endl;
            ble.dispose();
            return 0;
        }

        std::cout << std::endl;
        for (size_t i = 0; i < devices.size(); ++i) {
            std::cout << "  " << i << ": " << devices[i].name 
                      << " (" << devices[i].address << ")" << std::endl;
        }
    }

    int device_index = -1;
    std::cout << "\nType index of device to connect to: ";
    std::cin >> device_index;

    std::string target_address;
    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        if (device_index < 0 || device_index >= static_cast<int>(devices.size())) {
            std::cout << "Device index out of range." << std::endl;
            ble.dispose();
            return -1;
        }
        target_address = devices[device_index].address;
        std::cout << "\n[CONNECTING] " << devices[device_index].name << std::endl;
    }

    ble.connect(target_address);

    // 시뮬레이터 서비스 탐색 대기 (7초)
    std::cout << "Waiting for GATT Service Discovery (7s)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(7000));

    if (!connected) {
        std::cout << "[WARNING] Failed to establish connection with simulator." << std::endl;
        ble.dispose();
        return 0;
    }

    std::cout << "\n========================================================" << std::endl;
    std::cout << " Interactive Console Mode Enabled!" << std::endl;
    std::cout << " Type any text (e.g., 'a', 'b', 'c', 'd') and press Enter." << std::endl;
    std::cout << " Type 'exit' or 'quit' to close the program." << std::endl;
    std::cout << "========================================================\n" << std::endl;

    std::string input_buffer;
    
    // std::cin 버퍼에 남아있는 줄바꿈 문자 제거
    std::cin.ignore(1000, '\n');

    while (running && connected) {
        std::cout << "Send Data > ";
        if (!std::getline(std::cin, input_buffer)) {
            break;
        }

        if (input_buffer == "exit" || input_buffer == "quit") {
            std::cout << "Exiting application by user command." << std::endl;
            break;
        }

        if (input_buffer.empty()) {
            continue;
        }

        // 입력받은 a, b, c, d 등 문자열 데이터 전송
        send_ble_data(ble, target_service_uuid, target_char_uuid, input_buffer);
    }

    std::cout << "\n[EXITING] Disconnecting and cleaning up..." << std::endl;
    ble.disconnect();
    ble.dispose();

    return 0;
}

#endif