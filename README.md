# ESP32-S3 Dust Collection System

PlatformIO/Arduino firmware for Seeed Studio XIAO ESP32-S3 boards. Equipment senders report requests over ESP-NOW; the DustCollector controller operates the gates and collector, serves the local web UI, and receives barrel-level telemetry from a dedicated monitor ESP.

## Requirements

- VS Code with the PlatformIO IDE extension
- Seeed Studio XIAO ESP32-S3 boards
- USB data cable and a serial port for each board

The project uses the `seeed_xiao_esp32s3` PlatformIO board profile. Confirm all GPIO assignments and electrical levels against the actual hardware before wiring.

## Local Credentials

Copy `include/LocalConfig.example.h` to `include/LocalConfig.h` and enter the Wi-Fi SSID, password, and master ESP-NOW MAC address there. `include/LocalConfig.h` is ignored by Git and must not be committed. Keep the example file's placeholders in the repository. All firmware profiles include this local configuration at build time.

## Firmware Profiles

Build one firmware profile per board:

| Profile | Board role | Inputs / outputs |
| --- | --- | --- |
| `cncrouter` | CNC Router sender | Request input `D0` |
| `tablesawplaner` | Table Saw + Planer sender | Table Saw `D0`, Planer `D1` |
| `jointer` | Jointer sender | Request input `D0` |
| `worktable` | Work Table sender | Request input `D0` |
| `dustcollector` | Main controller | Collector `D0`; gates `D1`–`D5` |
| `barrelmonitor` | Two-barrel monitor | VL53L0X sensors and alarm output |

For a command-line build, substitute any profile name above:

```powershell
pio run -e tablesawplaner
```

To upload and monitor, specify the board's COM port:

```powershell
pio run -e tablesawplaner -t upload -t monitor --upload-port COM5
```

VS Code also has profile-specific **build** and **upload and monitor** tasks. Serial Monitor uses 115200 baud; exit it with `Ctrl+]`.

Equipment sender inputs use `INPUT_PULLDOWN`: a sensor must drive its input HIGH when extraction is requested and LOW when idle. The DustCollector relay outputs are active-low by default. Its Work Table gate is on `D5`; the collector output is on `D0`.

## Barrel Monitor Wiring

Use a dedicated XIAO ESP32-S3 with two VL53L0X sensors. Connect both sensors to the same I2C bus (`D4` SDA and `D5` SCL), 3.3 V, and common ground. Connect their XSHUT pins separately to `D0` and `D1`. The firmware initializes them individually and assigns addresses `0x30` and `0x31`.

The physical full-alarm signal is `D2`, active HIGH while either barrel is full. Connect it to a 3.3 V-compatible buzzer/LED driver or relay input. Do not power an alarm load directly from the ESP32 GPIO. Alarm polarity is configurable in `BarrelMonitor/BarrelMonitor.cpp`.

## Barrel Calibration and Alarms

In `BarrelMonitor/BarrelMonitor.cpp`, calibrate each barrel's empty and full sensor-to-dust distances:

- `BARREL_ONE_EMPTY_DISTANCE_MM` and `BARREL_ONE_FULL_DISTANCE_MM`
- `BARREL_TWO_EMPTY_DISTANCE_MM` and `BARREL_TWO_FULL_DISTANCE_MM`

The displayed percentage is a linear height estimate between those distances, not a calibrated volume measurement. The full alarm requires three consecutive readings at the full cutoff and clears after the measured distance increases 30 mm beyond that cutoff.

The monitor sends both barrel percentages and full flags to the main controller over ESP-NOW. The web page refreshes every five seconds and shows each barrel's percentage, a full warning, or `NO DATA` when readings are stale. The barrel alarm does not command the dust collector.

## Main Controller Web UI

The DustCollector controller joins the configured 2.4 GHz Wi-Fi network and prints its IP address to Serial. Open that address in a browser on the same network. The page provides ON/OFF controls for the five blastgates and read-only collector status; it also shows barrel fill percentages and warnings.

Manual gate requests coexist with ESP-NOW machine requests. Opening a gate waits 500 ms before starting the collector. A gate closes when its machine's 15-second off-delay expires if another gate remains open. The last open gate stays open while the collector runs and for five seconds after it stops, allowing the collector to spool down. The web UI has no separate login and is accessible to devices on the local Wi-Fi.

All ESP-NOW devices join the configured 2.4 GHz network so they follow the access point's channel. Each device reports its connected channel over Serial; verify the sender, barrel monitor, and DustCollector controller show the same channel. If a device cannot join Wi-Fi, its ESP-NOW channel may not match and communication is not guaranteed. After changing the ESP-NOW packet format, upload updated firmware to the senders, barrel monitor, and DustCollector controller.
