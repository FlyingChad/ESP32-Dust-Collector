#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <LocalConfig.h>

// ============================================================
// USER CONFIGURATION
// ============================================================

struct EquipmentInput {
    const char *name;
    uint8_t pin;
    bool currentRequest;
    bool lastRequest;
    unsigned long lastSendTime;
};

#if defined(EQUIPMENT_PROFILE_TABLE_SAW_PLANER)
EquipmentInput equipment[] = {
  {"Table Saw", D0, false, false, 0},
  {"Planer", D1, false, false, 0}
};
#elif defined(EQUIPMENT_PROFILE_JOINTER)
EquipmentInput equipment[] = {
  {"Jointer", D0, false, false, 0}
};
#elif defined(EQUIPMENT_PROFILE_WORK_TABLE)
EquipmentInput equipment[] = {
  {"Work Table", D0, false, false, 0}
};
#else
EquipmentInput equipment[] = {
  {"CNC Router", D0, false, false, 0}
};
#endif

const size_t EQUIPMENT_COUNT = sizeof(equipment) / sizeof(equipment[0]);


// ------------------------------------------------------------
// MASTER MAC ADDRESS
// ------------------------------------------------------------
//
// This is the MAC address you supplied for your master.
//

// ------------------------------------------------------------
// HEARTBEAT
// ------------------------------------------------------------
//
// Send current state every second even when nothing changes.
//

const unsigned long HEARTBEAT_INTERVAL_MS = 1000;
constexpr uint8_t FIRST_ESPNOW_CHANNEL = 1;
constexpr uint8_t LAST_ESPNOW_CHANNEL = 11;
constexpr unsigned long CHANNEL_SCAN_INTERVAL_MS = 75;
constexpr uint8_t SEND_FAILURES_BEFORE_RESCAN = 3;


// ============================================================
// VARIABLES
// ============================================================

// ============================================================
// ESP-NOW MESSAGE
// ============================================================
//
// MUST exactly match master's message structure.
//

typedef struct struct_message {

  char device[32];

  bool request;
  uint8_t fillPercent;

} struct_message;


struct_message myData;


// ============================================================
// PEER INFORMATION
// ============================================================

esp_now_peer_info_t peerInfo = {};
volatile bool sendResultReady = false;
volatile esp_now_send_status_t lastSendStatus = ESP_NOW_SEND_FAIL;
bool sendInProgress = false;
bool masterFound = false;
uint8_t scanChannel = FIRST_ESPNOW_CHANNEL;
uint8_t activeChannel = FIRST_ESPNOW_CHANNEL;
uint8_t consecutiveSendFailures = 0;
unsigned long lastChannelScanTime = 0;


// ============================================================
// SEND CALLBACK
// ============================================================

void OnDataSent(
    const uint8_t *macAddress,
    esp_now_send_status_t status) {
  lastSendStatus = status;
  sendResultReady = true;
}


// ============================================================
// SEND CURRENT MACHINE STATE
// ============================================================

void sendMachineState(const EquipmentInput &input, uint8_t channel) {
  esp_err_t channelResult = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (channelResult != ESP_OK) {
    Serial.printf("Failed to select ESP-NOW channel %u: %d\n",
                  channel, channelResult);
    lastSendStatus = ESP_NOW_SEND_FAIL;
    sendResultReady = true;
    sendInProgress = true;
    return;
  }

  // Safely copy device name

  strncpy(
      myData.device,
    input.name,
      sizeof(myData.device)
  );

  myData.device[
      sizeof(myData.device) - 1
  ] = '\0';


  // Current machine state

  myData.request =
    input.currentRequest;
  myData.fillPercent = 0;


  // ----------------------------------------------------------
  // SEND ESP-NOW PACKET
  // ----------------------------------------------------------

  sendResultReady = false;
  sendInProgress = true;
  esp_err_t result = esp_now_send(
      MASTER_MAC,
      reinterpret_cast<const uint8_t *>(&myData),
      sizeof(myData));
  if (result != ESP_OK) {
    Serial.printf("ESP-NOW send rejected on channel %u: %d\n",
                  channel, result);
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
    consecutiveSendFailures = 0;
    if (!masterFound) {
      masterFound = true;
      activeChannel = scanChannel;
      equipment[0].lastRequest = equipment[0].currentRequest;
      equipment[0].lastSendTime = now;
      Serial.printf("Master found on ESP-NOW channel %u\n", activeChannel);
    }
    return;
  }

  if (masterFound) {
    if (++consecutiveSendFailures >= SEND_FAILURES_BEFORE_RESCAN) {
      masterFound = false;
      scanChannel = FIRST_ESPNOW_CHANNEL;
      lastChannelScanTime = now - CHANNEL_SCAN_INTERVAL_MS;
      Serial.println("Master lost; restarting ESP-NOW channel search");
    }
  } else {
    scanChannel = scanChannel >= LAST_ESPNOW_CHANNEL
        ? FIRST_ESPNOW_CHANNEL
        : scanChannel + 1;
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  // ----------------------------------------------------------
  // SERIAL DEBUGGING
  // ----------------------------------------------------------

  // Uncomment if desired

  Serial.begin(115200);


  // ----------------------------------------------------------
  // MACHINE REQUEST INPUT
  // ----------------------------------------------------------

  for (size_t index = 0; index < EQUIPMENT_COUNT; ++index) {
    pinMode(equipment[index].pin, INPUT_PULLDOWN);
    equipment[index].currentRequest = digitalRead(equipment[index].pin);
    equipment[index].lastRequest = equipment[index].currentRequest;
  }


  // ----------------------------------------------------------
  // Start Wi-Fi radio without associating to an access point.
  // ----------------------------------------------------------

  WiFi.mode(
      WIFI_STA
  );
  Serial.println("Wi-Fi association disabled; searching ESP-NOW channels 1-11");


  // ----------------------------------------------------------
  // INITIALIZE ESP-NOW
  // ----------------------------------------------------------

  if (esp_now_init() != ESP_OK) {

    Serial.println(
        "ERROR: ESP-NOW initialization failed"
    );

    return;
  }


  // ----------------------------------------------------------
  // REGISTER SEND CALLBACK
  // ----------------------------------------------------------

  esp_now_register_send_cb(
      OnDataSent
  );


  // ----------------------------------------------------------
  // CONFIGURE MASTER AS PEER
  // ----------------------------------------------------------

  memcpy(
      peerInfo.peer_addr,
      MASTER_MAC,
      6
  );


  // Channel zero follows the radio's selected scan channel.
  peerInfo.channel = 0;

  // No ESP-NOW encryption
  peerInfo.encrypt = false;


  // ----------------------------------------------------------
  // ADD MASTER
  // ----------------------------------------------------------

  if (
      esp_now_add_peer(
          &peerInfo
      ) != ESP_OK
  ) {

    Serial.println(
        "ERROR: Failed to add master ESP"
    );

    return;
  }


  // ----------------------------------------------------------
  // Print configured input names before starting the channel search.
  for (size_t index = 0; index < EQUIPMENT_COUNT; ++index) {
    Serial.printf("Monitoring %s on pin %u\n",
                  equipment[index].name,
                  equipment[index].pin);
  }
  lastChannelScanTime = millis() - CHANNEL_SCAN_INTERVAL_MS;
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  unsigned long now = millis();
  for (size_t index = 0; index < EQUIPMENT_COUNT; ++index) {
    equipment[index].currentRequest = digitalRead(equipment[index].pin);
  }

  processSendResult(now);
  if (sendInProgress) {
    return;
  }

  if (!masterFound) {
    if (now - lastChannelScanTime >= CHANNEL_SCAN_INTERVAL_MS) {
      lastChannelScanTime = now;
      sendMachineState(equipment[0], scanChannel);
      equipment[0].lastRequest = equipment[0].currentRequest;
      equipment[0].lastSendTime = now;
    }
    return;
  }

  for (size_t index = 0; index < EQUIPMENT_COUNT; ++index) {
    EquipmentInput &input = equipment[index];
    if (input.currentRequest != input.lastRequest ||
        (now - input.lastSendTime) >= HEARTBEAT_INTERVAL_MS) {
      sendMachineState(input, activeChannel);
      input.lastRequest = input.currentRequest;
      input.lastSendTime = now;
      break;
    }
  }
}