#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <LocalConfig.h>

namespace {

constexpr uint8_t BARREL_ONE_TRIGGER_PIN = D0;
constexpr uint8_t BARREL_ONE_ECHO_PIN = D3;
constexpr uint8_t BARREL_TWO_TRIGGER_PIN = D1;
constexpr uint8_t BARREL_TWO_ECHO_PIN = D4;
constexpr uint8_t FULL_ALARM_OUTPUT_PIN = D2;
constexpr uint8_t FULL_ALARM_ACTIVE_LEVEL = HIGH;
constexpr uint8_t FULL_ALARM_INACTIVE_LEVEL = LOW;
constexpr uint8_t FIRST_ESPNOW_CHANNEL = 1;
constexpr uint8_t LAST_ESPNOW_CHANNEL = 11;
constexpr unsigned long CHANNEL_SCAN_INTERVAL_MS = 75;
constexpr uint8_t SEND_FAILURES_BEFORE_RESCAN = 3;

// Measure and adjust these for each installed sensor/barrel.
constexpr uint16_t BARREL_ONE_EMPTY_DISTANCE_MM = 850;
constexpr uint16_t BARREL_TWO_EMPTY_DISTANCE_MM = 850;
constexpr uint16_t BARREL_ONE_FULL_DISTANCE_MM = 150;
constexpr uint16_t BARREL_TWO_FULL_DISTANCE_MM = 150;
constexpr uint16_t FULL_ALARM_HYSTERESIS_MM = 30;
constexpr uint8_t REQUIRED_CONFIRMATION_READINGS = 3;
constexpr unsigned long SENSOR_READ_INTERVAL_MS = 500;
constexpr unsigned long SENSOR_MIN_CYCLE_INTERVAL_MS = 50;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 1000;
constexpr unsigned long ECHO_TIMEOUT_US = 40000;
constexpr unsigned long TRIGGER_PULSE_US = 10;
constexpr uint16_t SENSOR_MIN_DISTANCE_MM = 20;
constexpr uint16_t SENSOR_MAX_DISTANCE_MM = 4000;

static_assert(SENSOR_READ_INTERVAL_MS >= SENSOR_MIN_CYCLE_INTERVAL_MS,
              "RCWL-1670 measurements must be at least 50 ms apart");

struct Message {
  char device[32];
  bool request;
  uint8_t fillPercent;
};

struct Barrel {
  const char *deviceName;
  uint8_t triggerPin;
  uint8_t echoPin;
  uint16_t emptyDistanceMm;
  uint16_t fullDistanceMm;
  bool fullAlarm;
  bool candidateFull;
  uint8_t confirmationReadings;
  uint8_t fillPercent;
  uint16_t distanceMm;
  bool needsSend;
  unsigned long lastReadTime;
  unsigned long lastSendTime;
};

Barrel barrels[] = {
  {"Barrel 1 Full", BARREL_ONE_TRIGGER_PIN, BARREL_ONE_ECHO_PIN,
   BARREL_ONE_EMPTY_DISTANCE_MM, BARREL_ONE_FULL_DISTANCE_MM,
   false, false, 0, 0, 0, false, 0, 0},
  {"Barrel 2 Full", BARREL_TWO_TRIGGER_PIN, BARREL_TWO_ECHO_PIN,
   BARREL_TWO_EMPTY_DISTANCE_MM, BARREL_TWO_FULL_DISTANCE_MM,
   false, false, 0, 0, 0, false, 0, 0}
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

  //Serial.printf("ESP-NOW delivery failed for %s on channel %u\n",
  //              barrels[activeBarrelIndex].deviceName, lastAttemptChannel);
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
  if ((now - barrel.lastReadTime) < SENSOR_READ_INTERVAL_MS) {
    return;
  }
  barrel.lastReadTime = now;

  digitalWrite(barrel.triggerPin, LOW);
  delayMicroseconds(2);
  digitalWrite(barrel.triggerPin, HIGH);
  delayMicroseconds(TRIGGER_PULSE_US);
  digitalWrite(barrel.triggerPin, LOW);

  unsigned long echoDurationUs = pulseIn(
      barrel.echoPin, HIGH, ECHO_TIMEOUT_US);
  if (echoDurationUs == 0) {
    Serial.printf("%s: no echo within %lu ms\n",
                  barrel.deviceName, ECHO_TIMEOUT_US / 1000);
    return;
  }
  uint32_t distanceMm = (echoDurationUs * 343UL + 1000UL) / 2000UL;
  if (distanceMm < SENSOR_MIN_DISTANCE_MM ||
      distanceMm > SENSOR_MAX_DISTANCE_MM) {
    Serial.printf("%s: ignoring out-of-range distance (%lu mm; valid range %u-%u mm)\n",
                  barrel.deviceName,
                  static_cast<unsigned long>(distanceMm),
                  SENSOR_MIN_DISTANCE_MM,
                  SENSOR_MAX_DISTANCE_MM);
    return;
  }

  barrel.distanceMm = static_cast<uint16_t>(distanceMm);
  barrel.fillPercent = calculateFillPercent(barrel, barrel.distanceMm);
  Serial.printf("%s: %u mm, %u%% full, echo %lu us\n",
                barrel.deviceName,
                barrel.distanceMm,
                barrel.fillPercent,
                echoDurationUs);

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
  Serial.println("RCWL-1670 trigger/echo: barrel 1 RX=D0 TX=D3; barrel 2 RX=D1 TX=D4");

  pinMode(FULL_ALARM_OUTPUT_PIN, OUTPUT);
  digitalWrite(FULL_ALARM_OUTPUT_PIN, FULL_ALARM_INACTIVE_LEVEL);

  for (size_t index = 0; index < BARREL_COUNT; ++index) {
    pinMode(barrels[index].triggerPin, OUTPUT);
    digitalWrite(barrels[index].triggerPin, LOW);
    pinMode(barrels[index].echoPin, INPUT);
  }

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
    sendBarrelState(barrels[index]);
    barrels[index].lastReadTime = now - SENSOR_READ_INTERVAL_MS;
  }
  lastChannelScanTime = now - CHANNEL_SCAN_INTERVAL_MS;
}

void loop() {
  unsigned long now = millis();
  for (size_t index = 0; index < BARREL_COUNT; ++index) {
    Barrel &barrel = barrels[index];
    updateBarrel(barrel, now);
    if ((now - barrel.lastSendTime) >= HEARTBEAT_INTERVAL_MS) {
      sendBarrelState(barrel);
    }
  }

  processSendResult(now);
  if (!sendInProgress) {
    size_t pendingBarrelIndex = BARREL_COUNT;
    for (size_t index = 0; index < BARREL_COUNT; ++index) {
      if (barrels[index].needsSend) {
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
