# ESP32-S3 Project

- This workspace uses PlatformIO with the Arduino framework and targets the Seeed Studio XIAO ESP32-S3.
- Keep equipment sender logic in `Equipment/Equipment.cpp`, dust controller logic in `DustCollector/DustCollector.cpp`, and barrel sensor logic in `BarrelMonitor/BarrelMonitor.cpp`.
- `src/EquipmentSenderEntry.cpp`, `src/DustCollectorEntry.cpp`, and `src/BarrelMonitorEntry.cpp` are PlatformIO entry wrappers for those implementations.
- Keep sender and receiver ESP-NOW packet structures synchronized.
- Do not assume a board-specific LED or GPIO pin without confirming the connected ESP32-S3 board.
