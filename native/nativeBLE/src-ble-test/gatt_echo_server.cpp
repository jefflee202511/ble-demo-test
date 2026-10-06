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


#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <map>
#include <mutex>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

// ------------------------------------------------------------
// Active Client Device Management (객체 생명주기 유지)
// ------------------------------------------------------------
static std::map<uint64_t, BluetoothLEDevice> g_connectedDevices;
static std::mutex g_deviceMutex;

// UUID Definitions
static const winrt::guid SERVICE_UUID{
    L"{12345678-1234-5678-1234-56789abcdef0}"
};

static const winrt::guid CHARACTERISTIC_UUID{
    L"{12345678-1234-5678-1234-56789abcdef1}"
};

// ------------------------------------------------------------
// Client Tracking Helper
// ------------------------------------------------------------
void TrackDevice(GattSession const& session)
{
    if (session == nullptr) return;

    try
    {
        auto deviceId = session.DeviceId();
        auto device = BluetoothLEDevice::FromIdAsync(deviceId.Id()).get();

        if (device != nullptr)
        {
            uint64_t address = device.BluetoothAddress();

            std::lock_guard<std::mutex> lock(g_deviceMutex);
            if (g_connectedDevices.find(address) != g_connectedDevices.end())
            {
                return; // 이미 등록된 장치
            }

            // operator[] 대신 insert_or_assign 사용 (기본 생성자 미요구)
            g_connectedDevices.insert_or_assign(address, device);

            std::cout << "\n========================================\n";
            std::cout << "[CONNECTED]: Client Device Identified!\n";
            std::cout << "  - Device Name : " << winrt::to_string(device.Name()) << "\n";
            std::cout << "  - Mac Address : 0x" << std::hex << std::uppercase << address << std::dec << "\n";
            std::cout << "========================================\n";

            // 실시간 Disconnect 감지
            device.ConnectionStatusChanged([](BluetoothLEDevice const& devSender, winrt::Windows::Foundation::IInspectable const&)
            {
                if (devSender.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
                {
                    uint64_t addr = devSender.BluetoothAddress();
                    std::cout << "\n========================================\n";
                    std::cout << "[DISCONNECTED]: Client Disconnected!\n";
                    std::cout << "  - Device Name : " << winrt::to_string(devSender.Name()) << "\n";
                    std::cout << "  - Mac Address : 0x" << std::hex << std::uppercase << addr << std::dec << "\n";
                    std::cout << "========================================\n";

                    std::lock_guard<std::mutex> lock(g_deviceMutex);
                    g_connectedDevices.erase(addr);
                }
            });
        }
    }
    catch (...)
    {
    }
}

// ------------------------------------------------------------
// Main Entry
// ------------------------------------------------------------
int main()
{
    SetConsoleOutputCP(CP_UTF8);

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    try
    {
        std::cout << "========================================\n";
        std::cout << "         BLE GATT ECHO SERVER           \n";
        std::cout << "========================================\n";

        // Service 생성
        auto serviceResult = GattServiceProvider::CreateAsync(SERVICE_UUID).get();
        if (serviceResult.Error() != BluetoothError::Success)
        {
            std::cout << "[Error]: Service creation failed\n";
            return 1;
        }

        auto provider = serviceResult.ServiceProvider();
        std::cout << "[Init]: Service created successfully\n";

        // Advertising Status
        provider.AdvertisementStatusChanged([](GattServiceProvider const&, GattServiceProviderAdvertisementStatusChangedEventArgs const& args)
        {
            if (args.Status() == GattServiceProviderAdvertisementStatus::Started)
            {
                std::cout << "[Advertising]: Started (Waiting for Client Connection...)\n";
            }
        });

        // Characteristic 속성 설정
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(
            GattCharacteristicProperties::Read |
            GattCharacteristicProperties::Write |
            GattCharacteristicProperties::WriteWithoutResponse |
            GattCharacteristicProperties::Notify
        );
        parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
        parameters.WriteProtectionLevel(GattProtectionLevel::Plain);

        // Characteristic 생성
        auto characteristicResult = provider.Service().CreateCharacteristicAsync(CHARACTERISTIC_UUID, parameters).get();
        if (characteristicResult.Error() != BluetoothError::Success)
        {
            std::cout << "[Error]: Characteristic creation failed\n";
            return 1;
        }

        auto characteristic = characteristicResult.Characteristic();
        std::cout << "[Init]: Characteristic created successfully\n";

        // 1. 구독(Subscribed) 발생 시 연결 감지
        characteristic.SubscribedClientsChanged([](GattLocalCharacteristic const& sender, winrt::Windows::Foundation::IInspectable const&)
        {
            auto clients = sender.SubscribedClients();
            for (auto const& client : clients)
            {
                TrackDevice(client.Session());
            }
        });

        // 2. Read 요청 처리
        characteristic.ReadRequested([](GattLocalCharacteristic const&, GattReadRequestedEventArgs const& args)
        {
            TrackDevice(args.Session());

            auto deferral = args.GetDeferral();
            auto asyncOp = args.GetRequestAsync();

            asyncOp.Completed([deferral](auto const& asyncInfo, winrt::Windows::Foundation::AsyncStatus status)
            {
                if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
                {
                    GattReadRequest request = asyncInfo.GetResults();
                    if (request != nullptr)
                    {
                        DataWriter writer;
                        writer.WriteString(L"ECHO_OK");
                        request.RespondWithValue(writer.DetachBuffer());
                    }
                }
                deferral.Complete();
            });
        });

        // 3. Write 요청 처리
        characteristic.WriteRequested([](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args)
        {
            TrackDevice(args.Session());

            auto deferral = args.GetDeferral();
            auto asyncOp = args.GetRequestAsync();

            asyncOp.Completed([deferral](auto const& asyncInfo, winrt::Windows::Foundation::AsyncStatus status)
            {
                if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
                {
                    GattWriteRequest request = asyncInfo.GetResults();

                    if (request != nullptr)
                    {
                        IBuffer buffer = request.Value();
                        DataReader reader = DataReader::FromBuffer(buffer);
                        uint32_t length = reader.UnconsumedBufferLength();

                        std::cout << "\n----------------------------------------\n";
                        std::cout << "[Data Received] Length: " << length << " bytes\n";

                        if (length > 0)
                        {
                            std::vector<uint8_t> bytes(length);
                            reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + length));

                            // HEX
                            std::cout << "  - HEX  : ";
                            for (size_t i = 0; i < bytes.size(); ++i)
                            {
                                std::cout << "0x" << std::hex << std::uppercase 
                                          << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]) << " ";
                            }
                            std::cout << std::dec << "\n";

                            // TEXT
                            std::string text(bytes.begin(), bytes.end());
                            std::cout << "  - TEXT : " << text << "\n";
                        }
                        std::cout << "----------------------------------------\n";

                        if (request.Option() == GattWriteOption::WriteWithResponse)
                        {
                            request.Respond();
                        }
                    }
                }

                deferral.Complete();
            });
        });

        // Advertising 시작
        GattServiceProviderAdvertisingParameters advertising;
        advertising.IsDiscoverable(true);
        advertising.IsConnectable(true);

        provider.StartAdvertising(advertising);

        std::cout << "\nServer is Running! Press ENTER to exit...\n\n";
        std::cin.get();

        provider.StopAdvertising();
        std::cout << "Server stopped.\n";
    }
    catch (const winrt::hresult_error& e)
    {
        std::cout << "[WinRT Exception]: " << winrt::to_string(e.message()) << "\n";
        return 1;
    }

    winrt::uninit_apartment();
    return 0;
}


#endif