#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LocalConfig.h>

// ============================================================
// USER CONFIGURATION
// ============================================================

// -------------------------
// Output pins
// -------------------------

const int ROUTER_SOL_PIN     = D1;
const int TABLE_SAW_SOL_PIN  = D2;
const int JOINTER_SOL_PIN    = D3;
const int PLANER_SOL_PIN     = D4;
const int WORK_TABLE_SOL_PIN = D5;

// CHANGE IF NEEDED
const int DUST_COLLECTOR_PIN = D0;

// -------------------------
// Output polarity
// -------------------------
//
// Your original hardware used:
//
// LOW  = output ON
// HIGH = output OFF
//
// This is common with active-low relay boards.
//

const int OUTPUT_ON  = LOW;
const int OUTPUT_OFF = HIGH;


// -------------------------
// Timing
// -------------------------

// Keep extraction running after machine turns off
const unsigned long OFF_DELAY_MS = 15000;

// Allow gate time to open before starting collector
const unsigned long GATE_OPEN_DELAY_MS = 500;

// After collector turns off, wait before closing gates
const unsigned long GATE_CLOSE_DELAY_MS = 5000;

// Sender sends heartbeat every 1 second.
// Declare communication lost after 5 seconds.
const unsigned long COMM_TIMEOUT_MS = 5000;
constexpr uint8_t MASTER_WIFI_CHANNEL = 11;


// ============================================================
// ESP-NOW MESSAGE FORMAT
// ============================================================
//
// IMPORTANT:
// This structure MUST match the sender structure.
//

typedef struct struct_message {
  char device[32];
  bool request;
  uint8_t fillPercent;
} struct_message;


// ============================================================
// MACHINE STATE
// ============================================================

struct MachineState {

  // ESP-NOW requested state
  volatile bool request;

  // Has the master received at least one message?
  volatile bool hasReceivedPacket;

  // Time most recent packet arrived
  volatile unsigned long lastPacketTime;

  // Previous processed request state
  bool lastRequest;

  // Cleanup timer
  bool offDelayActive;
  unsigned long offTime;

  // Gate state requested by logic
  bool gateRequired;

  // Time gate was commanded open
  unsigned long gateOpenTime;
};


// ============================================================
// CREATE MACHINES
// ============================================================

MachineState router = {
  false, false, 0,
  false,
  false, 0,
  false, 0
};

MachineState tableSaw = {
  false, false, 0,
  false,
  false, 0,
  false, 0
};

MachineState jointer = {
  false, false, 0,
  false,
  false, 0,
  false, 0
};

MachineState planer = {
  false, false, 0,
  false,
  false, 0,
  false, 0
};

MachineState workTable = {
  false, false, 0,
  false,
  false, 0,
  false, 0
};

struct BarrelStatus {
  volatile bool full;
  volatile uint8_t fillPercent;
  volatile bool hasReceivedPacket;
  volatile unsigned long lastPacketTime;
};

BarrelStatus barrelOne = {false, 0, false, 0};
BarrelStatus barrelTwo = {false, 0, false, 0};


// ============================================================
// DUST COLLECTOR STATE
// ============================================================

bool dustCollectorOn = false;

unsigned long dustCollectorOffTime = 0;

WebServer server(80);
bool webServerStarted = false;
bool manualRouterOn = false;
bool manualTableSawOn = false;
bool manualJointerOn = false;
bool manualPlanerOn = false;
bool manualWorkTableOn = false;
unsigned long lastWiFiAttempt = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;


// ============================================================
// WEB SERVER
// ============================================================

void addControl(String &page, const char *name, const char *device,
                bool manualOn, bool outputOn) {
  page += "<section><h2>";
  page += name;
  page += "</h2><p>Output: <strong>";
  page += outputOn ? "ON" : "OFF";
  page += "</strong> | Manual: <strong>";
  page += manualOn ? "ON" : "OFF";
  page += "</strong></p><form method='post' action='/control'>";
  page += "<input type='hidden' name='device' value='";
  page += device;
  page += "'><button name='state' value='on'>ON</button> ";
  page += "<button name='state' value='off'>OFF</button></form></section>";
}

void addBarrelStatus(String &page, const char *name, BarrelStatus &barrel,
                     unsigned long now) {
  page += "<section class='barrel'><h2>";
  page += name;
  page += "</h2><p>Status: <strong class='";

  if (!barrel.hasReceivedPacket ||
      (now - barrel.lastPacketTime) > COMM_TIMEOUT_MS) {
    page += "unknown'>NO DATA";
  } else {
    page += barrel.full ? "full'>" : "clear'>";
    page += String(barrel.fillPercent);
    page += "% full";
    if (barrel.full) {
      page += " - FULL";
    }
  }

  page += "</strong></p></section>";
}

void appendHeartbeatAge(String &page, unsigned long ageMs) {
  unsigned long elapsedSeconds = ageMs / 1000;
  if (elapsedSeconds < 60) {
    page += String(elapsedSeconds);
    page += "s";
    return;
  }

  unsigned long elapsedMinutes = elapsedSeconds / 60;
  if (elapsedMinutes < 60) {
    page += String(elapsedMinutes);
    page += "m";
    return;
  }

  page += String(elapsedMinutes / 60);
  page += "h";
  unsigned long remainingMinutes = elapsedMinutes % 60;
  if (remainingMinutes > 0) {
    page += " ";
    page += String(remainingMinutes);
    page += "m";
  }
}

void addDeviceStatus(String &page, const char *name,
                     bool firstHasPacket, unsigned long firstPacketTime,
                     bool secondHasPacket, unsigned long secondPacketTime,
                     unsigned long now) {
  bool hasPacket = firstHasPacket || secondHasPacket;
  unsigned long packetAge = firstHasPacket ? now - firstPacketTime : 0;
  if (secondHasPacket &&
      (!firstHasPacket || now - secondPacketTime < packetAge)) {
    packetAge = now - secondPacketTime;
  }

  page += "<div class='device-status'><span>";
  page += name;
  page += "</span><strong class='";
  if (!hasPacket) {
    page += "waiting'>WAITING";
  } else if (packetAge <= COMM_TIMEOUT_MS) {
    page += "online'>ONLINE";
  } else {
    page += "offline'>OFFLINE - last heard ";
    appendHeartbeatAge(page, packetAge);
    page += " ago";
  }
  page += "</strong></div>";
}

void handleRoot() {
  String page = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<meta http-equiv='refresh' content='5'><title>Dust Collector</title>";
  page += "<style>body{font:18px sans-serif;max-width:680px;margin:24px auto;padding:0 16px;background:#f3f5f4;color:#17221d}section{padding:12px 0;border-bottom:1px solid #bac5bf}.collector-status{padding:12px 0;font-size:20px}.device-status{display:flex;justify-content:space-between;gap:16px;padding:6px 0}.device-status strong{min-width:0;text-align:right}.online,.barrel .clear{color:#176b4a}.offline,.barrel .full{color:#b42318}.waiting,.barrel .unknown{color:#59645e}.barrel .clear,.barrel .full,.barrel .unknown{min-width:3em;display:inline-block}button{font-size:18px;padding:10px 24px;margin:4px;border:0;border-radius:4px;background:#176b4a;color:white}button[value=off]{background:#59645e}strong{min-width:3em;display:inline-block}</style>";
  page += "</head><body><h1>Dust Collector Controls</h1><div class='collector-status'>Dust collector status: <strong>";
  page += dustCollectorOn ? "ON" : "OFF";
  page += "</strong></div>";
  unsigned long now = millis();
  page += "<section><h2>ESP-NOW Devices</h2><div class='device-list'>";
  addDeviceStatus(page, "CNC Router", router.hasReceivedPacket,
                  router.lastPacketTime, false, 0, now);
  addDeviceStatus(page, "Table Saw + Planer", tableSaw.hasReceivedPacket,
                  tableSaw.lastPacketTime, planer.hasReceivedPacket,
                  planer.lastPacketTime, now);
  addDeviceStatus(page, "Jointer", jointer.hasReceivedPacket,
                  jointer.lastPacketTime, false, 0, now);
  addDeviceStatus(page, "Work Table", workTable.hasReceivedPacket,
                  workTable.lastPacketTime, false, 0, now);
  addDeviceStatus(page, "Barrel Monitor", barrelOne.hasReceivedPacket,
                  barrelOne.lastPacketTime, barrelTwo.hasReceivedPacket,
                  barrelTwo.lastPacketTime, now);
  page += "</div></section>";
  unsigned long barrelOneAge = barrelOne.hasReceivedPacket
      ? now - barrelOne.lastPacketTime
      : 0;
    unsigned long barrelTwoAge = barrelTwo.hasReceivedPacket
      ? now - barrelTwo.lastPacketTime
      : 0;
    const char *barrelOneStatus = !barrelOne.hasReceivedPacket
      ? "never"
      : barrelOneAge > COMM_TIMEOUT_MS ? "stale" : "fresh";
    const char *barrelTwoStatus = !barrelTwo.hasReceivedPacket
      ? "never"
      : barrelTwoAge > COMM_TIMEOUT_MS ? "stale" : "fresh";
    Serial.printf("Web page requested: Barrel 1 %s, %u%%, age %lu ms; Barrel 2 %s, %u%%, age %lu ms\n",
          barrelOneStatus, barrelOne.fillPercent, barrelOneAge,
          barrelTwoStatus, barrelTwo.fillPercent, barrelTwoAge);
  addBarrelStatus(page, "Barrel 1", barrelOne, now);
  addBarrelStatus(page, "Barrel 2", barrelTwo, now);
  addControl(page, "CNC Router", "router", manualRouterOn, router.gateRequired);
  addControl(page, "Table Saw", "tableSaw", manualTableSawOn, tableSaw.gateRequired);
  addControl(page, "Jointer", "jointer", manualJointerOn, jointer.gateRequired);
  addControl(page, "Planer", "planer", manualPlanerOn, planer.gateRequired);
  addControl(page, "Work Table", "workTable", manualWorkTableOn, workTable.gateRequired);
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handleControl() {
  String device = server.arg("device");
  String state = server.arg("state");
  bool *manualRequest = nullptr;

  if (device == "router") manualRequest = &manualRouterOn;
  else if (device == "tableSaw") manualRequest = &manualTableSawOn;
  else if (device == "jointer") manualRequest = &manualJointerOn;
  else if (device == "planer") manualRequest = &manualPlanerOn;
  else if (device == "workTable") manualRequest = &manualWorkTableOn;

  if (manualRequest == nullptr || (state != "on" && state != "off")) {
    server.send(400, "text/plain", "Invalid control request");
    return;
  }

  *manualRequest = (state == "on");
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void startWebServer() {
  if (webServerStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/control", HTTP_POST, handleControl);
  server.begin();
  webServerStarted = true;
  Serial.print("Web controls available at http://");
  Serial.println(WiFi.localIP());
}


// ============================================================
// UPDATE MACHINE FROM ESP-NOW
// ============================================================

void updateMachineFromPacket(
    MachineState &machine,
    bool request,
    unsigned long now) {

  machine.request = request;
  machine.lastPacketTime = now;
  machine.hasReceivedPacket = true;
}


// ============================================================
// ESP-NOW RECEIVE CALLBACK
// ============================================================

void OnDataRecv(
  const uint8_t *senderAddress,
    const uint8_t *incomingData,
    int len) {

  // Reject packet if wrong size
  if (len != sizeof(struct_message)) {
    Serial.printf("ESP-NOW size mismatch from %02X:%02X:%02X:%02X:%02X:%02X: received %d bytes, expected %u\n",
                  senderAddress[0], senderAddress[1], senderAddress[2],
                  senderAddress[3], senderAddress[4], senderAddress[5],
                  len,
                  static_cast<unsigned>(sizeof(struct_message)));
    return;
  }

  struct_message incomingMessage;

  memcpy(
      &incomingMessage,
      incomingData,
      sizeof(incomingMessage)
  );

  // Force null termination
  incomingMessage.device[
      sizeof(incomingMessage.device) - 1
  ] = '\0';

  Serial.printf("ESP-NOW RX %d bytes from %02X:%02X:%02X:%02X:%02X:%02X: device='%s', request=%s, fill=%u%%\n",
                len,
                senderAddress[0], senderAddress[1], senderAddress[2],
                senderAddress[3], senderAddress[4], senderAddress[5],
                incomingMessage.device,
                incomingMessage.request ? "ON" : "OFF",
                incomingMessage.fillPercent);

  unsigned long now = millis();

  if (strcmp(incomingMessage.device, "Barrel 1 Full") == 0) {
    barrelOne.full = incomingMessage.request;
    barrelOne.fillPercent = incomingMessage.fillPercent;
    barrelOne.lastPacketTime = now;
    barrelOne.hasReceivedPacket = true;
    return;
  }

  if (strcmp(incomingMessage.device, "Barrel 2 Full") == 0) {
    barrelTwo.full = incomingMessage.request;
    barrelTwo.fillPercent = incomingMessage.fillPercent;
    barrelTwo.lastPacketTime = now;
    barrelTwo.hasReceivedPacket = true;
    return;
  }


  // ----------------------------------------------------------
  // Identify machine
  // ----------------------------------------------------------

  if (strcmp(
        incomingMessage.device,
        "CNC Router") == 0) {

    updateMachineFromPacket(
        router,
        incomingMessage.request,
        now
    );
  }


  else if (strcmp(
             incomingMessage.device,
             "Table Saw") == 0) {

    updateMachineFromPacket(
        tableSaw,
        incomingMessage.request,
        now
    );
  }


  else if (strcmp(
             incomingMessage.device,
             "Jointer") == 0) {

    updateMachineFromPacket(
        jointer,
        incomingMessage.request,
        now
    );
  }


  else if (strcmp(
             incomingMessage.device,
             "Planer") == 0) {

    updateMachineFromPacket(
        planer,
        incomingMessage.request,
        now
    );
  }


  else if (strcmp(
             incomingMessage.device,
             "Work Table") == 0) {

    updateMachineFromPacket(
        workTable,
        incomingMessage.request,
        now
    );
  } else {
    Serial.printf("ESP-NOW packet ignored: unrecognized device '%s'\n",
                  incomingMessage.device);
  }
}


// ============================================================
// DETERMINE WHETHER MACHINE NEEDS EXTRACTION
// ============================================================

bool machineNeedsExtraction(
    MachineState &machine,
    unsigned long now) {

  // ----------------------------------------------------------
  // No packet has ever been received
  // ----------------------------------------------------------

  if (!machine.hasReceivedPacket) {
    return false;
  }


  // ----------------------------------------------------------
  // COMMUNICATION CHECK
  // ----------------------------------------------------------

  bool communicationLost =
      (now - machine.lastPacketTime) > COMM_TIMEOUT_MS;


  // ----------------------------------------------------------
  // FAIL-SAFE CONDITION
  // ----------------------------------------------------------
  //
  // If communication disappears while the last known request
  // was ON, KEEP extraction ON.
  //
  // We do NOT want an ESP failure to shut the collector down
  // while the woodworking machine could still be running.
  //

  if (communicationLost && machine.request) {

    return true;
  }


  // ----------------------------------------------------------
  // MACHINE CURRENTLY ON
  // ----------------------------------------------------------

  if (machine.request) {

    // Cancel cleanup timer
    machine.offDelayActive = false;

    machine.lastRequest = true;

    return true;
  }


  // ----------------------------------------------------------
  // Detect ON -> OFF transition
  // ----------------------------------------------------------

  if (machine.lastRequest && !machine.request) {

    machine.offTime = now;

    machine.offDelayActive = true;

    machine.lastRequest = false;
  }


  // ----------------------------------------------------------
  // 15-second cleanup
  // ----------------------------------------------------------

  if (machine.offDelayActive) {

    if ((now - machine.offTime) < OFF_DELAY_MS) {

      return true;
    }

    // Cleanup finished
    machine.offDelayActive = false;
  }


  return false;
}


// ============================================================
// SET GATE STATE
// ============================================================

void updateGate(
    MachineState &machine,
    int outputPin,
    bool shouldOpen,
    unsigned long now) {

  // ----------------------------------------------------------
  // OPEN GATE
  // ----------------------------------------------------------

  if (shouldOpen) {

    // Detect closed -> open transition
    if (!machine.gateRequired) {

      machine.gateOpenTime = now;
    }

    machine.gateRequired = true;

    digitalWrite(
        outputPin,
        OUTPUT_ON
    );

    return;
  }


  // ----------------------------------------------------------
  // CLOSING LOGIC
  // ----------------------------------------------------------
  //
  // Don't close until collector has been OFF for 500 ms.
  //

  if (!dustCollectorOn) {

    if ((now - dustCollectorOffTime)
          >= GATE_CLOSE_DELAY_MS) {

      machine.gateRequired = false;

      digitalWrite(
          outputPin,
          OUTPUT_OFF
      );
    }
  }
}


// ============================================================
// CHECK WHETHER A GATE HAS BEEN OPEN LONG ENOUGH
// ============================================================

bool gateReadyForCollector(
    MachineState &machine,
    bool needsExtraction,
    unsigned long now) {

  if (!needsExtraction) {
    return false;
  }

  return (
    (now - machine.gateOpenTime)
      >= GATE_OPEN_DELAY_MS
  );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  // Uncomment for diagnostics
  Serial.begin(115200);


  // ----------------------------------------------------------
  // OUTPUT SETUP
  // ----------------------------------------------------------

  pinMode(
      ROUTER_SOL_PIN,
      OUTPUT
  );

  pinMode(
      TABLE_SAW_SOL_PIN,
      OUTPUT
  );

  pinMode(
      JOINTER_SOL_PIN,
      OUTPUT
  );

  pinMode(
      PLANER_SOL_PIN,
      OUTPUT
  );

    pinMode(
      WORK_TABLE_SOL_PIN,
      OUTPUT
    );

  pinMode(
      DUST_COLLECTOR_PIN,
      OUTPUT
  );


  // ----------------------------------------------------------
  // SAFE STARTUP STATE
  // ----------------------------------------------------------

  digitalWrite(
      ROUTER_SOL_PIN,
      OUTPUT_OFF
  );

  digitalWrite(
      TABLE_SAW_SOL_PIN,
      OUTPUT_OFF
  );

  digitalWrite(
      JOINTER_SOL_PIN,
      OUTPUT_OFF
  );

  digitalWrite(
      PLANER_SOL_PIN,
      OUTPUT_OFF
  );

    digitalWrite(
      WORK_TABLE_SOL_PIN,
      OUTPUT_OFF
    );

  digitalWrite(
      DUST_COLLECTOR_PIN,
      OUTPUT_OFF
  );


  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, MASTER_WIFI_CHANNEL);

  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - wifiStartTime) < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi AP BSSID: %s; channel: %u\n",
                  WiFi.BSSIDstr().c_str(), WiFi.channel());
    startWebServer();
  } else {
    Serial.println("Wi-Fi unavailable; automatic control will continue and web access will retry.");
  }


  // ----------------------------------------------------------
  // ESP-NOW
  // ----------------------------------------------------------

  if (esp_now_init() != ESP_OK) {

    Serial.println(
        "ERROR: ESP-NOW initialization failed"
    );

    return;
  }


  // Register receive callback

  esp_now_register_recv_cb(
      OnDataRecv
  );
  String masterMac = WiFi.macAddress();
  Serial.printf("ESP-NOW ready; master MAC %s, channel %u, packet size %u bytes\n",
                masterMac.c_str(),
                WiFi.channel(),
                static_cast<unsigned>(sizeof(struct_message)));


  Serial.println();
  Serial.println(
      "=================================="
  );

  Serial.println(
      "Dust Collection Master Ready"
  );

  Serial.println(
      "=================================="
  );
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

  unsigned long now = millis();


  // ==========================================================
  // DETERMINE MACHINE EXTRACTION REQUIREMENTS
  // ==========================================================

  bool routerNeedsExtraction =
      machineNeedsExtraction(
          router,
          now
      );

  bool tableSawNeedsExtraction =
      machineNeedsExtraction(
          tableSaw,
          now
      );

  bool jointerNeedsExtraction =
      machineNeedsExtraction(
          jointer,
          now
      );

  bool planerNeedsExtraction =
      machineNeedsExtraction(
          planer,
          now
      );

  routerNeedsExtraction = routerNeedsExtraction || manualRouterOn;
  tableSawNeedsExtraction = tableSawNeedsExtraction || manualTableSawOn;
  jointerNeedsExtraction = jointerNeedsExtraction || manualJointerOn;
  planerNeedsExtraction = planerNeedsExtraction || manualPlanerOn;
  bool workTableNeedsExtraction =
      machineNeedsExtraction(workTable, now) || manualWorkTableOn;


  // ==========================================================
  // OPEN REQUIRED GATES
  // ==========================================================

  updateGate(
      router,
      ROUTER_SOL_PIN,
      routerNeedsExtraction,
      now
  );

  updateGate(
      tableSaw,
      TABLE_SAW_SOL_PIN,
      tableSawNeedsExtraction,
      now
  );

  updateGate(
      jointer,
      JOINTER_SOL_PIN,
      jointerNeedsExtraction,
      now
  );

  updateGate(
      planer,
      PLANER_SOL_PIN,
      planerNeedsExtraction,
      now
  );

    updateGate(
      workTable,
      WORK_TABLE_SOL_PIN,
      workTableNeedsExtraction,
      now
    );


  // ==========================================================
  // DETERMINE IF COLLECTOR IS REQUIRED
  // ==========================================================

  bool collectorRequired =
      routerNeedsExtraction ||
      tableSawNeedsExtraction ||
      jointerNeedsExtraction ||
      planerNeedsExtraction ||
      workTableNeedsExtraction;


  // ==========================================================
  // COLLECTOR START LOGIC
  // ==========================================================
  //
  // Before starting the collector, at least one requesting
  // gate must have been open for 500 ms.
  //

  bool collectorStartAllowed =

      gateReadyForCollector(
          router,
          routerNeedsExtraction,
          now
      )

      ||

      gateReadyForCollector(
          tableSaw,
          tableSawNeedsExtraction,
          now
      )

      ||

      gateReadyForCollector(
          jointer,
          jointerNeedsExtraction,
          now
      )

      ||

      gateReadyForCollector(
          planer,
          planerNeedsExtraction,
          now
        )

        ||

        gateReadyForCollector(
          workTable,
          workTableNeedsExtraction,
          now
      );


  // ==========================================================
  // CONTROL DUST COLLECTOR
  // ==========================================================

  if (collectorRequired) {

    // If collector is already running,
    // leave it running.
    //
    // Otherwise wait for gate-open delay.

    if (
        dustCollectorOn ||
      collectorStartAllowed
    ) {

      digitalWrite(
          DUST_COLLECTOR_PIN,
          OUTPUT_ON
      );

      dustCollectorOn = true;
    }
  }

  else {

    // Nothing requires extraction anymore

    if (dustCollectorOn) {

      digitalWrite(
          DUST_COLLECTOR_PIN,
          OUTPUT_OFF
      );

      dustCollectorOn = false;

      dustCollectorOffTime = now;
    }
  }


  // ==========================================================
  // UPDATE GATES AGAIN
  // ==========================================================
  //
  // This second pass allows gates waiting for the collector
  // shutdown delay to close when appropriate.
  //

  updateGate(
      router,
      ROUTER_SOL_PIN,
      routerNeedsExtraction,
      now
  );

  updateGate(
      tableSaw,
      TABLE_SAW_SOL_PIN,
      tableSawNeedsExtraction,
      now
  );

  updateGate(
      jointer,
      JOINTER_SOL_PIN,
      jointerNeedsExtraction,
      now
  );

  updateGate(
      planer,
      PLANER_SOL_PIN,
      planerNeedsExtraction,
      now
  );

    updateGate(
      workTable,
      WORK_TABLE_SOL_PIN,
      workTableNeedsExtraction,
      now
    );

  if (WiFi.status() == WL_CONNECTED) {
    startWebServer();
    server.handleClient();
  } else if ((now - lastWiFiAttempt) >= WIFI_RETRY_INTERVAL_MS) {
    lastWiFiAttempt = now;
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, MASTER_WIFI_CHANNEL);
  }
}