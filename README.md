# Zephyr BLE Accelerometer Logger

## Overview
This project is a Zephyr RTOS-based firmware that continuously reads accelerometer data from an ST LIS2DH sensor, logs it locally to a W25Q16DV SPI NOR flash memory, and broadcasts the live telemetry over Bluetooth Low Energy (BLE) to connected and authenticated clients. 

The firmware is designed with highly independent subsystems: the flash logger runs continuously on every valid sample regardless of the BLE state, while the BLE subsystem securely streams live data whenever an authenticated device is connected and in range.

---

## Hardware Requirements
* **MCU:** Any Zephyr-supported board (e.g., nRF52 series)
* **Accelerometer:** ST LIS2DH (I2C/SPI)
* **External Flash:** Winbond W25Q16DV (SPI)

---

## Key Features
* **Independent Local Logging:** Saves 16-byte X/Y/Z coordinate records to flash memory every 500ms.
* **Flash Wear Leveling & Resume:** Employs a binary search on boot to find the next empty slot in flash, preventing data overwrite on reset.
* **Secure BLE Streaming:** Requires Level 3 Security (Passkey Authentication) before live streaming data.
* **Automated MTU Exchange:** Negotiates MTU upon connection and begins streaming immediately.
* **Sensor Sanity Checking:** Filters out NaN/Inf values and sensor noise (limits capped at 200.0 m/s²).

---

## Bluetooth LE Profile

### Identification
* **Device Name:** Defined by `CONFIG_BT_DEVICE_NAME` in `prj.conf`
* **Custom MAC Address:** `DE:AD:BE:AF:BA:11`
* **Passkey (Static):** `123456`

### Custom GATT Service
* **Service UUID:** `12345678-1234-5678-1234-56789abcdef0`
* **Characteristic UUID:** `12345678-1234-5678-1234-56789abcdef1`
    * **Properties:** `READ` | `NOTIFY`
    * **Permissions:** `READ_AUTHEN` | `WRITE_AUTHEN` (for CCC descriptor)
    * **Payload Format:** String formatted as `"X:0.00 Y:0.00 Z:0.00"`

---

## Flash Memory Map
Data is written sequentially in 16-byte padded records to align with NOR flash requirements and prevent unaligned writes.

* **Total Flash Size:** `0x200000` (2 MB)
* **Sector Size:** `4096` bytes
* **Data Start Address:** `0x005000`
* **Record Structure:**
    * `float x` (4 bytes)
    * `float y` (4 bytes)
    * `float z` (4 bytes)
    * `uint8_t padding` (4 bytes of `0xFF`)

*Note: If the flash memory fills up, the pointer wraps back to `FLASH_DATA_START` and begins erasing sectors.*

---

## Software Dependencies
Ensure the following are enabled in your `prj.conf`:
* `CONFIG_BT=y`
* `CONFIG_BT_PERIPHERAL=y`
* `CONFIG_BT_SMP=y` (Security Manager Protocol)
* `CONFIG_BT_PRIVACY=y`
* `CONFIG_BT_GATT_CLIENT=y`
* `CONFIG_SENSOR=y`
* `CONFIG_FLASH=y`
* `CONFIG_SETTINGS=y`

Make sure your Devicetree (`.overlay` or `.dts`) correctly configures the `st,lis2dh` and `w25q16dv` nodes, assigning the alias `w25q16dv` for the flash device.

---

## Build & Flash Instructions

1.  **Initialize the build environment:**
    ```bash
    source zephyr-env.sh
    ```

2.  **Build the application:**
    ```bash
    west build -b <your_board_name>
    ```

3.  **Flash to the board:**
    ```bash
    west flash
    ```

## Usage
1. Power on the device. It will immediately begin sampling the accelerometer and logging to flash.
2. Use a BLE client app (like nRF Connect).
3. Connect to the device (`DE:AD:BE:AF:BA:11`).
4. The device will prompt for a passkey. Enter `123456`.
5. Once bonded and MTU is negotiated, the device will automatically start streaming accelerometer string payloads via Notifications on the custom characteristic.
