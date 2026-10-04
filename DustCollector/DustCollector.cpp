#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <stdlib.h>
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

struct TimerSettings {
  uint32_t offDelayMs;
  uint32_t gateOpenDelayMs;
  uint32_t gateCloseDelayMs;
  uint32_t communicationTimeoutMs;
  uint32_t wifiRetryIntervalMs;
  uint32_t wifiConnectTimeoutMs;
};

constexpr TimerSettings DEFAULT_TIMER_SETTINGS = {
  15000, 500, 5000, 5000, 10000, 15000
};
constexpr uint32_t MAX_CONFIGURABLE_TIMER_MS = 3600000;
constexpr uint32_t MIN_COMMUNICATION_TIMEOUT_MS = 1000;
constexpr uint32_t MIN_WIFI_INTERVAL_MS = 1000;
constexpr uint32_t MAX_WIFI_CONNECT_TIMEOUT_MS = 120000;
TimerSettings timerSettings = DEFAULT_TIMER_SETTINGS;

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

  uint64_t runtimeMs;
  unsigned long runtimeLastUpdateTime;
};


// ============================================================
// CREATE MACHINES
// ============================================================

MachineState router = {
  false, false, 0,
  false,
  false, 0,
  false, 0,
  0, 0
};

MachineState tableSaw = {
  false, false, 0,
  false,
  false, 0,
  false, 0,
  0, 0
};

MachineState jointer = {
  false, false, 0,
  false,
  false, 0,
  false, 0,
  0, 0
};

MachineState planer = {
  false, false, 0,
  false,
  false, 0,
  false, 0,
  0, 0
};

MachineState workTable = {
  false, false, 0,
  false,
  false, 0,
  false, 0,
  0, 0
};

struct BarrelStatus {
  volatile bool full;
  volatile uint8_t fillPercent;
  volatile bool hasReceivedPacket;
  volatile bool hasKnownReading;
  volatile unsigned long lastPacketTime;
};

BarrelStatus barrelOne = {false, 0, false, false, 0};
BarrelStatus barrelTwo = {false, 0, false, false, 0};


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


// ============================================================
// WEB SERVER
// ============================================================

bool isValidTimerSettings(const TimerSettings &settings) {
  return settings.offDelayMs <= MAX_CONFIGURABLE_TIMER_MS &&
         settings.gateOpenDelayMs <= MAX_CONFIGURABLE_TIMER_MS &&
         settings.gateCloseDelayMs <= MAX_CONFIGURABLE_TIMER_MS &&
         settings.communicationTimeoutMs >= MIN_COMMUNICATION_TIMEOUT_MS &&
         settings.communicationTimeoutMs <= MAX_CONFIGURABLE_TIMER_MS &&
         settings.wifiRetryIntervalMs >= MIN_WIFI_INTERVAL_MS &&
         settings.wifiRetryIntervalMs <= MAX_CONFIGURABLE_TIMER_MS &&
         settings.wifiConnectTimeoutMs >= MIN_WIFI_INTERVAL_MS &&
         settings.wifiConnectTimeoutMs <= MAX_WIFI_CONNECT_TIMEOUT_MS;
}

void loadTimerSettings() {
  Preferences preferences;
  if (!preferences.begin("dustcfg", true)) {
    Serial.println("Unable to read timer settings; using defaults.");
    return;
  }

  TimerSettings storedSettings = {};
  size_t storedLength = preferences.getBytes(
      "timers", &storedSettings, sizeof(storedSettings));
  preferences.end();

  if (storedLength == 0) {
    Serial.println("No saved timer settings; using defaults.");
    return;
  }

  if (storedLength != sizeof(storedSettings) ||
      !isValidTimerSettings(storedSettings)) {
    Serial.println("Saved timer settings are invalid; using defaults.");
    return;
  }

  timerSettings = storedSettings;
  Serial.println("Loaded saved timer settings.");
}

bool parseTimerValue(const char *name, uint32_t minimum, uint32_t maximum,
                     uint32_t &value) {
  String rawValue = server.arg(name);
  if (rawValue.length() == 0) {
    return false;
  }

  for (size_t index = 0; index < rawValue.length(); ++index) {
    char character = rawValue.charAt(index);
    if (character < '0' || character > '9') {
      return false;
    }
  }

  char *end = nullptr;
  unsigned long parsedValue = strtoul(rawValue.c_str(), &end, 10);
  if (end == rawValue.c_str() || *end != '\0' ||
      parsedValue < minimum || parsedValue > maximum) {
    return false;
  }

  value = static_cast<uint32_t>(parsedValue);
  return true;
}

void addTimerField(String &page, const char *label, const char *name,
                   uint32_t value, uint32_t minimum, uint32_t maximum) {
  page += "<label class='field' for='";
  page += name;
  page += "'><span>";
  page += label;
  page += "</span><span class='input-row'><input id='";
  page += name;
  page += "' name='";
  page += name;
  page += "' type='number' inputmode='numeric' min='";
  page += String(minimum);
  page += "' max='";
  page += String(maximum);
  page += "' step='1' value='";
  page += String(value);
  page += "' required><span>ms</span></span></label>";
}

void handleTimerConfig() {
  String page;
  page.reserve(3600);
  page = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Timer Settings - Dust Collector</title>";
  page += "<style>:root{color-scheme:light;--ink:#172923;--muted:#65746d;--line:#d8e1dc;--paper:#f1f5f2;--white:#fff;--green:#176b4a;--orange:#e97835}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.45 'Trebuchet MS',sans-serif}.shell{max-width:720px;margin:0 auto;padding:28px 20px 48px}.topbar,.panel{padding:22px 24px;background:var(--white);border:1px solid var(--line);border-radius:8px}.topbar{margin-bottom:18px;background:#18372d;color:white;border-bottom:4px solid var(--orange)}h1{margin:0;font-size:clamp(24px,5vw,32px)}.topbar p{margin:5px 0 0;color:#d5e4dc}.panel h2{margin:0 0 6px;font-size:20px}.help{margin:0 0 18px;color:var(--muted)}.field{display:flex;justify-content:space-between;align-items:center;gap:16px;padding:12px 0;border-top:1px solid #edf1ee}.input-row{display:flex;align-items:center;gap:8px;color:var(--muted)}input{width:145px;min-height:42px;padding:8px 10px;border:1px solid #aab8b0;border-radius:4px;color:var(--ink);font:inherit}input:focus-visible,a:focus-visible,button:focus-visible{outline:3px solid var(--orange);outline-offset:2px}.actions{display:flex;align-items:center;gap:16px;margin-top:20px}button{min-height:44px;padding:10px 18px;border:0;border-radius:4px;background:var(--green);color:white;font:700 15px 'Trebuchet MS',sans-serif;cursor:pointer}a{color:var(--green);font-weight:700}.saved{padding:10px 12px;background:#e4f2e9;color:var(--green);border-radius:4px}@media(max-width:520px){.shell{padding:14px 12px 30px}.topbar,.panel{padding:18px}.field{align-items:flex-start;flex-direction:column;gap:8px}.input-row,input{width:100%}}</style>";
  page += "</head><body><main class='shell'><header class='topbar'><h1>Timer Settings</h1><p>Adjust how long the dust collection system waits.</p></header><section class='panel'><h2>Controller timers</h2><p class='help'>Values are in milliseconds and are saved on this controller.</p>";
  if (server.arg("saved") == "1") {
    page += "<p class='saved' role='status'>Timer settings saved.</p>";
  }
  page += "<form method='post' action='/config/save'>";
  addTimerField(page, "Machine off-delay", "offDelayMs",
                timerSettings.offDelayMs, 0, MAX_CONFIGURABLE_TIMER_MS);
  addTimerField(page, "Gate opening delay", "gateOpenDelayMs",
                timerSettings.gateOpenDelayMs, 0, MAX_CONFIGURABLE_TIMER_MS);
  addTimerField(page, "Last gate close delay", "gateCloseDelayMs",
                timerSettings.gateCloseDelayMs, 0, MAX_CONFIGURABLE_TIMER_MS);
  addTimerField(page, "Communication timeout", "communicationTimeoutMs",
                timerSettings.communicationTimeoutMs,
                MIN_COMMUNICATION_TIMEOUT_MS, MAX_CONFIGURABLE_TIMER_MS);
  addTimerField(page, "Wi-Fi retry interval", "wifiRetryIntervalMs",
                timerSettings.wifiRetryIntervalMs,
                MIN_WIFI_INTERVAL_MS, MAX_CONFIGURABLE_TIMER_MS);
  addTimerField(page, "Startup Wi-Fi connection timeout",
                "wifiConnectTimeoutMs", timerSettings.wifiConnectTimeoutMs,
                MIN_WIFI_INTERVAL_MS, MAX_WIFI_CONNECT_TIMEOUT_MS);
  page += "<div class='actions'><button type='submit'>Save settings</button><a href='/'>Back to controls</a></div></form></section></main></body></html>";
  server.send(200, "text/html", page);
}

void handleTimerConfigSave() {
  TimerSettings updatedSettings = {};
  if (!parseTimerValue("offDelayMs", 0, MAX_CONFIGURABLE_TIMER_MS,
                       updatedSettings.offDelayMs) ||
      !parseTimerValue("gateOpenDelayMs", 0, MAX_CONFIGURABLE_TIMER_MS,
                       updatedSettings.gateOpenDelayMs) ||
      !parseTimerValue("gateCloseDelayMs", 0, MAX_CONFIGURABLE_TIMER_MS,
                       updatedSettings.gateCloseDelayMs) ||
      !parseTimerValue("communicationTimeoutMs",
                       MIN_COMMUNICATION_TIMEOUT_MS,
                       MAX_CONFIGURABLE_TIMER_MS,
                       updatedSettings.communicationTimeoutMs) ||
      !parseTimerValue("wifiRetryIntervalMs", MIN_WIFI_INTERVAL_MS,
                       MAX_CONFIGURABLE_TIMER_MS,
                       updatedSettings.wifiRetryIntervalMs) ||
      !parseTimerValue("wifiConnectTimeoutMs", MIN_WIFI_INTERVAL_MS,
                       MAX_WIFI_CONNECT_TIMEOUT_MS,
                       updatedSettings.wifiConnectTimeoutMs)) {
    server.send(400, "text/plain",
                "Invalid timer values. Settings were not changed.");
    return;
  }

  Preferences preferences;
  if (!preferences.begin("dustcfg", false)) {
    server.send(500, "text/plain",
                "Unable to open timer storage. Settings were not changed.");
    return;
  }

  size_t savedLength = preferences.putBytes(
      "timers", &updatedSettings, sizeof(updatedSettings));
  preferences.end();
  if (savedLength != sizeof(updatedSettings)) {
    server.send(500, "text/plain",
                "Unable to save timer settings. Settings were not changed.");
    return;
  }

  timerSettings = updatedSettings;
  server.sendHeader("Location", "/config?saved=1", true);
  server.send(303, "text/plain", "");
}

uint64_t machineRuntimeAt(const MachineState &machine, unsigned long now) {
  uint64_t runtimeMs = machine.runtimeMs;
  if (machine.request && machine.runtimeLastUpdateTime != 0) {
    runtimeMs += now - machine.runtimeLastUpdateTime;
  }
  return runtimeMs;
}

void updateMachineRuntime(MachineState &machine, unsigned long now) {
  if (machine.runtimeLastUpdateTime != 0 && machine.request) {
    machine.runtimeMs += now - machine.runtimeLastUpdateTime;
  }
  machine.runtimeLastUpdateTime = now;
}

String formatRuntime(uint64_t runtimeMs) {
  uint64_t totalSeconds = runtimeMs / 1000;
  uint64_t hours = totalSeconds / 3600;
  uint64_t minutes = (totalSeconds / 60) % 60;
  uint64_t seconds = totalSeconds % 60;

  if (hours > 0) {
    return String(static_cast<unsigned long>(hours)) + "h " +
           String(static_cast<unsigned long>(minutes)) + "m";
  }
  if (totalSeconds >= 60) {
    return String(static_cast<unsigned long>(totalSeconds / 60)) + "m " +
           String(static_cast<unsigned long>(seconds)) + "s";
  }
  return String(static_cast<unsigned long>(seconds)) + "s";
}

void addControl(String &page, const char *name, const char *device,
                bool manualOn, bool outputOn, const MachineState &machine,
                unsigned long now) {
  page += "<section class='control-card'><div class='control-heading'><h2>";
  page += name;
  page += "</h2><span class='runtime' id='runtime-";
  page += device;
  page += "'>";
  page += formatRuntime(machineRuntimeAt(machine, now));
  page += "</span></div><div class='control-state'><span>Gate output</span><strong class='state-pill' id='output-";
  page += device;
  page += "'>";
  page += outputOn ? "ON" : "OFF";
  page += "</strong></div><div class='control-state'><span>Manual request</span><strong class='state-pill' id='manual-";
  page += device;
  page += "'>";
  page += manualOn ? "ON" : "OFF";
  page += "</strong></div><form class='control-actions' method='post' action='/control'>";
  page += "<input type='hidden' name='device' value='";
  page += device;
  page += "'><button class='toggle-button";
  page += manualOn ? " is-on" : "";
  page += "' id='toggle-";
  page += device;
  page += "' name='state' value='";
  page += manualOn ? "off'>Turn OFF" : "on'>Turn ON";
  page += "</button></form></section>";
}

void addBarrelStatus(String &page, const char *name, const char *statusId,
                     BarrelStatus &barrel, unsigned long now) {
  bool readingFresh = barrel.hasReceivedPacket &&
      (now - barrel.lastPacketTime) <=
          timerSettings.communicationTimeoutMs;
  page += "<section class='barrel'><h2>";
  page += name;
  page += "</h2><p>Status: <strong id='barrel-";
  page += statusId;
  page += "-status' class='";

  if (!barrel.hasKnownReading) {
    page += "unknown'>NO DATA";
  } else {
    page += barrel.full ? "full'>" : readingFresh ? "clear'>" : "stale'>";
    page += String(barrel.fillPercent);
    page += "% full";
    if (barrel.full) {
      page += " - FULL";
    }
    if (!readingFresh) {
      page += " - STALE";
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

void appendDeviceStatusJson(String &json,
                            bool firstHasPacket, unsigned long firstPacketTime,
                            bool secondHasPacket, unsigned long secondPacketTime,
                            unsigned long now) {
  bool hasPacket = firstHasPacket || secondHasPacket;
  unsigned long packetAge = firstHasPacket ? now - firstPacketTime : 0;
  if (secondHasPacket &&
      (!firstHasPacket || now - secondPacketTime < packetAge)) {
    packetAge = now - secondPacketTime;
  }

  const char *className = "waiting";
  String status = "WAITING";
  if (hasPacket && packetAge <= timerSettings.communicationTimeoutMs) {
    className = "online";
    status = "ONLINE";
  } else if (hasPacket) {
    className = "offline";
    status = "OFFLINE - last heard ";
    appendHeartbeatAge(status, packetAge);
    status += " ago";
  }

  json += "{\"text\":\"";
  json += status;
  json += "\",\"class\":\"";
  json += className;
  json += "\"}";
}

void appendBarrelStatusJson(String &json, BarrelStatus &barrel,
                            unsigned long now) {
  const char *className = "unknown";
  String status = "NO DATA";
  bool readingFresh = barrel.hasReceivedPacket &&
      (now - barrel.lastPacketTime) <=
          timerSettings.communicationTimeoutMs;
  if (barrel.hasKnownReading) {
    className = barrel.full ? "full" : readingFresh ? "clear" : "stale";
    status = String(barrel.fillPercent) + "% full";
    if (barrel.full) {
      status += " - FULL";
    }
    if (!readingFresh) {
      status += " - STALE";
    }
  }

  json += "{\"text\":\"";
  json += status;
  json += "\",\"class\":\"";
  json += className;
  json += "\"}";
}

void appendControlStatusJson(String &json, bool manualOn, bool outputOn,
                             const MachineState &machine, unsigned long now) {
  json += "{\"output\":\"";
  json += outputOn ? "ON" : "OFF";
  json += "\",\"manual\":\"";
  json += manualOn ? "ON" : "OFF";
  json += "\",\"runtime\":\"";
  json += formatRuntime(machineRuntimeAt(machine, now));
  json += "\"}";
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
  } else if (packetAge <= timerSettings.communicationTimeoutMs) {
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
  page += "<title>Dust Collector</title>";
  page += "<style>:root{color-scheme:light;--ink:#172923;--muted:#65746d;--line:#d8e1dc;--paper:#f1f5f2;--white:#fff;--green:#176b4a;--red:#b42318;--orange:#e97835}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.45 'Trebuchet MS',sans-serif}.shell{max-width:1080px;margin:0 auto;padding:28px 24px 48px}.topbar{display:flex;align-items:center;justify-content:space-between;gap:20px;padding:24px 28px;margin-bottom:22px;background:#18372d;color:white;border-radius:8px;border-bottom:4px solid var(--orange)}.eyebrow,.section-label{margin:0 0 5px;color:#a8c5b7;font-size:12px;font-weight:700;letter-spacing:1.2px;text-transform:uppercase}h1{margin:0;font-size:clamp(25px,4vw,34px);line-height:1.1}.collector-status{display:flex;align-items:center;gap:12px;font-size:14px;color:#d5e4dc}.collector-status strong,.state-pill{display:inline-flex;align-items:center;justify-content:center;min-width:52px;padding:4px 10px;border-radius:99px;background:#d9eee2;color:#155b3e;font-size:12px;font-weight:800;letter-spacing:.4px}.panel{margin:18px 0;padding:20px 22px;background:var(--white);border:1px solid var(--line);border-radius:8px}.panel h2,.barrel h2,.control-card h2{margin:0;font-size:18px}.panel-title{display:flex;align-items:baseline;justify-content:space-between;gap:12px;margin-bottom:12px}.panel-title .section-label{margin:0;color:var(--muted)}.device-list{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));column-gap:36px}.device-status{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:11px 0;border-bottom:1px solid #edf1ee}.device-status strong{min-width:0;text-align:right;font-size:12px;line-height:1.35}.online,.barrel .clear{color:var(--green)}.offline,.barrel .full{color:var(--red)}.waiting,.barrel .unknown{color:var(--muted)}.barrel .stale{color:var(--orange)}.barrel-grid,.control-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}.barrel,.control-card{min-width:0;padding:18px;background:var(--white);border:1px solid var(--line);border-radius:8px}.barrel h2{margin-bottom:10px}.barrel p{margin:0;color:var(--muted)}.barrel strong{display:block;margin-top:4px;font-size:21px}.barrel .clear,.barrel .full,.barrel .unknown,.barrel .stale{min-width:3em}.control-card{border-top:3px solid #a9c6b7}.control-heading{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;margin-bottom:16px}.runtime{color:var(--muted);font-size:13px;font-variant-numeric:tabular-nums;white-space:nowrap}.control-state{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:8px 0;border-top:1px solid #edf1ee;color:var(--muted);font-size:14px}.control-actions{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:14px}button{min-height:44px;padding:10px 16px;border:0;border-radius:4px;color:white;font:700 14px 'Trebuchet MS',sans-serif;cursor:pointer;transition:filter .15s,transform .15s}button:hover{filter:brightness(1.08)}button:active{transform:translateY(1px)}button:focus-visible{outline:3px solid var(--orange);outline-offset:2px}.open-button{background:var(--green)}.close-button{background:#59645e}strong{font-variant-numeric:tabular-nums}@media(max-width:620px){.shell{padding:14px 14px 32px}.topbar{align-items:flex-start;flex-direction:column;padding:20px}.device-list,.barrel-grid,.control-grid{grid-template-columns:1fr}.panel{padding:17px}.collector-status{width:100%;justify-content:space-between}}</style>";
  page += "<style>.barrel-grid{margin-bottom:20px}.control-actions{grid-template-columns:1fr}.toggle-button{width:100%;background:var(--green)}.toggle-button.is-on{background:#59645e}.settings-link{padding:9px 12px;border:1px solid #9bb5a7;border-radius:4px;color:white;font-weight:700;text-decoration:none;white-space:nowrap}</style>";
  page += "</head><body><main class='shell'><header class='topbar'><div><p class='eyebrow'>Shop air system</p><h1>Dust Collection</h1></div><div class='collector-status'><span>Collector</span><strong id='collector-status'>";
  page += dustCollectorOn ? "ON" : "OFF";
  page += "</strong></div><a class='settings-link' href='/config'>Timer settings</a></header>";
  unsigned long now = millis();
  page += "<section class='panel'><div class='panel-title'><h2>Connected equipment</h2><p class='section-label'>ESP-NOW links</p></div><div class='device-list'>";
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
  page += "</div></section><div class='barrel-grid'>";
  unsigned long barrelOneAge = barrelOne.hasReceivedPacket
      ? now - barrelOne.lastPacketTime
      : 0;
    unsigned long barrelTwoAge = barrelTwo.hasReceivedPacket
      ? now - barrelTwo.lastPacketTime
      : 0;
    const char *barrelOneStatus = !barrelOne.hasReceivedPacket
      ? "never"
      : barrelOneAge > timerSettings.communicationTimeoutMs
          ? "stale" : "fresh";
    const char *barrelTwoStatus = !barrelTwo.hasReceivedPacket
      ? "never"
      : barrelTwoAge > timerSettings.communicationTimeoutMs
          ? "stale" : "fresh";
    Serial.printf("Web page requested: Barrel 1 %s, %u%%, age %lu ms; Barrel 2 %s, %u%%, age %lu ms\n",
          barrelOneStatus, barrelOne.fillPercent, barrelOneAge,
          barrelTwoStatus, barrelTwo.fillPercent, barrelTwoAge);
  addBarrelStatus(page, "Barrel 1", "one", barrelOne, now);
  addBarrelStatus(page, "Barrel 2", "two", barrelTwo, now);
  page += "</div><div class='control-grid'>";
  addControl(page, "CNC Router", "router", manualRouterOn,
             router.gateRequired, router, now);
  addControl(page, "Table Saw", "tableSaw", manualTableSawOn,
             tableSaw.gateRequired, tableSaw, now);
  addControl(page, "Jointer", "jointer", manualJointerOn,
             jointer.gateRequired, jointer, now);
  addControl(page, "Planer", "planer", manualPlanerOn,
             planer.gateRequired, planer, now);
  addControl(page, "Work Table", "workTable", manualWorkTableOn,
             workTable.gateRequired, workTable, now);
  page += "</div></main><script>async function refreshStatus(){try{const response=await fetch('/status',{cache:'no-store'});if(!response.ok)return;const data=await response.json();document.getElementById('collector-status').textContent=data.collector;document.querySelectorAll('.device-status strong').forEach((node,index)=>{node.textContent=data.devices[index].text;node.className=data.devices[index].class;});document.querySelectorAll('.barrel strong').forEach((node,index)=>{node.textContent=data.barrels[index].text;node.className=data.barrels[index].class;});const devices=['router','tableSaw','jointer','planer','workTable'];data.controls.forEach((control,index)=>{document.getElementById('output-'+devices[index]).textContent=control.output;document.getElementById('manual-'+devices[index]).textContent=control.manual;const toggle=document.getElementById('toggle-'+devices[index]);const manualIsOn=control.manual==='ON';toggle.value=manualIsOn?'off':'on';toggle.textContent=manualIsOn?'Turn OFF':'Turn ON';toggle.classList.toggle('is-on',manualIsOn);document.getElementById('runtime-'+devices[index]).textContent=control.runtime;});}catch(error){}finally{setTimeout(refreshStatus,500);}}refreshStatus();</script></body></html>";
  server.send(200, "text/html", page);
}

void handleStatus() {
  unsigned long now = millis();
  String json;
  json.reserve(768);
  json += "{\"collector\":\"";
  json += dustCollectorOn ? "ON" : "OFF";
  json += "\",\"devices\":[";
  appendDeviceStatusJson(json, router.hasReceivedPacket, router.lastPacketTime,
                         false, 0, now);
  json += ",";
  appendDeviceStatusJson(json, tableSaw.hasReceivedPacket,
                         tableSaw.lastPacketTime, planer.hasReceivedPacket,
                         planer.lastPacketTime, now);
  json += ",";
  appendDeviceStatusJson(json, jointer.hasReceivedPacket,
                         jointer.lastPacketTime, false, 0, now);
  json += ",";
  appendDeviceStatusJson(json, workTable.hasReceivedPacket,
                         workTable.lastPacketTime, false, 0, now);
  json += ",";
  appendDeviceStatusJson(json, barrelOne.hasReceivedPacket,
                         barrelOne.lastPacketTime, barrelTwo.hasReceivedPacket,
                         barrelTwo.lastPacketTime, now);
  json += "],\"barrels\":[";
  appendBarrelStatusJson(json, barrelOne, now);
  json += ",";
  appendBarrelStatusJson(json, barrelTwo, now);
  json += "],\"controls\":[";
  appendControlStatusJson(json, manualRouterOn, router.gateRequired, router, now);
  json += ",";
  appendControlStatusJson(json, manualTableSawOn, tableSaw.gateRequired,
                          tableSaw, now);
  json += ",";
  appendControlStatusJson(json, manualJointerOn, jointer.gateRequired,
                          jointer, now);
  json += ",";
  appendControlStatusJson(json, manualPlanerOn, planer.gateRequired,
                          planer, now);
  json += ",";
  appendControlStatusJson(json, manualWorkTableOn, workTable.gateRequired,
                          workTable, now);
  json += "]}";
  server.send(200, "application/json", json);
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
  server.on("/config", HTTP_GET, handleTimerConfig);
  server.on("/config/save", HTTP_POST, handleTimerConfigSave);
  server.on("/status", HTTP_GET, handleStatus);
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
    barrelOne.hasKnownReading = true;
    return;
  }

  if (strcmp(incomingMessage.device, "Barrel 2 Full") == 0) {
    barrelTwo.full = incomingMessage.request;
    barrelTwo.fillPercent = incomingMessage.fillPercent;
    barrelTwo.lastPacketTime = now;
    barrelTwo.hasReceivedPacket = true;
    barrelTwo.hasKnownReading = true;
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
      (now - machine.lastPacketTime) >
          timerSettings.communicationTimeoutMs;


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

    if ((now - machine.offTime) < timerSettings.offDelayMs) {

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

bool anyOtherGateOpen(const MachineState &machine) {
  return
      (&machine != &router && router.gateRequired) ||
      (&machine != &tableSaw && tableSaw.gateRequired) ||
      (&machine != &jointer && jointer.gateRequired) ||
      (&machine != &planer && planer.gateRequired) ||
      (&machine != &workTable && workTable.gateRequired);
}

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
  if (
      machine.gateRequired &&
      (anyOtherGateOpen(machine) ||
       (!dustCollectorOn &&
        (now - dustCollectorOffTime) >=
            timerSettings.gateCloseDelayMs))
  ) {
    machine.gateRequired = false;

    digitalWrite(
        outputPin,
        OUTPUT_OFF
    );
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
      >= timerSettings.gateOpenDelayMs
  );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  // Uncomment for diagnostics
  Serial.begin(115200);
  loadTimerSettings();


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
         (millis() - wifiStartTime) < timerSettings.wifiConnectTimeoutMs) {
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

  updateMachineRuntime(router, now);
  updateMachineRuntime(tableSaw, now);
  updateMachineRuntime(jointer, now);
  updateMachineRuntime(planer, now);
  updateMachineRuntime(workTable, now);


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
  } else if ((now - lastWiFiAttempt) >=
             timerSettings.wifiRetryIntervalMs) {
    lastWiFiAttempt = now;
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, MASTER_WIFI_CHANNEL);
  }
}