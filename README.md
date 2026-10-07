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
| `barrelmonitor` | Two-barrel monitor | RCWL-1670 sensors and alarm output |

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

Use a dedicated XIAO ESP32-S3 with two RCWL-1670 sensors. The module's specified working voltage is 3–5 V; power each sensor from the board's 3.3 V pin and connect grounds together. Connect the sensor's `RX`/trigger pin to `D0` for barrel 1 and `D1` for barrel 2; connect its `TX`/echo pin to `D3` for barrel 1 and `D4` for barrel 2. These assignments match the XIAO ESP32-S3 barrel-monitor firmware. Operating at 3.3 V keeps the echo signal within the ESP32-S3 GPIO voltage range. Do not power a sensor at 5 V unless its echo output is level-shifted before connecting to the ESP32.

The physical full-alarm signal is `D2`, active HIGH while either barrel is full. Connect it to a 3.3 V-compatible buzzer/LED driver or relay input. Do not power an alarm load directly from the ESP32 GPIO. Alarm polarity is configurable in `BarrelMonitor/BarrelMonitor.cpp`.

## Barrel Calibration and Alarms

In `BarrelMonitor/BarrelMonitor.cpp`, calibrate each barrel's empty and full sensor-to-dust distances. The monitor triggers each sensor every 500 ms, reads the echo pulse width, and converts it to distance. Measurements outside the RCWL-1670's specified 20–4000 mm range are rejected. Keep measurements at least 50 ms apart:

- `BARREL_ONE_EMPTY_DISTANCE_MM` and `BARREL_ONE_FULL_DISTANCE_MM`
- `BARREL_TWO_EMPTY_DISTANCE_MM` and `BARREL_TWO_FULL_DISTANCE_MM`

The displayed percentage is a linear height estimate between those distances, not a calibrated volume measurement. The full alarm requires three consecutive readings at the full cutoff and clears after the measured distance increases 30 mm beyond that cutoff.

The web UI changes barrel artwork with hysteresis to prevent toggling at level boundaries. Artwork switches upward at 14%, 39%, 64%, 84%, and 96%, and switches downward at 11%, 36%, 61%, 81%, and 93%, respectively. The selected artwork level is retained by the controller until the opposite threshold is crossed; it resets after a controller restart.

The monitor sends both barrel percentages and full flags to the main controller over ESP-NOW. The controller keeps the last received barrel levels in memory and labels them `STALE` whenever live monitor data is unavailable or timed out. Levels are not saved across controller restarts. The barrel monitor logs ESP-NOW delivery failures with the channel used to help diagnose intermittent links. The barrel alarm does not command the dust collector.

## Main Controller Web UI

The DustCollector controller joins the configured 2.4 GHz Wi-Fi network and prints its IP address to Serial. Open that address in a browser on the same network. The page provides ON/OFF controls for the five blastgates and read-only collector status; it also shows barrel fill percentages and warnings.

The barrel cards use the six fill-level illustrations in `data/barrel-levels.png`, selecting the nearest artwork level for each live reading. The web page offsets each sprite cell to keep the barrel centered as the displayed level changes. Upload the filesystem image to the DustCollector board after the first firmware install or whenever the artwork changes: `pio run -e dustcollector -t uploadfs --upload-port COM3`. In VS Code, use **DustCollector: upload filesystem** and select the board's serial port.

Manual gate requests coexist with ESP-NOW machine requests. Opening a gate waits 500 ms before starting the collector. A gate closes when its machine's 15-second off-delay expires if another gate remains open. The last open gate stays open while the collector runs and for five seconds after it stops, allowing the collector to spool down. The web UI has no separate login and is accessible to devices on the local Wi-Fi.

Select **Timer settings** on the controller page to adjust the machine off-delay, gate opening and closing delays, communication timeout, startup Wi-Fi connection timeout, and Wi-Fi retry interval. The default communication timeout is 30 seconds. Values are entered in milliseconds and saved in the controller's non-volatile storage, so they persist across restarts. The communication timeout must be at least one second; gate delays may be set to zero.

All ESP-NOW devices join the configured 2.4 GHz network so they follow the access point's channel. Each device reports its connected channel over Serial; verify the sender, barrel monitor, and DustCollector controller show the same channel. If a device cannot join Wi-Fi, its ESP-NOW channel may not match and communication is not guaranteed. After changing the ESP-NOW packet format, upload updated firmware to the senders, barrel monitor, and DustCollector controller.
