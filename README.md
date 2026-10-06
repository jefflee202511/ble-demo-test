# Cross-Platform BLE Library

A cross-platform Bluetooth Low Energy (BLE) library for sending and receiving ASCII data.

This project provides a simple Echo application to test BLE communication between a desktop application (Central/Client) and a BLE device (Peripheral/Server).

The library uses BLE GATT characteristics with Write and Notify to send data and receive responses. Notification subscription is enabled to receive data asynchronously from the peripheral, including in simulator implementations that support this mechanism.

## Features

* Cross-platform native BLE library
* ASCII data transmission and reception
* Simple Echo communication test application included (electron desktop)
* ble-bridge.cpp is main for funtionality source file 

## Requirements

* Node.js and npm
* A supported Bluetooth adapter
* A compatible BLE peripheral

## Build

Run the following command from the project root:

```bash
npm run build:native
```

## Run

Start the application:

```bash
npm start
```

## BLE Echo Test

Send an ASCII message and verify that the expected response is received.

Example:

```text
TX: HELLO BLE
RX: HELLO BLE
```

The response depends on the BLE peripheral's firmware and configuration.

### Test with nRF UART Toolbox

You can also use **nRF UART Toolbox** to test BLE communication with a compatible peripheral.

1. Open nRF UART Toolbox on your mobile device.
2. Connect to a BLE peripheral that supports the Nordic UART Service (NUS).
3. Send an ASCII message through the UART interface.
4. Verify that the received data matches the expected response.

The peripheral must implement the appropriate UART service and data-handling behavior. The application and peripheral must use compatible BLE services and characteristics.

## License

This project is licensed under the [MIT License](https://opensource.org/license/mit/).

You are free to use, modify, and distribute this software under the terms of the MIT License.
