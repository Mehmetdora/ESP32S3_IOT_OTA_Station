# ESP32-S3 Weather Station + OTA [mehmetdora.me(Live here)](https://www.mehmetdora.me/)



An embedded project built on the ESP32-S3 (XIAO) with an AHT15 sensor. It measures temperature and humidity, sends the data over Wi-Fi to my own Laravel server via HTTPS, and can securely update its own firmware remotely. Written in C++ with ESP-IDF and FreeRTOS.

## How It Works

Three independent FreeRTOS tasks run on the device:

| Task | Responsibility |
|---|---|
| `sensor_task` | Reads the AHT15 every 10 seconds, adds a timestamp, and pushes the sample to a queue |
| `telemetry_task` | Takes samples from the queue and sends them to the server over HTTPS |
| `ota_task` | Handles update checks, firmware download, heartbeat, and rollback |

A queue sits between the sensor and the network. Even if the network is slow or down, the sensor task never blocks, so the measurement loop stays intact.

Before measuring, the device waits for a Wi-Fi connection and time synchronization, so every sample carries a valid timestamp. The time zone is set to UTC+3 and timestamps are sent to the server with a `+0300` offset.

## Features

### Sensor
- The AHT15 driver is written from scratch following the datasheet, using the ESP-IDF 5.x `i2c_master` API.
- Measurement trigger, ready-status check, and conversion of raw data to temperature and humidity are all handled inside the driver.

### Telemetry
- Temperature, humidity, and measurement time are sent to the server over HTTPS.
- The timestamp is taken at measurement time, not at send time, so it stays accurate even if delivery is delayed.

### OTA Updates
The device asks the server for updates by reporting its current version. If a newer version exists, the server returns the download URL and a SHA-256 hash.

- **Secure transport:** Both the API and download URLs must use `https://`, and certificates are verified with the ESP-IDF certificate bundle. Redirects (up to 3) are checked one by one.
- **Streaming download:** Firmware is downloaded in 4 KB chunks. Each chunk is written to flash and fed into the SHA-256 computation at the same time, so the whole image never has to fit in RAM.
- **Integrity check:** If the hash does not match the one provided by the server, the boot partition is left unchanged and the device keeps running the old firmware.
- **Automatic rollback:** A freshly updated firmware starts as unverified. It is confirmed once it communicates successfully with the server; if it fails to do so within 5 minutes, the device rolls back to the previous version.
- **Loop protection:** The last attempted version is stored in NVS, so a bad version that was rolled back is not downloaded again and again.

### Device Monitoring
- The device ID and the running firmware version are reported to the server regularly (heartbeat), so the server can see whether a device is online and which version it runs.