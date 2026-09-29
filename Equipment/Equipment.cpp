#include <esp_now.h>
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


// ============================================================
// SEND CALLBACK
// ============================================================

void OnDataSent(
    const uint8_t *macAddress,
    esp_now_send_status_t status) {

  // Debugging can be enabled here if desired.

  /*
  Serial.print("Packet delivery: ");

  if (status == ESP_NOW_SEND_SUCCESS) {

    Serial.println("SUCCESS");

  }
  else {

    Serial.println("FAILED");

  }
  */
}


// ============================================================
// SEND CURRENT MACHINE STATE
// ============================================================

void sendMachineState(const EquipmentInput &input) {

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

  esp_err_t result =
      esp_now_send(

          MASTER_MAC,

          (uint8_t *)&myData,

          sizeof(myData)
      );


  // ----------------------------------------------------------
  // OPTIONAL DEBUGGING
  // ----------------------------------------------------------

  /*
  Serial.print("Device: ");

  Serial.print(myData.device);

  Serial.print(" | Request: ");

  Serial.print(myData.request);

  Serial.print(" | Local send: ");


  if (result == ESP_OK) {

    Serial.println("OK");

  }
  else {

    Serial.println("ERROR");

  }
  */
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
  // WIFI CONFIGURATION
  // ----------------------------------------------------------

  WiFi.mode(
      WIFI_STA
  );
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - wifiStartTime) < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected to %s on channel %u\n",
                  WIFI_SSID, WiFi.channel());
  } else {
    Serial.println("Wi-Fi unavailable; ESP-NOW channel may not match the master");
  }


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


  // Current WiFi channel
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
  // SEND INITIAL STATE FOR EACH MACHINE
  // ----------------------------------------------------------

  unsigned long now = millis();
  for (size_t index = 0; index < EQUIPMENT_COUNT; ++index) {
    sendMachineState(equipment[index]);
    equipment[index].lastSendTime = now;
    Serial.printf("Monitoring %s on pin %u\n",
                  equipment[index].name,
                  equipment[index].pin);
  }
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    unsigned long now = millis();

    for (size_t index = 0; index < EQUIPMENT_COUNT; ++index) {
        EquipmentInput &input = equipment[index];
        input.currentRequest = digitalRead(input.pin);

        if (input.currentRequest != input.lastRequest ||
                (now - input.lastSendTime) >= HEARTBEAT_INTERVAL_MS) {
            sendMachineState(input);
            input.lastRequest = input.currentRequest;
            input.lastSendTime = now;
        }
    }
}