#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiUdp.h>
#include <EEPROM.h>

#include "index_html.h"

#define FIRMWARE_VERSION "1.0.0"

constexpr uint8_t FAN_PIN = D1;
constexpr bool FAN_ACTIVE_HIGH = true;
constexpr uint16_t DEFAULT_UDP_PORT = 4210;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr size_t EEPROM_SIZE = 512;

constexpr uint32_t CONFIG_MAGIC_V3 = 0x46414E33; // FAN3
constexpr uint32_t CONFIG_MAGIC_V4 = 0x46414E34; // FAN4
constexpr uint8_t BEHAVIOR_STEADY = 0;
constexpr uint8_t BEHAVIOR_ANTISTALL = 1;
constexpr uint32_t DEFAULT_PULSE_INTERVAL_SEC = 45;
constexpr uint16_t DEFAULT_PULSE_OFF_MS = 250;
constexpr uint32_t MIN_PULSE_INTERVAL_SEC = 5;
constexpr uint32_t MAX_PULSE_INTERVAL_SEC = 3600;
constexpr uint16_t MIN_PULSE_OFF_MS = 50;
constexpr uint16_t MAX_PULSE_OFF_MS = 3000;

const char* GITHUB_RELEASE_API = "https://api.github.com/repos/iamvinyl/TubemanController/releases/latest";
const char* GITHUB_ASSET_NAME = "tubeman-controller.bin";

struct DeviceConfigV3 {
  uint32_t magic;
  char ssid[33];
  char password[65];
  uint16_t udpPort;
  uint8_t behaviorMode;
  uint32_t pulseIntervalSec;
  uint16_t pulseOffMs;
  uint8_t reserved[24];
};

struct DeviceConfig {
  uint32_t magic;
  char ssid[33];
  char password[65];
  char hostname[33];
  uint16_t udpPort;
  uint8_t behaviorMode;
  uint32_t pulseIntervalSec;
  uint16_t pulseOffMs;
  uint8_t reserved[12];
};

DeviceConfig config;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiUDP udp;

bool fanOn = false;
bool fanOutputOn = false;
bool udpRunning = false;
bool behaviorPulseActive = false;
uint32_t behaviorPulseStartedAt = 0;
uint32_t lastBehaviorPulseAt = 0;
uint32_t behaviorPulseCount = 0;
uint32_t udpPacketCount = 0;
uint32_t webCommandCount = 0;

constexpr size_t LOG_CAPACITY = 50;
String eventLog[LOG_CAPACITY];
size_t logStart = 0;
size_t logCount = 0;

String setupApSsid;
String setupApPassword;
bool mdnsRunning = false;
bool restartPending = false;
uint32_t restartAt = 0;

String latestVersion;
String latestDownloadUrl;
String firmwareStatus = "unknown";
String firmwareError;
bool firmwareInstallPending = false;
uint32_t firmwareInstallAt = 0;

String chipIdText() {
  String chip = String(ESP.getChipId(), HEX);
  chip.toUpperCase();
  while (chip.length() < 6) chip = "0" + chip;
  return chip;
}

String defaultHostname() {
  String s = "tubeman-" + chipIdText();
  s.toLowerCase();
  return s;
}

String sanitizeHostname(String value) {
  value.trim();
  value.toLowerCase();
  String out;
  out.reserve(32);
  bool lastDash = false;
  for (size_t i = 0; i < value.length() && out.length() < 32; i++) {
    char c = value.charAt(i);
    bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (valid) { out += c; lastDash = false; }
    else if ((c == '-' || c == ' ' || c == '_') && out.length() && !lastDash) { out += '-'; lastDash = true; }
  }
  while (out.endsWith("-")) out.remove(out.length() - 1);
  if (!out.length()) out = defaultHostname();
  return out;
}

String uptimeText() {
  uint32_t totalSeconds = millis() / 1000;
  uint32_t days = totalSeconds / 86400;
  uint32_t hours = (totalSeconds % 86400) / 3600;
  uint32_t minutes = (totalSeconds % 3600) / 60;
  uint32_t seconds = totalSeconds % 60;
  String result;
  if (days > 0) result += String(days) + "d ";
  if (hours < 10) result += "0"; result += String(hours) + ":";
  if (minutes < 10) result += "0"; result += String(minutes) + ":";
  if (seconds < 10) result += "0"; result += String(seconds);
  return result;
}

String jsonEscape(const String& input) {
  String output; output.reserve(input.length() + 12);
  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    switch (c) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: if ((uint8_t)c >= 0x20) output += c;
    }
  }
  return output;
}

void addLog(const String& message) {
  String line = "[" + uptimeText() + "] " + message;
  size_t index;
  if (logCount < LOG_CAPACITY) { index = (logStart + logCount) % LOG_CAPACITY; logCount++; }
  else { index = logStart; logStart = (logStart + 1) % LOG_CAPACITY; }
  eventLog[index] = line;
  Serial.println(line);
}

void setDefaultConfig() {
  memset(&config, 0, sizeof(config));
  config.magic = CONFIG_MAGIC_V4;
  defaultHostname().toCharArray(config.hostname, sizeof(config.hostname));
  config.udpPort = DEFAULT_UDP_PORT;
  config.behaviorMode = BEHAVIOR_STEADY;
  config.pulseIntervalSec = DEFAULT_PULSE_INTERVAL_SEC;
  config.pulseOffMs = DEFAULT_PULSE_OFF_MS;
}

bool commitConfig() {
  config.magic = CONFIG_MAGIC_V4;
  EEPROM.put(0, config);
  return EEPROM.commit();
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  uint32_t storedMagic = 0; EEPROM.get(0, storedMagic);
  if (storedMagic == CONFIG_MAGIC_V4) {
    EEPROM.get(0, config);
  } else if (storedMagic == CONFIG_MAGIC_V3) {
    DeviceConfigV3 oldConfig; EEPROM.get(0, oldConfig);
    setDefaultConfig();
    strncpy(config.ssid, oldConfig.ssid, sizeof(config.ssid) - 1);
    strncpy(config.password, oldConfig.password, sizeof(config.password) - 1);
    config.udpPort = oldConfig.udpPort ? oldConfig.udpPort : DEFAULT_UDP_PORT;
    config.behaviorMode = oldConfig.behaviorMode;
    config.pulseIntervalSec = oldConfig.pulseIntervalSec;
    config.pulseOffMs = oldConfig.pulseOffMs;
    commitConfig();
    addLog("Migrated settings to current format");
  } else {
    setDefaultConfig(); commitConfig();
  }
  config.ssid[32] = 0; config.password[64] = 0; config.hostname[32] = 0;
  String host = sanitizeHostname(config.hostname);
  host.toCharArray(config.hostname, sizeof(config.hostname));
  if (!config.udpPort) config.udpPort = DEFAULT_UDP_PORT;
  if (config.behaviorMode > BEHAVIOR_ANTISTALL) config.behaviorMode = BEHAVIOR_STEADY;
  if (config.pulseIntervalSec < MIN_PULSE_INTERVAL_SEC || config.pulseIntervalSec > MAX_PULSE_INTERVAL_SEC) config.pulseIntervalSec = DEFAULT_PULSE_INTERVAL_SEC;
  if (config.pulseOffMs < MIN_PULSE_OFF_MS || config.pulseOffMs > MAX_PULSE_OFF_MS) config.pulseOffMs = DEFAULT_PULSE_OFF_MS;
}

bool saveNetworkConfig(const String& ssid, const String& password, uint16_t port, const String& hostname) {
  if (!ssid.length() || ssid.length() > 32 || password.length() > 64 || !port) return false;
  String cleanHost = sanitizeHostname(hostname);
  memset(config.ssid, 0, sizeof(config.ssid)); memset(config.password, 0, sizeof(config.password)); memset(config.hostname, 0, sizeof(config.hostname));
  ssid.toCharArray(config.ssid, sizeof(config.ssid)); password.toCharArray(config.password, sizeof(config.password)); cleanHost.toCharArray(config.hostname, sizeof(config.hostname));
  config.udpPort = port;
  return commitConfig();
}

bool saveBehaviorConfig(uint8_t mode, uint32_t intervalSec, uint16_t offMs) {
  if (mode > BEHAVIOR_ANTISTALL || intervalSec < MIN_PULSE_INTERVAL_SEC || intervalSec > MAX_PULSE_INTERVAL_SEC || offMs < MIN_PULSE_OFF_MS || offMs > MAX_PULSE_OFF_MS) return false;
  config.behaviorMode = mode; config.pulseIntervalSec = intervalSec; config.pulseOffMs = offMs;
  return commitConfig();
}

void writeFanOutput(bool enabled) {
  fanOutputOn = enabled;
  digitalWrite(FAN_PIN, FAN_ACTIVE_HIGH ? (enabled ? HIGH : LOW) : (enabled ? LOW : HIGH));
}

void cancelBehaviorPulse() { if (behaviorPulseActive) { behaviorPulseActive = false; if (fanOn) writeFanOutput(true); } }

void setFan(bool enabled, const String& source) {
  bool changed = fanOn != enabled; fanOn = enabled; behaviorPulseActive = false; writeFanOutput(enabled);
  if (enabled) lastBehaviorPulseAt = millis();
  String m = source + " set fan " + (fanOn ? "ON" : "OFF"); if (!changed) m += " (no change)"; addLog(m);
}

void startBehaviorPulse(const String& source) {
  if (!fanOn || behaviorPulseActive) return;
  behaviorPulseActive = true; behaviorPulseStartedAt = millis(); lastBehaviorPulseAt = millis(); behaviorPulseCount++; writeFanOutput(false);
  addLog(source + " anti-stall pulse: output OFF for " + String(config.pulseOffMs) + " ms");
}

void handleBehavior() {
  if (!fanOn) { behaviorPulseActive = false; return; }
  uint32_t now = millis();
  if (behaviorPulseActive) {
    if ((uint32_t)(now - behaviorPulseStartedAt) >= config.pulseOffMs) { behaviorPulseActive = false; writeFanOutput(true); addLog("Anti-stall pulse complete: output restored ON"); }
    return;
  }
  if (config.behaviorMode == BEHAVIOR_ANTISTALL && (uint32_t)(now - lastBehaviorPulseAt) >= config.pulseIntervalSec * 1000UL) startBehaviorPulse("BEHAVIOR");
}

String currentIpAddress() { return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); }
String wifiModeText() { return WiFi.status() == WL_CONNECTED ? "Connected to Wi-Fi" : ((WiFi.getMode() & WIFI_AP) ? "AP mode" : "Offline"); }
String behaviorModeText() { return config.behaviorMode == BEHAVIOR_ANTISTALL ? "Anti-stall" : "Steady"; }

String normalizeCommand(String command) {
  command.trim(); command.toLowerCase();
  if (command == "fan:on" || command == "fan on" || command == "1") return "on";
  if (command == "fan:off" || command == "fan off" || command == "0") return "off";
  if (command == "fan:toggle" || command == "fan toggle") return "toggle";
  if (command == "fan:status" || command == "fan status") return "status";
  if (command == "fan:pulse" || command == "fan pulse" || command == "pulse") return "pulse";
  return command;
}

String executeCommand(const String& raw, const String& source) {
  String c = normalizeCommand(raw);
  if (c == "on") { setFan(true, source); return "fan:on"; }
  if (c == "off") { setFan(false, source); return "fan:off"; }
  if (c == "toggle") { setFan(!fanOn, source); return fanOn ? "fan:on" : "fan:off"; }
  if (c == "pulse") { if (!fanOn) { addLog(source + " requested pulse while fan OFF"); return "error:fan-off"; } startBehaviorPulse(source); return "fan:pulse"; }
  if (c == "status") { addLog(source + " requested status -> " + (fanOn ? "ON" : "OFF")); return fanOn ? "fan:on" : "fan:off"; }
  addLog(source + " sent invalid command: \"" + raw + "\""); return "error:unknown-command";
}

void buildSetupApCredentials() { setupApSsid = "Tubeman-" + chipIdText(); setupApPassword = "tubeman" + chipIdText(); }

void startSetupAp() {
  buildSetupApCredentials(); WiFi.persistent(false); WiFi.mode(WIFI_AP_STA); WiFi.softAP(setupApSsid.c_str(), setupApPassword.c_str());
  addLog("Setup AP started: " + setupApSsid); addLog("Setup page: http://" + WiFi.softAPIP().toString() + "/");
}

void startMdns() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mdnsRunning) MDNS.end();
  mdnsRunning = MDNS.begin(config.hostname);
  if (mdnsRunning) { MDNS.addService("http", "tcp", 80); addLog("mDNS: http://" + String(config.hostname) + ".local/"); }
  else addLog("mDNS failed to start");
}

void startWiFi() {
  if (!strlen(config.ssid)) { addLog("No saved Wi-Fi credentials"); startSetupAp(); return; }
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.hostname(config.hostname); WiFi.begin(config.ssid, config.password); addLog("Connecting to \"" + String(config.ssid) + "\"");
  uint32_t start = millis(); while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) { delay(250); yield(); }
  if (WiFi.status() == WL_CONNECTED) { addLog("Wi-Fi connected. IP: " + WiFi.localIP().toString()); startMdns(); }
  else { addLog("Saved Wi-Fi connection failed"); startSetupAp(); }
}

void startUdp() { if (udpRunning) udp.stop(); udpRunning = udp.begin(config.udpPort); addLog(udpRunning ? "UDP listening on port " + String(config.udpPort) : "UDP listener failed"); }
void sendNoCacheHeaders() { server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0"); server.sendHeader("Pragma", "no-cache"); }
void scheduleRestart(uint32_t delayMs = 1800) { restartPending = true; restartAt = millis() + delayMs; }

int compareVersions(String a, String b) {
  a.replace("v", ""); b.replace("v", "");
  for (int i = 0; i < 4; i++) {
    int ad = a.indexOf('.'), bd = b.indexOf('.');
    String as = ad >= 0 ? a.substring(0, ad) : a; String bs = bd >= 0 ? b.substring(0, bd) : b;
    int av = as.toInt(), bv = bs.toInt(); if (av < bv) return -1; if (av > bv) return 1;
    a = ad >= 0 ? a.substring(ad + 1) : "0"; b = bd >= 0 ? b.substring(bd + 1) : "0";
  }
  return 0;
}

String jsonField(const String& body, const String& key, int startAt = 0) {
  String needle = "\"" + key + "\""; int p = body.indexOf(needle, startAt); if (p < 0) return "";
  p = body.indexOf(':', p + needle.length()); if (p < 0) return "";
  p = body.indexOf('"', p + 1); if (p < 0) return ""; int e = body.indexOf('"', p + 1); if (e < 0) return "";
  return body.substring(p + 1, e);
}

bool checkGitHubUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    firmwareStatus = "unavailable";
    firmwareError = "Wi-Fi not connected";
    return false;
  }

  firmwareStatus = "checking";
  firmwareError = "";
  latestVersion = "";
  latestDownloadUrl = "";

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.useHTTP10(true); // Avoid chunked transfer parsing issues on ESP8266.
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, GITHUB_RELEASE_API)) {
    firmwareStatus = "error";
    firmwareError = "Could not start GitHub request";
    addLog("Firmware check failed: " + firmwareError);
    return false;
  }

  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("Accept-Encoding", "identity"); // Request plain JSON, not gzip/compressed content.
  http.addHeader("User-Agent", "TubemanController/" FIRMWARE_VERSION);
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    firmwareStatus = "error";
    firmwareError = "GitHub returned HTTP " + String(code);
    http.end();
    addLog("Firmware check failed: " + firmwareError);
    return false;
  }

  int expectedLength = http.getSize();
  String body = http.getString();
  http.end();

  addLog("GitHub release response: " + String(body.length()) + " bytes" +
         (expectedLength > 0 ? " / expected " + String(expectedLength) : ""));

  if (!body.length()) {
    firmwareStatus = "error";
    firmwareError = "GitHub returned an empty release response";
    addLog("Firmware check failed: " + firmwareError);
    return false;
  }

  latestVersion = jsonField(body, "tag_name");

  // Fallback: recover the version from the release html_url if tag_name parsing
  // ever fails. GitHub formats it as .../releases/tag/v1.2.3.
  if (!latestVersion.length()) {
    String htmlUrl = jsonField(body, "html_url");
    int tagPos = htmlUrl.indexOf("/releases/tag/");
    if (tagPos >= 0) latestVersion = htmlUrl.substring(tagPos + 14);
  }

  if (!latestVersion.length()) {
    firmwareStatus = "error";
    firmwareError = "Could not parse latest release version";
    String preview = body.substring(0, body.length() > 180 ? 180 : body.length());
    preview.replace("\n", " ");
    preview.replace("\r", " ");
    addLog("Firmware check failed: " + firmwareError + " | JSON: " + preview);
    return false;
  }

  // Public GitHub release assets have a deterministic URL. Building it from the
  // version tag avoids fragile scanning of GitHub's nested assets JSON.
  latestDownloadUrl = "https://github.com/iamvinyl/TubemanController/releases/download/" +
                      latestVersion + "/" + GITHUB_ASSET_NAME;

  firmwareStatus = compareVersions(FIRMWARE_VERSION, latestVersion) < 0 ? "available" : "current";
  addLog("Firmware check: installed " FIRMWARE_VERSION ", latest " + latestVersion +
         (firmwareStatus == "available" ? " (update available)" : " (up to date)"));
  return true;
}

void installGitHubUpdate() {
  firmwareInstallPending = false;
  if (WiFi.status() != WL_CONNECTED || !latestDownloadUrl.length()) { addLog("Firmware install cancelled: update information unavailable"); return; }
  addLog("Starting firmware update to " + latestVersion);
  firmwareStatus = "installing";
  BearSSL::WiFiClientSecure client; client.setInsecure();
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.setClientTimeout(12000);
  ESPhttpUpdate.onStart([](){ addLog("Firmware download started"); });
  ESPhttpUpdate.onProgress([](int cur, int total){ if (total > 0 && (cur == total || cur % 32768 < 1460)) Serial.printf("[OTA] %d/%d\n", cur, total); });
  ESPhttpUpdate.onError([](int err){ addLog("Firmware update failed: " + String(err) + " " + ESPhttpUpdate.getLastErrorString()); firmwareStatus = "error"; firmwareError = ESPhttpUpdate.getLastErrorString(); });
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, latestDownloadUrl);
  if (ret == HTTP_UPDATE_FAILED) addLog("Firmware update failed: " + ESPhttpUpdate.getLastErrorString());
  else if (ret == HTTP_UPDATE_NO_UPDATES) { firmwareStatus = "current"; addLog("Firmware updater reported no update"); }
}

void handleRoot() { sendNoCacheHeaders(); server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }

void handleStatus() {
  String json; json.reserve(1100); json += "{";
  json += "\"fanOn\":" + String(fanOn ? "true" : "false") + ",\"fanOutputOn\":" + String(fanOutputOn ? "true" : "false") + ",\"pulseActive\":" + String(behaviorPulseActive ? "true" : "false") + ",";
  json += "\"ip\":\"" + jsonEscape(currentIpAddress()) + "\",\"wifiMode\":\"" + jsonEscape(wifiModeText()) + "\",\"ssid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : setupApSsid) + "\",\"savedSsid\":\"" + jsonEscape(config.ssid) + "\",";
  json += "\"hostname\":\"" + jsonEscape(config.hostname) + "\",\"localUrl\":\"http://" + jsonEscape(config.hostname) + ".local/\",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"udpPort\":" + String(config.udpPort) + ",\"udpPackets\":" + String(udpPacketCount) + ",\"webCommands\":" + String(webCommandCount) + ",";
  json += "\"behaviorMode\":" + String(config.behaviorMode) + ",\"behaviorModeText\":\"" + behaviorModeText() + "\",\"pulseIntervalSec\":" + String(config.pulseIntervalSec) + ",\"pulseOffMs\":" + String(config.pulseOffMs) + ",\"pulseCount\":" + String(behaviorPulseCount) + ",";
  json += "\"firmwareVersion\":\"" FIRMWARE_VERSION "\",\"latestVersion\":\"" + jsonEscape(latestVersion) + "\",\"firmwareStatus\":\"" + jsonEscape(firmwareStatus) + "\",\"firmwareError\":\"" + jsonEscape(firmwareError) + "\",";
  json += "\"uptime\":\"" + jsonEscape(uptimeText()) + "\"}";
  sendNoCacheHeaders(); server.send(200, "application/json; charset=utf-8", json);
}

void handleLogs() {
  String json = "["; for (size_t i = 0; i < logCount; i++) { if (i) json += ','; size_t idx = (logStart + i) % LOG_CAPACITY; json += '"' + jsonEscape(eventLog[idx]) + '"'; } json += "]";
  sendNoCacheHeaders(); server.send(200, "application/json; charset=utf-8", json);
}

void handleFanCommand() {
  if (!server.hasArg("state")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing-state\"}"); return; }
  webCommandCount++; String result = executeCommand(server.arg("state"), "WEB " + server.client().remoteIP().toString()); bool ok = !result.startsWith("error:");
  server.send(ok ? 200 : 400, "application/json", "{\"ok\":" + String(ok ? "true" : "false") + ",\"result\":\"" + jsonEscape(result) + "\"}");
}

void handleClearLogs() { logStart = 0; logCount = 0; addLog("Event log cleared"); server.send(200, "application/json", "{\"ok\":true}"); }

void handleWifiScan() {
  WiFiMode_t oldMode = WiFi.getMode(); if (oldMode == WIFI_AP) WiFi.mode(WIFI_AP_STA); if (oldMode == WIFI_OFF) WiFi.mode(WIFI_STA);
  int count = WiFi.scanNetworks(false, true); String json = "["; bool first = true;
  for (int rank = 0; rank < count && rank < 40; rank++) {
    int best = -1; for (int i = 0; i < count && i < 40; i++) { bool seen = false; for (int j = 0; j < rank; j++) {} if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i; }
    // simpler duplicate-safe pass below; scan order is already generally useful
    break;
  }
  String seen = "\n";
  for (int i = 0; i < count && i < 40; i++) {
    String ssid = WiFi.SSID(i); if (!ssid.length() || seen.indexOf("\n" + ssid + "\n") >= 0) continue; seen += ssid + "\n";
    if (!first) json += ','; first = false;
    json += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"secure\":" + String(WiFi.encryptionType(i) == ENC_TYPE_NONE ? "false" : "true") + "}";
  }
  json += "]"; WiFi.scanDelete(); sendNoCacheHeaders(); server.send(200, "application/json", json);
}

bool testWifiCredentials(const String& ssid, const String& password, IPAddress& connectedIp) {
  buildSetupApCredentials(); WiFi.persistent(false); WiFi.mode(WIFI_AP_STA); WiFi.softAP(setupApSsid.c_str(), setupApPassword.c_str()); WiFi.disconnect(false); delay(100); WiFi.begin(ssid.c_str(), password.c_str());
  addLog("Testing Wi-Fi \"" + ssid + "\""); uint32_t start = millis(); while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) { delay(100); yield(); }
  if (WiFi.status() == WL_CONNECTED) { connectedIp = WiFi.localIP(); addLog("Wi-Fi test successful: " + connectedIp.toString()); return true; }
  addLog("Wi-Fi test failed; settings not saved"); WiFi.disconnect(false); return false;
}

void handleSaveSettings() {
  if (!server.hasArg("ssid") || !server.hasArg("udpPort") || !server.hasArg("hostname")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing-fields\"}"); return; }
  String ssid = server.arg("ssid"); ssid.trim(); String password = server.hasArg("password") ? server.arg("password") : ""; String hostname = sanitizeHostname(server.arg("hostname")); long port = server.arg("udpPort").toInt();
  if (!ssid.length() || ssid.length() > 32 || password.length() > 64 || port < 1 || port > 65535) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid-settings\"}"); return; }
  if (!password.length() && ssid == String(config.ssid)) password = String(config.password);
  IPAddress newIp; if (!testWifiCredentials(ssid, password, newIp)) { server.send(422, "application/json", "{\"ok\":false,\"error\":\"wifi-connect-failed\"}"); return; }
  if (!saveNetworkConfig(ssid, password, (uint16_t)port, hostname)) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"save-failed\"}"); return; }
  addLog("Network settings saved");
  String json = "{\"ok\":true,\"newIp\":\"" + newIp.toString() + "\",\"localUrl\":\"http://" + hostname + ".local/\"}"; server.send(200, "application/json", json); scheduleRestart();
}

void handleSaveBehavior() {
  long mode = server.arg("mode").toInt(), interval = server.arg("intervalSec").toInt(), offMs = server.arg("offMs").toInt();
  if (!saveBehaviorConfig((uint8_t)mode, (uint32_t)interval, (uint16_t)offMs)) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid-settings\"}"); return; }
  cancelBehaviorPulse(); lastBehaviorPulseAt = millis(); addLog("Behavior settings saved"); server.send(200, "application/json", "{\"ok\":true}");
}

void handleTestPulse() { if (!fanOn) { server.send(409, "application/json", "{\"ok\":false,\"error\":\"fan-must-be-on\"}"); return; } startBehaviorPulse("WEB TEST"); server.send(200, "application/json", "{\"ok\":true}"); }
void handleResetSettings() { setDefaultConfig(); commitConfig(); server.send(200, "application/json", "{\"ok\":true}"); scheduleRestart(); }

void handleUpdateCheck() {
  bool ok = checkGitHubUpdate();
  String json = "{\"ok\":" + String(ok ? "true" : "false") + ",\"current\":\"" FIRMWARE_VERSION "\",\"latest\":\"" + jsonEscape(latestVersion) + "\",\"status\":\"" + jsonEscape(firmwareStatus) + "\",\"error\":\"" + jsonEscape(firmwareError) + "\"}";
  server.send(ok ? 200 : 503, "application/json", json);
}

void handleUpdateInstall() {
  if (firmwareStatus != "available" || !latestDownloadUrl.length()) { if (!checkGitHubUpdate() || firmwareStatus != "available") { server.send(409, "application/json", "{\"ok\":false,\"error\":\"no-update-available\"}"); return; } }
  firmwareInstallPending = true; firmwareInstallAt = millis() + 1500; server.send(202, "application/json", "{\"ok\":true,\"installing\":true}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot); server.on("/api/status", HTTP_GET, handleStatus); server.on("/api/logs", HTTP_GET, handleLogs); server.on("/api/fan", HTTP_ANY, handleFanCommand);
  server.on("/api/logs/clear", HTTP_POST, handleClearLogs); server.on("/api/wifi/scan", HTTP_GET, handleWifiScan); server.on("/api/settings", HTTP_POST, handleSaveSettings);
  server.on("/api/behavior", HTTP_POST, handleSaveBehavior); server.on("/api/behavior/test", HTTP_POST, handleTestPulse); server.on("/api/settings/reset", HTTP_POST, handleResetSettings);
  server.on("/api/update/check", HTTP_GET, handleUpdateCheck); server.on("/api/update/install", HTTP_POST, handleUpdateInstall);
  httpUpdater.setup(&server, "/update");
  server.onNotFound([](){ server.send(404, "application/json", "{\"ok\":false,\"error\":\"not-found\"}"); }); server.begin();
  addLog("Web UI started: http://" + currentIpAddress() + "/");
}

void sendUdpReply(const IPAddress& ip, uint16_t port, const String& response) { udp.beginPacket(ip, port); udp.print(response); udp.endPacket(); }
void handleUdp() {
  if (!udpRunning) return; int packetSize = udp.parsePacket(); if (packetSize <= 0) return;
  char buf[129]; int n = udp.read(buf, sizeof(buf)-1); if (n <= 0) return; buf[n] = 0;
  IPAddress ip = udp.remoteIP(); uint16_t port = udp.remotePort(); String raw(buf); String source = "UDP " + ip.toString() + ":" + String(port);
  udpPacketCount++; addLog(source + " received \"" + raw + "\""); String response = executeCommand(raw, source); sendUdpReply(ip, port, response); addLog(source + " reply \"" + response + "\"");
}

void setup() {
  Serial.begin(115200); delay(100); Serial.println(); pinMode(FAN_PIN, OUTPUT); writeFanOutput(false); fanOn = false; addLog("BOOT firmware " FIRMWARE_VERSION ": fan OFF");
  loadConfig(); startWiFi(); startUdp(); setupWebServer(); lastBehaviorPulseAt = millis();
}

void loop() {
  server.handleClient(); handleUdp(); handleBehavior(); if (mdnsRunning) MDNS.update();
  if (firmwareInstallPending && (int32_t)(millis() - firmwareInstallAt) >= 0) installGitHubUpdate();
  if (restartPending && (int32_t)(millis() - restartAt) >= 0) { delay(50); ESP.restart(); }
  yield();
}
