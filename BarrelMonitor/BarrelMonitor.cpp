#include <Adafruit_VL53L0X.h>
#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>
#include <LocalConfig.h>

namespace {

// XIAO ESP32-S3: D4/D5 are the default SDA/SCL pins.
constexpr uint8_t BARREL_ONE_XSHUT_PIN = D0;
constexpr uint8_t BARREL_TWO_XSHUT_PIN = D1;
constexpr uint8_t FULL_ALARM_OUTPUT_PIN = D2;
constexpr uint8_t FULL_ALARM_ACTIVE_LEVEL = HIGH;
constexpr uint8_t FULL_ALARM_INACTIVE_LEVEL = LOW;
constexpr uint8_t BARREL_ONE_I2C_ADDRESS = 0x30;
constexpr uint8_t BARREL_TWO_I2C_ADDRESS = 0x31;

// Measure and adjust these for each installed sensor/barrel.
constexpr uint16_t BARREL_ONE_EMPTY_DISTANCE_MM = 850;
constexpr uint16_t BARREL_TWO_EMPTY_DISTANCE_MM = 850;
constexpr uint16_t BARREL_ONE_FULL_DISTANCE_MM = 150;
constexpr uint16_t BARREL_TWO_FULL_DISTANCE_MM = 150;
constexpr uint16_t FULL_ALARM_HYSTERESIS_MM = 30;
constexpr uint8_t REQUIRED_CONFIRMATION_READINGS = 3;
constexpr unsigned long SENSOR_READ_INTERVAL_MS = 500;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 1000;

struct Message {
  char device[32];
  bool request;
  uint8_t fillPercent;
};

struct Barrel {
  const char *deviceName;
  uint8_t xshutPin;
  uint8_t i2cAddress;
  uint16_t emptyDistanceMm;
  uint16_t fullDistanceMm;
  Adafruit_VL53L0X *sensor;
  bool online;
  bool fullAlarm;
  bool candidateFull;
  uint8_t confirmationReadings;
  uint8_t fillPercent;
  uint16_t distanceMm;
  unsigned long lastReadTime;
  unsigned long lastSendTime;
};

Adafruit_VL53L0X barrelOneSensor;
Adafruit_VL53L0X barrelTwoSensor;

Barrel barrels[] = {
  {"Barrel 1 Full", BARREL_ONE_XSHUT_PIN, BARREL_ONE_I2C_ADDRESS,
   BARREL_ONE_EMPTY_DISTANCE_MM, BARREL_ONE_FULL_DISTANCE_MM,
   &barrelOneSensor, false, false, false, 0, 0, 0, 0, 0},
  {"Barrel 2 Full", BARREL_TWO_XSHUT_PIN, BARREL_TWO_I2C_ADDRESS,
   BARREL_TWO_EMPTY_DISTANCE_MM, BARREL_TWO_FULL_DISTANCE_MM,
   &barrelTwoSensor, false, false, false, 0, 0, 0, 0, 0}
};

constexpr size_t BARREL_COUNT = sizeof(barrels) / sizeof(barrels[0]);
Message message;
esp_now_peer_info_t masterPeer = {};

uint8_t calculateFillPercent(const Barrel &barrel, uint16_t distanceMm) {
  if (distanceMm >= barrel.emptyDistanceMm) {
    return 0;
  }
  if (distanceMm <= barrel.fullDistanceMm) {
    return 100;
  }

  uint32_t filledDistance = barrel.emptyDistanceMm - distanceMm;
  uint32_t usableDistance = barrel.emptyDistanceMm - barrel.fullDistanceMm;
  return static_cast<uint8_t>((filledDistance * 100) / usableDistance);
}

void sendBarrelState(Barrel &barrel) {
  strncpy(message.device, barrel.deviceName, sizeof(message.device));
  message.device[sizeof(message.device) - 1] = '\0';
  message.request = barrel.fullAlarm;
  message.fillPercent = barrel.fillPercent;

  esp_err_t result = esp_now_send(
      MASTER_MAC,
      reinterpret_cast<const uint8_t *>(&message),
      sizeof(message));

  if (result != ESP_OK) {
    Serial.printf("ESP-NOW send failed for %s: %d\n",
                  barrel.deviceName, result);
  }
  barrel.lastSendTime = millis();
}

bool initializeSensor(Barrel &barrel) {
  digitalWrite(barrel.xshutPin, HIGH);
  delay(10);

  if (!barrel.sensor->begin(0x29, false, &Wire)) {
    Serial.printf("VL53L0X not found for %s\n", barrel.deviceName);
    return false;
  }

  barrel.sensor->setAddress(barrel.i2cAddress);
  Serial.printf("%s sensor ready at I2C address 0x%02X\n",
                barrel.deviceName, barrel.i2cAddress);
  return true;
}

void updateFullAlarm(Barrel &barrel, bool shouldBeFull) {
  if (shouldBeFull == barrel.fullAlarm) {
    barrel.candidateFull = barrel.fullAlarm;
    barrel.confirmationReadings = 0;
    return;
  }

  if (shouldBeFull != barrel.candidateFull) {
    barrel.candidateFull = shouldBeFull;
    barrel.confirmationReadings = 1;
  } else if (barrel.confirmationReadings < REQUIRED_CONFIRMATION_READINGS) {
    ++barrel.confirmationReadings;
  }

  if (barrel.confirmationReadings >= REQUIRED_CONFIRMATION_READINGS) {
    barrel.fullAlarm = barrel.candidateFull;
    barrel.confirmationReadings = 0;
    Serial.printf("%s: %s, %u%% full (%u mm)\n",
                  barrel.deviceName,
                  barrel.fullAlarm ? "FULL ALARM" : "clear",
                  barrel.fillPercent,
                  barrel.distanceMm);
  }
}

void updateBarrel(Barrel &barrel, unsigned long now) {
  if (!barrel.online || (now - barrel.lastReadTime) < SENSOR_READ_INTERVAL_MS) {
    return;
  }
  barrel.lastReadTime = now;

  VL53L0X_RangingMeasurementData_t measurement;
  barrel.sensor->rangingTest(&measurement, false);
  if (measurement.RangeStatus == 4 || measurement.RangeMilliMeter == 0) {
    return;
  }

  barrel.distanceMm = measurement.RangeMilliMeter;
  barrel.fillPercent = calculateFillPercent(barrel, barrel.distanceMm);

  bool shouldBeFull = barrel.fullAlarm
      ? barrel.distanceMm <= barrel.fullDistanceMm + FULL_ALARM_HYSTERESIS_MM
      : barrel.distanceMm <= barrel.fullDistanceMm;
  bool previousFull = barrel.fullAlarm;
  updateFullAlarm(barrel, shouldBeFull);

  if (previousFull != barrel.fullAlarm) {
    sendBarrelState(barrel);
  }
}

void updatePhysicalAlarm() {
  bool anyBarrelFull = false;
  for (size_t index = 0; index < BARREL_COUNT; ++index) {
    anyBarrelFull = anyBarrelFull || barrels[index].fullAlarm;
  }

  digitalWrite(FULL_ALARM_OUTPUT_PIN,
               anyBarrelFull ? FULL_ALARM_ACTIVE_LEVEL
                             : FULL_ALARM_INACTIVE_LEVEL);
}

}  // namespace

void setup() {
  Serial.begin(115200);

  pinMode(FULL_ALARM_OUTPUT_PIN, OUTPUT);
  digitalWrite(FULL_ALARM_OUTPUT_PIN, FULL_ALARM_INACTIVE_LEVEL);

  pinMode(BARREL_ONE_XSHUT_PIN, OUTPUT);
  pinMode(BARREL_TWO_XSHUT_PIN, OUTPUT);
  digitalWrite(BARREL_ONE_XSHUT_PIN, LOW);
  digitalWrite(BARREL_TWO_XSHUT_PIN, LOW);
  Wire.begin();

  barrels[0].online = initializeSensor(barrels[0]);
  digitalWrite(BARREL_TWO_XSHUT_PIN, LOW);
  barrels[1].online = initializeSensor(barrels[1]);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected to Wi-Fi on channel %u\n", WiFi.channel());
  } else {
    Serial.println("Wi-Fi unavailable; ESP-NOW channel may not match the master");
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed");
    return;
  }

  memcpy(masterPeer.peer_addr, MASTER_MAC, sizeof(MASTER_MAC));
  masterPeer.channel = 0;
  masterPeer.encrypt = false;
  if (esp_now_add_peer(&masterPeer) != ESP_OK) {
    Serial.println("Failed to add dust collector controller as ESP-NOW peer");
    return;
  }

  unsigned long now = millis();
  for (size_t index = 0; index < BARREL_COUNT; ++index) {
    if (barrels[index].online) {
      sendBarrelState(barrels[index]);
      barrels[index].lastReadTime = now - SENSOR_READ_INTERVAL_MS;
    }
  }
}

void loop() {
  unsigned long now = millis();
  for (size_t index = 0; index < BARREL_COUNT; ++index) {
    Barrel &barrel = barrels[index];
    updateBarrel(barrel, now);
    if (barrel.online && (now - barrel.lastSendTime) >= HEARTBEAT_INTERVAL_MS) {
      sendBarrelState(barrel);
    }
  }
  updatePhysicalAlarm();
}
