#include <Adafruit_VL53L0X.h>
#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <LocalConfig.h>

namespace {

// XIAO ESP32-S3: D4/D5 are the default SDA/SCL pins.
constexpr uint8_t BARREL_ONE_XSHUT_PIN = D0;
constexpr uint8_t BARREL_TWO_XSHUT_PIN = D1;
constexpr uint8_t FULL_ALARM_OUTPUT_PIN = D2;
constexpr uint8_t FULL_ALARM_ACTIVE_LEVEL = HIGH;
constexpr uint8_t FULL_ALARM_INACTIVE_LEVEL = LOW;
constexpr uint8_t FIRST_ESPNOW_CHANNEL = 1;
constexpr uint8_t LAST_ESPNOW_CHANNEL = 11;
constexpr unsigned long CHANNEL_SCAN_INTERVAL_MS = 75;
constexpr uint8_t SEND_FAILURES_BEFORE_RESCAN = 3;
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
  bool needsSend;
  unsigned long lastReadTime;
  unsigned long lastSendTime;
};

Adafruit_VL53L0X barrelOneSensor;
Adafruit_VL53L0X barrelTwoSensor;

Barrel barrels[] = {
  {"Barrel 1 Full", BARREL_ONE_XSHUT_PIN, BARREL_ONE_I2C_ADDRESS,
   BARREL_ONE_EMPTY_DISTANCE_MM, BARREL_ONE_FULL_DISTANCE_MM,
    &barrelOneSensor, false, false, false, 0, 0, 0, 0, 0, 0},
  {"Barrel 2 Full", BARREL_TWO_XSHUT_PIN, BARREL_TWO_I2C_ADDRESS,
   BARREL_TWO_EMPTY_DISTANCE_MM, BARREL_TWO_FULL_DISTANCE_MM,
    &barrelTwoSensor, false, false, false, 0, 0, 0, 0, 0, 0}
};

constexpr size_t BARREL_COUNT = sizeof(barrels) / sizeof(barrels[0]);
Message message;
esp_now_peer_info_t masterPeer = {};
  volatile bool sendResultReady = false;
  volatile esp_now_send_status_t lastSendStatus = ESP_NOW_SEND_FAIL;
  bool sendInProgress = false;
  bool masterFound = false;
  uint8_t scanChannel = FIRST_ESPNOW_CHANNEL;
  uint8_t activeChannel = FIRST_ESPNOW_CHANNEL;
  uint8_t consecutiveSendFailures = 0;
  size_t activeBarrelIndex = 0;
  uint8_t lastAttemptChannel = FIRST_ESPNOW_CHANNEL;
  unsigned long lastChannelScanTime = 0;

void onDataSent(const uint8_t *macAddress, esp_now_send_status_t status) {
    lastSendStatus = status;
    sendResultReady = true;
}

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
  barrel.needsSend = true;
}

void startBarrelSend(size_t barrelIndex, uint8_t channel) {
  lastAttemptChannel = channel;
  esp_err_t channelResult = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (channelResult != ESP_OK) {
    Serial.printf("Failed to select ESP-NOW channel %u: %d\n",
                  channel, channelResult);
    lastSendStatus = ESP_NOW_SEND_FAIL;
    sendResultReady = true;
    sendInProgress = true;
    activeBarrelIndex = barrelIndex;
    return;
  }

  Barrel &barrel = barrels[barrelIndex];
  strncpy(message.device, barrel.deviceName, sizeof(message.device));
  message.device[sizeof(message.device) - 1] = '\0';
  message.request = barrel.fullAlarm;
  message.fillPercent = barrel.fillPercent;

  activeBarrelIndex = barrelIndex;
  sendResultReady = false;
  sendInProgress = true;
  esp_err_t result = esp_now_send(
      MASTER_MAC,
      reinterpret_cast<const uint8_t *>(&message),
      sizeof(message));
  if (result != ESP_OK) {
    Serial.printf("ESP-NOW send rejected for %s on channel %u: %d\n",
                  barrel.deviceName, channel, result);
    lastSendStatus = ESP_NOW_SEND_FAIL;
    sendResultReady = true;
  }
}

void processSendResult(unsigned long now) {
  if (!sendInProgress || !sendResultReady) {
    return;
  }

  sendInProgress = false;
  sendResultReady = false;

  if (lastSendStatus == ESP_NOW_SEND_SUCCESS) {
    barrels[activeBarrelIndex].needsSend = false;
    barrels[activeBarrelIndex].lastSendTime = now;
    consecutiveSendFailures = 0;
    if (!masterFound) {
      masterFound = true;
      activeChannel = scanChannel;
      Serial.printf("Master found on ESP-NOW channel %u\n", activeChannel);
    }
    Serial.println("ESP-NOW delivery to master: success");
    return;
  }

  Serial.printf("ESP-NOW delivery failed for %s on channel %u\n",
                barrels[activeBarrelIndex].deviceName, lastAttemptChannel);
  if (masterFound) {
    if (++consecutiveSendFailures >= SEND_FAILURES_BEFORE_RESCAN) {
      masterFound = false;
      scanChannel = FIRST_ESPNOW_CHANNEL;
      Serial.println("Master lost; restarting ESP-NOW channel search");
    }
  } else {
    scanChannel = scanChannel >= LAST_ESPNOW_CHANNEL
        ? FIRST_ESPNOW_CHANNEL
        : scanChannel + 1;
    if (scanChannel == FIRST_ESPNOW_CHANNEL) {
      Serial.println("Master not found on channels 1-11; restarting search");
    }
  }
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
    Serial.printf("%s: invalid range, status %u, distance %u mm\n",
                  barrel.deviceName,
                  measurement.RangeStatus,
                  measurement.RangeMilliMeter);
    return;
  }

  barrel.distanceMm = measurement.RangeMilliMeter;
  barrel.fillPercent = calculateFillPercent(barrel, barrel.distanceMm);
  Serial.printf("%s: %u mm, %u%% full, range status %u\n",
                barrel.deviceName,
                barrel.distanceMm,
                barrel.fillPercent,
                measurement.RangeStatus);

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
  Serial.println("Barrel monitor starting");
  Serial.println("XSHUT pins: barrel 1=D0, barrel 2=D1; I2C: SDA=D4, SCL=D5");

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
  Serial.println("Wi-Fi association disabled; searching ESP-NOW channels 1-11");

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed");
    return;
  }
  Serial.println("ESP-NOW initialized");
  esp_now_register_send_cb(onDataSent);

  memcpy(masterPeer.peer_addr, MASTER_MAC, sizeof(MASTER_MAC));
  masterPeer.channel = 0;
  masterPeer.encrypt = false;
  Serial.printf("Target master MAC: %02X:%02X:%02X:%02X:%02X:%02X; packet size: %u bytes\n",
                MASTER_MAC[0], MASTER_MAC[1], MASTER_MAC[2],
                MASTER_MAC[3], MASTER_MAC[4], MASTER_MAC[5],
                static_cast<unsigned>(sizeof(message)));
  if (esp_now_add_peer(&masterPeer) != ESP_OK) {
    Serial.println("Failed to add dust collector controller as ESP-NOW peer");
    return;
  }
  Serial.println("Dust collector controller added as ESP-NOW peer");

  unsigned long now = millis();
  for (size_t index = 0; index < BARREL_COUNT; ++index) {
    if (barrels[index].online) {
      sendBarrelState(barrels[index]);
      barrels[index].lastReadTime = now - SENSOR_READ_INTERVAL_MS;
    }
  }
  lastChannelScanTime = now - CHANNEL_SCAN_INTERVAL_MS;
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

  processSendResult(now);
  if (!sendInProgress) {
    size_t pendingBarrelIndex = BARREL_COUNT;
    for (size_t index = 0; index < BARREL_COUNT; ++index) {
      if (barrels[index].online && barrels[index].needsSend) {
        pendingBarrelIndex = index;
        break;
      }
    }

    if (pendingBarrelIndex < BARREL_COUNT &&
        (masterFound ||
         now - lastChannelScanTime >= CHANNEL_SCAN_INTERVAL_MS)) {
      if (!masterFound) {
        lastChannelScanTime = now;
      }
      startBarrelSend(pendingBarrelIndex,
                      masterFound ? activeChannel : scanChannel);
    }
  }
  updatePhysicalAlarm();
}
