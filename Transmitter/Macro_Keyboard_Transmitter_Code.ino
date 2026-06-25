#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <USBHIDConsumerControl.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>
#include <ctype.h>
USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
USBHIDConsumerControl Consumer;
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences prefs;
static const uint8_t BUTTON_PINS[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint16_t DEBOUNCE_MS = 30;
static const uint8_t DNS_PORT = 53;
static const uint8_t MACRO_LEN = 100;
static const uint8_t SSID_LEN = 33;
static const uint8_t PASS_LEN = 65;
static const uint8_t MAC_STR_LEN = 18;
static const uint8_t MAX_LAYERS = 10;
bool buttonState[16];
bool lastReading[16];
unsigned long lastDebounceTime[16];
unsigned long lastFire[16];
char macros[MAX_LAYERS][16][MACRO_LEN];
char homeSsid[SSID_LEN];
char homePass[PASS_LEN];
char apSsid[SSID_LEN] = "MacroPad_Setup";
char apPass[PASS_LEN] = "macro1234";
char layout[5] = "US";
char targetMacStr[MAC_STR_LEN] = "FF:FF:FF:FF:FF:FF";
char receiverIp[16] = "";
uint32_t sleepTimeout = 5;
uint32_t repeatDelay = 0;
bool disableSleepWired = false;
uint8_t totalLayers = 3;
uint8_t activeLayer = 0;
uint8_t targetMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool peerRegistered = false;
unsigned long lastActivity = 0;
bool lowPower = false;
bool staConnected = false;
bool apActive = false;
unsigned long lastStaCheck = 0;
typedef struct struct_message {
  char layout[5];
  char macro[100];
} struct_message;
struct_message outgoing;
static const int LOG_LINES = 40;
static const int LOG_WIDTH = 96;
char logBuf[LOG_LINES][LOG_WIDTH];
int logHead = 0;
int logCount = 0;
char configBuf[22000];
char statusBuf[700];
void logLine(const char *msg) {
  snprintf(logBuf[logHead], LOG_WIDTH, "[%lus] %s", millis() / 1000, msg);
  Serial.println(logBuf[logHead]);
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}
void clearLogs() {
  logHead = 0;
  logCount = 0;
  for (int i = 0; i < LOG_LINES; i++) logBuf[i][0] = '\0';
}
void buildLogText(char *out, size_t maxLen) {
  size_t pos = 0;
  int start = (logHead - logCount + LOG_LINES) % LOG_LINES;
  for (int i = 0; i < logCount && pos < maxLen - 2; i++) {
    const char *line = logBuf[(start + i) % LOG_LINES];
    size_t len = strlen(line);
    if (pos + len + 2 >= maxLen) break;
    memcpy(out + pos, line, len);
    pos += len;
    out[pos++] = '\n';
  }
  out[pos] = '\0';
}
bool parseMac(const char *str, uint8_t *out) {
  int v[6];
  if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}
void loadSettings() {
  prefs.begin("macropad", true);
  char key[8];
  for (int l = 0; l < MAX_LAYERS; l++) {
    for (int k = 0; k < 16; k++) {
      snprintf(key, sizeof(key), "l%dm%d", l, k);
      if (prefs.getString(key, macros[l][k], MACRO_LEN) == 0) macros[l][k][0] = '\0';
    }
  }
  if (prefs.getString("ssid", homeSsid, SSID_LEN) == 0) homeSsid[0] = '\0';
  if (prefs.getString("pass", homePass, PASS_LEN) == 0) homePass[0] = '\0';
  if (prefs.getString("layout", layout, sizeof(layout)) == 0) strcpy(layout, "US");
  if (prefs.getString("mac", targetMacStr, MAC_STR_LEN) == 0) strcpy(targetMacStr, "FF:FF:FF:FF:FF:FF");
  sleepTimeout = prefs.getUInt("sleep", 5);
  repeatDelay = prefs.getUInt("repeat", 0);
  disableSleepWired = prefs.getBool("nosleepw", false);
  totalLayers = prefs.getUChar("tlayers", 3);
  activeLayer = prefs.getUChar("active", 0);
  prefs.end();
  if (totalLayers < 1) totalLayers = 1;
  if (totalLayers > MAX_LAYERS) totalLayers = MAX_LAYERS;
  if (activeLayer >= totalLayers) activeLayer = 0;
  parseMac(targetMacStr, targetMac);
}
void saveSettings() {
  prefs.begin("macropad", false);
  char key[8];
  for (int l = 0; l < MAX_LAYERS; l++) {
    for (int k = 0; k < 16; k++) {
      snprintf(key, sizeof(key), "l%dm%d", l, k);
      prefs.putString(key, macros[l][k]);
    }
  }
  prefs.putString("ssid", homeSsid);
  prefs.putString("pass", homePass);
  prefs.putString("layout", layout);
  prefs.putString("mac", targetMacStr);
  prefs.putUInt("sleep", sleepTimeout);
  prefs.putUInt("repeat", repeatDelay);
  prefs.putBool("nosleepw", disableSleepWired);
  prefs.putUChar("tlayers", totalLayers);
  prefs.putUChar("active", activeLayer);
  prefs.end();
}
void setActiveLayer(int idx) {
  if (idx < 0) idx = 0;
  if (idx >= (int)totalLayers) idx = totalLayers - 1;
  if ((uint8_t)idx != activeLayer) {
    activeLayer = (uint8_t)idx;
    prefs.begin("macropad", false);
    prefs.putUChar("active", activeLayer);
    prefs.end();
    char m[32];
    snprintf(m, sizeof(m), "Active layer -> %d", activeLayer + 1);
    logLine(m);
  }
}
void registerPeer() {
  if (peerRegistered) {
    esp_now_del_peer(targetMac);
    peerRegistered = false;
  }
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, targetMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    peerRegistered = true;
    char m[48];
    snprintf(m, sizeof(m), "ESP-NOW peer set: %s", targetMacStr);
    logLine(m);
  } else {
    logLine("ESP-NOW peer registration FAILED");
  }
}
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  logLine(status == ESP_NOW_SEND_SUCCESS ? "ESP-NOW delivery OK" : "ESP-NOW delivery FAIL");
}
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len >= 4 && memcmp(data, "<IP>", 4) == 0) {
    int n = len - 4;
    if (n > (int)sizeof(receiverIp) - 1) n = sizeof(receiverIp) - 1;
    memcpy(receiverIp, data + 4, n);
    receiverIp[n] = '\0';
    char m[48];
    snprintf(m, sizeof(m), "Receiver IP learned: %s", receiverIp);
    logLine(m);
  }
}
void broadcastMacro(const char *macro) {
  memset(&outgoing, 0, sizeof(outgoing));
  strncpy(outgoing.layout, layout, sizeof(outgoing.layout) - 1);
  strncpy(outgoing.macro, macro, sizeof(outgoing.macro) - 1);
  if (esp_now_send(targetMac, (uint8_t *)&outgoing, sizeof(outgoing)) != ESP_OK) logLine("ESP-NOW send error");
}
void broadcastWifiCreds() {
  char buf[120];
  int n = snprintf(buf, sizeof(buf), "<WIFI>%s|%s", homeSsid, homePass);
  if (n < 0) return;
  if (n > (int)sizeof(buf)) n = sizeof(buf);
  esp_now_send(targetMac, (uint8_t *)buf, n);
  logLine("Broadcast Wi-Fi credentials to receiver");
}
char translateChar(char c) {
  if (strcmp(layout, "FR") == 0) {
    switch (c) {
      case 'a': return 'q';
      case 'q': return 'a';
      case 'z': return 'w';
      case 'w': return 'z';
      case 'A': return 'Q';
      case 'Q': return 'A';
      case 'Z': return 'W';
      case 'W': return 'Z';
      case 'm': return ',';
      default: break;
    }
  } else if (strcmp(layout, "DE") == 0) {
    switch (c) {
      case 'y': return 'z';
      case 'z': return 'y';
      case 'Y': return 'Z';
      case 'Z': return 'Y';
      default: break;
    }
  } else if (strcmp(layout, "JP") == 0) {
    if (c == '@') return '[';
  } else if (strcmp(layout, "TR") == 0) {
    if (c == 'i') return 'i';
  }
  return c;
}
uint8_t namedKeyCode(const char *t) {
  if (!strcasecmp(t, "CTRL") || !strcasecmp(t, "CONTROL")) return KEY_LEFT_CTRL;
  if (!strcasecmp(t, "SHIFT")) return KEY_LEFT_SHIFT;
  if (!strcasecmp(t, "ALT")) return KEY_LEFT_ALT;
  if (!strcasecmp(t, "WIN") || !strcasecmp(t, "GUI") || !strcasecmp(t, "CMD") || !strcasecmp(t, "META")) return KEY_LEFT_GUI;
  if (!strcasecmp(t, "ENTER") || !strcasecmp(t, "RETURN")) return KEY_RETURN;
  if (!strcasecmp(t, "TAB")) return KEY_TAB;
  if (!strcasecmp(t, "ESC") || !strcasecmp(t, "ESCAPE")) return KEY_ESC;
  if (!strcasecmp(t, "BACKSPACE") || !strcasecmp(t, "BKSP")) return KEY_BACKSPACE;
  if (!strcasecmp(t, "DELETE") || !strcasecmp(t, "DEL")) return KEY_DELETE;
  if (!strcasecmp(t, "SPACE")) return ' ';
  if (!strcasecmp(t, "UP")) return KEY_UP_ARROW;
  if (!strcasecmp(t, "DOWN")) return KEY_DOWN_ARROW;
  if (!strcasecmp(t, "LEFT")) return KEY_LEFT_ARROW;
  if (!strcasecmp(t, "RIGHT")) return KEY_RIGHT_ARROW;
  if (!strcasecmp(t, "HOME")) return KEY_HOME;
  if (!strcasecmp(t, "END")) return KEY_END;
  if (!strcasecmp(t, "PAGEUP")) return KEY_PAGE_UP;
  if (!strcasecmp(t, "PAGEDOWN")) return KEY_PAGE_DOWN;
  if (!strcasecmp(t, "CAPSLOCK")) return KEY_CAPS_LOCK;
  if ((t[0] == 'F' || t[0] == 'f') && isdigit((unsigned char)t[1])) {
    int num = atoi(t + 1);
    if (num >= 1 && num <= 12) return KEY_F1 + (num - 1);
  }
  return 0;
}
bool consumerAction(const char *t) {
  if (!strcasecmp(t, "VOL_UP")) { Consumer.press(CONSUMER_CONTROL_VOLUME_INCREMENT); delay(20); Consumer.release(); return true; }
  if (!strcasecmp(t, "VOL_DOWN")) { Consumer.press(CONSUMER_CONTROL_VOLUME_DECREMENT); delay(20); Consumer.release(); return true; }
  if (!strcasecmp(t, "MUTE")) { Consumer.press(CONSUMER_CONTROL_MUTE); delay(20); Consumer.release(); return true; }
  if (!strcasecmp(t, "PLAY_PAUSE") || !strcasecmp(t, "PLAYPAUSE")) { Consumer.press(CONSUMER_CONTROL_PLAY_PAUSE); delay(20); Consumer.release(); return true; }
  if (!strcasecmp(t, "NEXT")) { Consumer.press(CONSUMER_CONTROL_SCAN_NEXT); delay(20); Consumer.release(); return true; }
  if (!strcasecmp(t, "PREV") || !strcasecmp(t, "PREVIOUS")) { Consumer.press(CONSUMER_CONTROL_SCAN_PREVIOUS); delay(20); Consumer.release(); return true; }
  if (!strcasecmp(t, "STOP")) { Consumer.press(CONSUMER_CONTROL_STOP); delay(20); Consumer.release(); return true; }
  return false;
}
bool mouseAction(const char *t) {
  if (!strcasecmp(t, "MOUSE_LEFT")) { Mouse.click(MOUSE_LEFT); return true; }
  if (!strcasecmp(t, "MOUSE_RIGHT")) { Mouse.click(MOUSE_RIGHT); return true; }
  if (!strcasecmp(t, "MOUSE_MIDDLE")) { Mouse.click(MOUSE_MIDDLE); return true; }
  if (!strcasecmp(t, "MOUSE_DOUBLE")) { Mouse.click(MOUSE_LEFT); delay(40); Mouse.click(MOUSE_LEFT); return true; }
  return false;
}
void pressCombo(char *token) {
  char *save;
  char *part = strtok_r(token, "+", &save);
  while (part != NULL) {
    while (*part == ' ') part++;
    uint8_t code = namedKeyCode(part);
    if (code != 0) {
      Keyboard.press(code);
    } else if (strlen(part) == 1) {
      Keyboard.press(translateChar(part[0]));
    }
    delay(8);
    part = strtok_r(NULL, "+", &save);
  }
  delay(15);
  Keyboard.releaseAll();
}
void sendToken(char *t) {
  while (*t == ' ') t++;
  size_t len = strlen(t);
  if (len == 0) return;
  if (!strncasecmp(t, "DELAY:", 6)) {
    long ms = atol(t + 6);
    if (ms > 0) delay(ms);
    return;
  }
  if (!strncasecmp(t, "LAYER:", 6)) {
    int l = atoi(t + 6);
    if (l >= 1 && l <= (int)totalLayers) setActiveLayer(l - 1);
    return;
  }
  if (mouseAction(t)) return;
  if (consumerAction(t)) return;
  if (strchr(t, '+') != NULL) { pressCombo(t); return; }
  uint8_t code = namedKeyCode(t);
  if (code != 0 && len > 1) {
    Keyboard.press(code);
    delay(12);
    Keyboard.release(code);
    return;
  }
  if (len == 1) {
    Keyboard.write(translateChar(t[0]));
    return;
  }
  for (size_t i = 0; i < len; i++) {
    Keyboard.write(translateChar(t[i]));
    delay(5);
  }
}
void executeMacro(int index) {
  const char *macro = macros[activeLayer][index];
  char m[80];
  snprintf(m, sizeof(m), "L%d Btn %d -> %s", activeLayer + 1, index + 1, macro[0] ? macro : "(empty)");
  logLine(m);
  if (macro[0] == '\0') return;
  broadcastMacro(macro);
  char work[MACRO_LEN];
  strncpy(work, macro, MACRO_LEN - 1);
  work[MACRO_LEN - 1] = '\0';
  char *save;
  char *token = strtok_r(work, ",", &save);
  while (token != NULL) {
    sendToken(token);
    token = strtok_r(NULL, ",", &save);
  }
}
void wakeUp() {
  if (lowPower) {
    setCpuFrequencyMhz(240);
    lowPower = false;
    logLine("Wake -> CPU 240MHz");
  }
  lastActivity = millis();
}
size_t jsonEscape(const char *in, char *out, size_t maxLen) {
  size_t pos = 0;
  for (size_t i = 0; in[i] && pos < maxLen - 2; i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out[pos++] = '\\'; out[pos++] = c; }
    else if (c == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
    else out[pos++] = c;
  }
  out[pos] = '\0';
  return pos;
}
void buildConfigJson() {
  char esc[MACRO_LEN * 2];
  size_t pos = 0;
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "{\"macros\":[");
  for (int l = 0; l < MAX_LAYERS; l++) {
    pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "[");
    for (int k = 0; k < 16; k++) {
      jsonEscape(macros[l][k], esc, sizeof(esc));
      pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"%s\"%s", esc, k < 15 ? "," : "");
    }
    pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "]%s", l < MAX_LAYERS - 1 ? "," : "");
  }
  jsonEscape(homeSsid, esc, sizeof(esc));
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "],\"ssid\":\"%s\",", esc);
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"layout\":\"%s\",", layout);
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"sleep\":%u,", sleepTimeout);
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"repeat\":%u,", repeatDelay);
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"nosleepw\":%s,", disableSleepWired ? "true" : "false");
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"mac\":\"%s\",", targetMacStr);
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"totalLayers\":%u,", totalLayers);
  pos += snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"maxLayers\":%u,", MAX_LAYERS);
  snprintf(configBuf + pos, sizeof(configBuf) - pos, "\"activeLayer\":%u}", activeLayer);
}
void buildStatusJson() {
  snprintf(statusBuf, sizeof(statusBuf),
    "{\"wired\":%s,\"apActive\":%s,\"staConnected\":%s,\"staIp\":\"%s\",\"apIp\":\"%s\",\"selfMac\":\"%s\",\"receiverIp\":\"%s\",\"activeLayer\":%u,\"totalLayers\":%u}",
    ((bool)USB) ? "true" : "false",
    apActive ? "true" : "false",
    staConnected ? "true" : "false",
    WiFi.localIP().toString().c_str(),
    WiFi.softAPIP().toString().c_str(),
    WiFi.macAddress().c_str(),
    receiverIp[0] ? receiverIp : "",
    activeLayer, totalLayers);
}
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MacroPad Settings</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1c1e21;--muted:#65676b;--accent:#3b6cf6;--border:#d8dadf;--tabbg:#e9eaee;--danger:#e23b3b;}
[data-theme="dark"]{--bg:#15171c;--card:#1e2128;--fg:#e7e9ee;--muted:#9aa0aa;--accent:#5b8cff;--border:#30343d;--tabbg:#262a32;--danger:#ff5d5d;}
*{box-sizing:border-box;}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg);transition:.2s;}
header{display:flex;align-items:center;justify-content:space-between;padding:16px 24px;background:var(--card);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:5;}
h1{font-size:20px;margin:0;}
.theme-switch{background:none;border:none;padding:0;margin:0;cursor:pointer;display:inline-flex;align-items:center;-webkit-tap-highlight-color:transparent;}
.switch-track{position:relative;width:64px;height:32px;border-radius:999px;background:var(--tabbg);border:1px solid var(--border);display:flex;align-items:center;justify-content:space-between;padding:0 8px;box-shadow:inset 0 1px 3px rgba(0,0,0,.18);transition:all 0.3s ease-in-out;}
.switch-icon{position:relative;z-index:2;width:15px;height:15px;display:flex;align-items:center;justify-content:center;transition:all 0.3s ease-in-out;}
.switch-icon svg{width:15px;height:15px;display:block;}
.switch-icon.sun{color:#f5a623;opacity:1;transform:scale(1);}
.switch-icon.moon{color:#8ab4ff;opacity:.45;transform:scale(.85);}
[data-theme="dark"] .switch-icon.sun{opacity:.45;transform:scale(.85);}
[data-theme="dark"] .switch-icon.moon{opacity:1;transform:scale(1);}
.switch-thumb{position:absolute;top:50%;left:3px;width:26px;height:26px;border-radius:50%;background:var(--card);box-shadow:0 2px 6px rgba(0,0,0,.28);transform:translateY(-50%) translateX(0);transition:all 0.3s ease-in-out;z-index:1;}
[data-theme="dark"] .switch-thumb{transform:translateY(-50%) translateX(32px);}
.theme-switch:hover .switch-track{border-color:var(--accent);box-shadow:inset 0 1px 3px rgba(0,0,0,.18),0 0 0 3px color-mix(in srgb,var(--accent) 22%,transparent);}
.theme-switch:active .switch-thumb{width:30px;}
.theme-switch:focus-visible .switch-track{outline:2px solid var(--accent);outline-offset:2px;}
.wrap{max-width:880px;margin:0 auto;padding:20px;}
.tabs{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:18px;}
.tab{padding:10px 16px;border-radius:8px;background:var(--tabbg);cursor:pointer;font-size:14px;}
.tab.active{background:var(--accent);color:#fff;}
.panel{display:none;background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;}
.panel.active{display:block;}
label{display:block;font-size:13px;color:var(--muted);margin:12px 0 4px;}
input,select{width:100%;padding:10px;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--fg);font-size:14px;font-family:inherit;}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px;}
.keyrow{display:flex;gap:6px;}
.keyrow input{flex:2;}
.keyrow select{flex:1;min-width:0;}
.inline{display:flex;gap:8px;align-items:flex-end;}
.inline select{flex:1;}
button.save{margin-top:20px;background:var(--accent);color:#fff;border:none;padding:12px 22px;border-radius:8px;font-size:15px;cursor:pointer;width:100%;}
pre{background:#0b0d11;color:#5bd66f;padding:14px;border-radius:8px;height:300px;overflow:auto;font-size:12px;white-space:pre-wrap;}
.small{font-size:12px;color:var(--muted);margin-top:4px;}
.status{padding:8px 12px;border-radius:8px;background:var(--tabbg);font-size:13px;margin-bottom:10px;}
.card{margin-top:24px;border:1px solid var(--border);border-radius:10px;padding:16px;}
.card h3{margin:0 0 6px;font-size:15px;}
.layerbar{display:flex;gap:10px;align-items:center;background:var(--tabbg);padding:10px 12px;border-radius:8px;margin-bottom:16px;}
.layerbar select{flex:1;}
.danger{margin-top:24px;border:1px solid var(--danger);border-radius:10px;padding:16px;}
.danger h3{margin:0 0 6px;color:var(--danger);font-size:15px;}
button.act{background:var(--accent);color:#fff;border:none;padding:10px 16px;border-radius:8px;cursor:pointer;font-size:14px;white-space:nowrap;}
button.dangerbtn{background:var(--danger);color:#fff;border:none;padding:10px 18px;border-radius:8px;cursor:pointer;font-size:14px;margin-top:10px;}
button.ghost{background:var(--tabbg);color:var(--fg);border:1px solid var(--border);padding:8px 14px;border-radius:8px;cursor:pointer;font-size:13px;margin-top:8px;}
#toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:var(--accent);color:#fff;padding:12px 20px;border-radius:8px;opacity:0;transition:.3s;pointer-events:none;}
#toast.show{opacity:1;}
</style>
</head>
<body>
<header>
<h1>ESP32-S3 MacroPad</h1>
<button class="theme-switch" id="themeBtn" type="button" role="switch" aria-label="Toggle light and dark theme">
<span class="switch-track">
<span class="switch-icon sun"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="4"></circle><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"></path></svg></span>
<span class="switch-icon moon"><svg viewBox="0 0 24 24" fill="currentColor"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"></path></svg></span>
<span class="switch-thumb"></span>
</span>
</button>
</header>
<div class="wrap">
<div class="tabs">
<div class="tab active" data-t="bindings">Key Bindings</div>
<div class="tab" data-t="system">System Settings</div>
<div class="tab" data-t="diag">Diagnostics</div>
<div class="tab" data-t="pair">Device Pairing</div>
</div>
<div class="panel active" id="bindings">
<div class="layerbar">
<label style="margin:0;">Select Layer to Edit</label>
<select id="editLayerSel"></select>
</div>
<p class="small">Syntax: CTRL+C | WIN,DELAY:500,C,A,L,C,ENTER | MOUSE_LEFT | VOL_UP | LAYER:2. Use the preset dropdown to auto-fill.</p>
<div class="grid" id="keyGrid"></div>
</div>
<div class="panel" id="system">
<label>Keyboard Layout</label>
<select id="layout">
<option value="US">US</option><option value="TR">TR</option><option value="FR">FR</option><option value="DE">DE</option><option value="JP">JP</option>
</select>
<label>Total Active Layers (1-10)</label>
<input type="number" id="totalLayers" min="1" max="10">
<div class="card">
<h3>Active Hardware Layer</h3>
<p class="small">Force the keyboard to operate on a specific layer immediately.</p>
<div class="inline">
<select id="activeLayerSel"></select>
<button class="act" id="applyLayerBtn">Set Active</button>
</div>
</div>
<label>Sleep Timeout (minutes)</label>
<input type="number" id="sleep" min="1" max="1440">
<label>Wait Time Between Repeats (ms)</label>
<input type="number" id="repeat" min="0" max="60000">
<p class="small">0 disables key repeat. While a button is held, the macro re-fires every N ms.</p>
<label>Disable Sleep in Wired Mode</label>
<select id="nosleepw">
<option value="0">No (always honor sleep timeout)</option>
<option value="1">Yes (stay at 240MHz when USB connected)</option>
</select>
<label>Home Wi-Fi SSID (STA)</label>
<input type="text" id="ssid">
<label>Home Wi-Fi Password</label>
<input type="password" id="pass" placeholder="(unchanged if blank)">
<p class="small">Saving Wi-Fi credentials also auto-syncs them to the paired Receiver over ESP-NOW.</p>
<div class="card">
<h3>Firmware Update (OTA)</h3>
<p class="small">Select a .bin, then flash this MacroPad or the paired Receiver over Wi-Fi. The Receiver is flashed cross-origin via its learned IP.</p>
<input type="file" id="fwfile" accept=".bin">
<div class="inline" style="margin-top:10px;">
<button class="act" id="otaSelfBtn" style="flex:1;">Flash MacroPad</button>
<button class="act" id="otaRxBtn" style="flex:1;">Flash Receiver</button>
</div>
<div class="small" id="otaStat"></div>
</div>
<div class="danger">
<h3>Danger Zone</h3>
<p class="small">Factory Reset erases ALL stored macros, credentials, and settings from non-volatile memory, then reboots the device.</p>
<button class="dangerbtn" id="resetBtn">Factory Reset</button>
</div>
</div>
<div class="panel" id="diag">
<div class="status" id="netStatus">Loading status...</div>
<label>Live Serial Log</label>
<pre id="logView">Fetching logs...</pre>
<button class="ghost" id="clearLogBtn">Clear Logs</button>
<div class="small">Auto-refresh every 2s.</div>
</div>
<div class="panel" id="pair">
<label>Receiver (Dongle) MAC Address</label>
<input type="text" id="mac" placeholder="AA:BB:CC:DD:EE:FF">
<p class="small">Enter the Receiver's MAC shown on its own web page. Macros and Wi-Fi sync are sent to this address over ESP-NOW. The Receiver auto-reports its IP back here.</p>
<div class="status" id="rxStatus" style="margin-top:12px;">Receiver IP: unknown</div>
</div>
<button class="save" id="saveBtn">Save All Settings</button>
</div>
<div id="toast">Saved</div>
<script>
const PRESETS=["","CTRL+C","CTRL+V","CTRL+X","CTRL+Z","CTRL+A","CTRL+S","ALT+TAB","ALT+F4","WIN+D","WIN+L","WIN,DELAY:500,C,A,L,C,ENTER","ENTER","TAB","ESC","MOUSE_LEFT","MOUSE_RIGHT","MOUSE_MIDDLE","MOUSE_DOUBLE","VOL_UP","VOL_DOWN","MUTE","PLAY_PAUSE","NEXT","PREV","LAYER:1","LAYER:2","LAYER:3","LAYER:4"];
let MAXL=10;
let layers=[];
let totalLayers=3;
let editLayer=0;
let rxIp='';
const grid=document.getElementById('keyGrid');
const inputs=[];
for(let i=0;i<16;i++){
  const d=document.createElement('div');
  const l=document.createElement('label');l.textContent='Key '+(i+1);
  const row=document.createElement('div');row.className='keyrow';
  const inp=document.createElement('input');inp.type='text';inp.id='m'+i;
  const sel=document.createElement('select');
  PRESETS.forEach(p=>{const o=document.createElement('option');o.value=p;o.textContent=p===''?'-- preset --':p;sel.appendChild(o);});
  sel.onchange=()=>{if(sel.value!==''){inp.value=sel.value;}sel.value='';};
  row.appendChild(inp);row.appendChild(sel);
  d.appendChild(l);d.appendChild(row);grid.appendChild(d);
  inputs.push(inp);
}
document.querySelectorAll('.tab').forEach(t=>t.onclick=()=>{
  document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));
  document.querySelectorAll('.panel').forEach(x=>x.classList.remove('active'));
  t.classList.add('active');document.getElementById(t.dataset.t).classList.add('active');
});
const themeBtn=document.getElementById('themeBtn');
function applyTheme(m){document.documentElement.setAttribute('data-theme',m);themeBtn.setAttribute('aria-checked',m==='dark'?'true':'false');localStorage.setItem('mp_theme',m);}
applyTheme(localStorage.getItem('mp_theme')||'light');
themeBtn.onclick=()=>applyTheme(document.documentElement.getAttribute('data-theme')==='dark'?'light':'dark');
function toast(t){const e=document.getElementById('toast');e.textContent=t;e.classList.add('show');setTimeout(()=>e.classList.remove('show'),1800);}
function clampLayers(v){v=parseInt(v)||1;if(v<1)v=1;if(v>MAXL)v=MAXL;return v;}
function storeCurrent(){for(let i=0;i<16;i++)layers[editLayer][i]=inputs[i].value;}
function loadLayerToInputs(l){for(let i=0;i<16;i++)inputs[i].value=(layers[l]&&layers[l][i])?layers[l][i]:'';}
function fillSelect(sel,count,oneBased){sel.innerHTML='';for(let i=0;i<count;i++){const o=document.createElement('option');o.value=oneBased?(i+1):i;o.textContent='Layer '+(i+1);sel.appendChild(o);}}
function rebuildLayerUI(){
  fillSelect(document.getElementById('editLayerSel'),totalLayers,false);
  fillSelect(document.getElementById('activeLayerSel'),totalLayers,true);
  document.getElementById('totalLayers').value=totalLayers;
  if(editLayer>=totalLayers)editLayer=0;
  document.getElementById('editLayerSel').value=editLayer;
}
document.getElementById('editLayerSel').onchange=function(){storeCurrent();editLayer=parseInt(this.value);loadLayerToInputs(editLayer);};
document.getElementById('totalLayers').onchange=function(){storeCurrent();totalLayers=clampLayers(this.value);rebuildLayerUI();loadLayerToInputs(editLayer);};
function loadConfig(){
  fetch('/api/config').then(r=>r.json()).then(c=>{
    MAXL=c.maxLayers;
    layers=c.macros;
    totalLayers=clampLayers(c.totalLayers);
    editLayer=0;
    document.getElementById('layout').value=c.layout;
    document.getElementById('sleep').value=c.sleep;
    document.getElementById('repeat').value=c.repeat;
    document.getElementById('nosleepw').value=c.nosleepw?'1':'0';
    document.getElementById('ssid').value=c.ssid;
    document.getElementById('mac').value=c.mac;
    rebuildLayerUI();
    loadLayerToInputs(0);
    document.getElementById('activeLayerSel').value=c.activeLayer+1;
  });
}
function refreshStatus(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    rxIp=s.receiverIp||'';
    document.getElementById('netStatus').textContent='Active Layer: '+(s.activeLayer+1)+'/'+s.totalLayers+' | USB: '+(s.wired?'Wired':'Unpowered')+' | Mode: '+(s.apActive?'AP+STA':'STA-only')+' | STA: '+(s.staConnected?('Connected '+s.staIp):'Disconnected')+' | RX IP: '+(rxIp||'unknown')+' | MAC: '+s.selfMac;
    document.getElementById('rxStatus').textContent='Receiver IP: '+(rxIp||'unknown (waiting for handshake)');
  });
}
function loadLogs(){fetch('/api/logs').then(r=>r.text()).then(t=>{const v=document.getElementById('logView');v.textContent=t;v.scrollTop=v.scrollHeight;});}
document.getElementById('saveBtn').onclick=()=>{
  storeCurrent();
  const p=new URLSearchParams();
  for(let l=0;l<MAXL;l++)for(let k=0;k<16;k++)p.append('l'+l+'m'+k,(layers[l]&&layers[l][k])?layers[l][k]:'');
  p.append('totalLayers',totalLayers);
  p.append('active',document.getElementById('activeLayerSel').value);
  p.append('layout',document.getElementById('layout').value);
  p.append('sleep',document.getElementById('sleep').value);
  p.append('repeat',document.getElementById('repeat').value);
  p.append('nosleepw',document.getElementById('nosleepw').value);
  p.append('ssid',document.getElementById('ssid').value);
  p.append('pass',document.getElementById('pass').value);
  p.append('mac',document.getElementById('mac').value);
  fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()})
    .then(r=>r.json()).then(j=>{toast(j.status||'Saved');});
};
document.getElementById('applyLayerBtn').onclick=()=>{
  const l=document.getElementById('activeLayerSel').value;
  fetch('/api/setlayer',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'layer='+l})
    .then(r=>r.json()).then(j=>{toast(j.status||'Layer set');refreshStatus();});
};
document.getElementById('clearLogBtn').onclick=()=>{
  fetch('/api/clearlogs',{method:'POST'}).then(r=>r.json()).then(j=>{toast(j.status||'Cleared');loadLogs();});
};
document.getElementById('resetBtn').onclick=()=>{
  if(!confirm('Factory Reset will erase ALL settings and reboot. Continue?'))return;
  fetch('/api/reset',{method:'POST'}).then(r=>r.json()).then(j=>{toast(j.status||'Resetting');}).catch(()=>toast('Rebooting'));
};
function doOTA(url,label){
  const f=document.getElementById('fwfile').files[0];
  const stat=document.getElementById('otaStat');
  if(!f){toast('Select a .bin file');return;}
  if(url.indexOf('http')===0&&!rxIp){toast('Receiver IP unknown');return;}
  const fd=new FormData();fd.append('update',f,f.name);
  const xhr=new XMLHttpRequest();
  xhr.open('POST',url);
  xhr.upload.onprogress=e=>{if(e.lengthComputable)stat.textContent=label+': '+Math.round(e.loaded/e.total*100)+'%';};
  xhr.onload=()=>{try{const j=JSON.parse(xhr.responseText);stat.textContent=label+': '+j.status;toast(j.status);}catch(e){stat.textContent=label+': done, rebooting';toast('Rebooting');}};
  xhr.onerror=()=>{stat.textContent=label+': connection lost (device may be rebooting)';};
  stat.textContent=label+': starting...';
  xhr.send(fd);
}
document.getElementById('otaSelfBtn').onclick=()=>doOTA('/api/update','MacroPad');
document.getElementById('otaRxBtn').onclick=()=>doOTA('http://'+rxIp+'/api/update','Receiver');
loadConfig();refreshStatus();loadLogs();
setInterval(()=>{refreshStatus();loadLogs();},2000);
</script>
</body>
</html>
)rawliteral";
class CaptiveHandler : public AsyncWebHandler {
public:
  bool canHandle(AsyncWebServerRequest *request) { return true; }
  void handleRequest(AsyncWebServerRequest *request) {
    char loc[40];
    snprintf(loc, sizeof(loc), "http://%s/", WiFi.softAPIP().toString().c_str());
    AsyncWebServerResponse *res = request->beginResponse(302, "text/plain", "Redirect");
    res->addHeader("Location", loc);
    request->send(res);
  }
};
void startMdns() {
  MDNS.end();
  if (MDNS.begin("macropadsettings")) {
    MDNS.addService("http", "tcp", 80);
    logLine("mDNS: macropadsettings.local");
  } else {
    logLine("mDNS start failed");
  }
}
void enableApMode() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, apPass);
  delay(100);
  dnsServer.stop();
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  apActive = true;
  startMdns();
  char m[64];
  snprintf(m, sizeof(m), "AP enabled: %s @ %s", apSsid, WiFi.softAPIP().toString().c_str());
  logLine(m);
}
void disableApMode() {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  startMdns();
  logLine("STA locked, AP + captive portal disabled");
}
void handleSave(AsyncWebServerRequest *request) {
  wakeUp();
  char key[8];
  if (request->hasParam("totalLayers", true)) {
    int tl = request->getParam("totalLayers", true)->value().toInt();
    if (tl < 1) tl = 1;
    if (tl > MAX_LAYERS) tl = MAX_LAYERS;
    totalLayers = (uint8_t)tl;
  }
  for (int l = 0; l < MAX_LAYERS; l++) {
    for (int k = 0; k < 16; k++) {
      snprintf(key, sizeof(key), "l%dm%d", l, k);
      if (request->hasParam(key, true)) {
        const char *val = request->getParam(key, true)->value().c_str();
        strncpy(macros[l][k], val, MACRO_LEN - 1);
        macros[l][k][MACRO_LEN - 1] = '\0';
      }
    }
  }
  if (request->hasParam("active", true)) {
    int a = request->getParam("active", true)->value().toInt() - 1;
    if (a < 0) a = 0;
    if (a >= (int)totalLayers) a = totalLayers - 1;
    activeLayer = (uint8_t)a;
  }
  if (activeLayer >= totalLayers) activeLayer = totalLayers - 1;
  if (request->hasParam("layout", true)) {
    strncpy(layout, request->getParam("layout", true)->value().c_str(), sizeof(layout) - 1);
    layout[sizeof(layout) - 1] = '\0';
  }
  if (request->hasParam("sleep", true)) {
    uint32_t s = request->getParam("sleep", true)->value().toInt();
    if (s >= 1) sleepTimeout = s;
  }
  if (request->hasParam("repeat", true)) {
    repeatDelay = request->getParam("repeat", true)->value().toInt();
  }
  if (request->hasParam("nosleepw", true)) {
    disableSleepWired = request->getParam("nosleepw", true)->value().toInt() != 0;
  }
  bool ssidChanged = false;
  if (request->hasParam("ssid", true)) {
    strncpy(homeSsid, request->getParam("ssid", true)->value().c_str(), SSID_LEN - 1);
    homeSsid[SSID_LEN - 1] = '\0';
    ssidChanged = true;
  }
  if (request->hasParam("pass", true)) {
    const char *p = request->getParam("pass", true)->value().c_str();
    if (strlen(p) > 0) {
      strncpy(homePass, p, PASS_LEN - 1);
      homePass[PASS_LEN - 1] = '\0';
      ssidChanged = true;
    }
  }
  if (request->hasParam("mac", true)) {
    const char *m = request->getParam("mac", true)->value().c_str();
    uint8_t tmp[6];
    if (parseMac(m, tmp)) {
      strncpy(targetMacStr, m, MAC_STR_LEN - 1);
      targetMacStr[MAC_STR_LEN - 1] = '\0';
      memcpy(targetMac, tmp, 6);
      registerPeer();
    }
  }
  saveSettings();
  logLine("Settings saved via REST API");
  request->send(200, "application/json", "{\"status\":\"Saved\"}");
  if (ssidChanged) {
    if (!apActive) enableApMode();
    broadcastWifiCreds();
    staConnected = false;
    lastStaCheck = 0;
    if (homeSsid[0] != '\0') WiFi.begin(homeSsid, homePass);
  }
}
void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    wakeUp();
    logLine("OTA upload started");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      logLine("OTA begin failed");
    }
  }
  if (Update.isRunning()) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
      logLine("OTA write error");
    }
  }
  if (final) {
    if (Update.end(true)) {
      char m[48];
      snprintf(m, sizeof(m), "OTA success: %u bytes", (unsigned)(index + len));
      logLine(m);
    } else {
      Update.printError(Serial);
      logLine("OTA end failed");
    }
  }
}
void startNetwork() {
  enableApMode();
  if (homeSsid[0] != '\0') {
    WiFi.begin(homeSsid, homePass);
    char m[64];
    snprintf(m, sizeof(m), "Connecting STA to: %s", homeSsid);
    logLine(m);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      snprintf(m, sizeof(m), "STA connected: %s", WiFi.localIP().toString().c_str());
      logLine(m);
      disableApMode();
    } else {
      logLine("STA failed, staying in AP fallback");
    }
  } else {
    logLine("No STA credentials, AP setup mode active");
  }
}
void initEspNow() {
  uint8_t ch;
  wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  if (esp_now_init() != ESP_OK) {
    logLine("ESP-NOW init FAILED");
    return;
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  registerPeer();
  char m[48];
  snprintf(m, sizeof(m), "ESP-NOW ready on channel %u", ch);
  logLine(m);
}
void setup() {
  Serial.begin(115200);
  delay(300);
  for (int i = 0; i < 16; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    buttonState[i] = HIGH;
    lastReading[i] = HIGH;
    lastDebounceTime[i] = 0;
    lastFire[i] = 0;
  }
  for (int l = 0; l < MAX_LAYERS; l++)
    for (int k = 0; k < 16; k++) macros[l][k][0] = '\0';
  homeSsid[0] = '\0';
  homePass[0] = '\0';
  loadSettings();
  logLine("Settings loaded");
  Keyboard.begin();
  Mouse.begin();
  Consumer.begin();
  USB.begin();
  logLine("USB HID started");
  startNetwork();
  initEspNow();
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    wakeUp();
    request->send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    buildConfigJson();
    request->send(200, "application/json", configBuf);
  });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    buildStatusJson();
    request->send(200, "application/json", statusBuf);
  });
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    static char logText[LOG_LINES * LOG_WIDTH];
    buildLogText(logText, sizeof(logText));
    request->send(200, "text/plain", logText);
  });
  server.on("/api/save", HTTP_POST, handleSave);
  server.on("/api/setlayer", HTTP_POST, [](AsyncWebServerRequest *request) {
    wakeUp();
    if (request->hasParam("layer", true)) {
      int l = request->getParam("layer", true)->value().toInt();
      if (l >= 1 && l <= (int)totalLayers) {
        setActiveLayer(l - 1);
        request->send(200, "application/json", "{\"status\":\"Layer activated\"}");
        return;
      }
    }
    request->send(400, "application/json", "{\"status\":\"Invalid layer\"}");
  });
  server.on("/api/clearlogs", HTTP_POST, [](AsyncWebServerRequest *request) {
    clearLogs();
    logLine("Log buffer cleared via API");
    request->send(200, "application/json", "{\"status\":\"Logs Cleared\"}");
  });
  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    logLine("FACTORY RESET requested");
    request->send(200, "application/json", "{\"status\":\"Resetting\"}");
    prefs.begin("macropad", false);
    prefs.clear();
    prefs.end();
    delay(600);
    ESP.restart();
  });
  server.on("/api/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    bool ok = !Update.hasError();
    AsyncWebServerResponse *res = request->beginResponse(200, "application/json",
      ok ? "{\"status\":\"Flash OK, rebooting\"}" : "{\"status\":\"Flash FAILED\"}");
    res->addHeader("Connection", "close");
    request->send(res);
    if (ok) {
      delay(800);
      ESP.restart();
    }
  }, handleUpload);
  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });
  server.addHandler(new CaptiveHandler()).setFilter(ON_AP_FILTER);
  server.begin();
  logLine("Async web server live on port 80");
  lastActivity = millis();
}
void scanButtons() {
  unsigned long now = millis();
  for (int i = 0; i < 16; i++) {
    bool reading = digitalRead(BUTTON_PINS[i]);
    if (reading != lastReading[i]) {
      lastDebounceTime[i] = now;
      lastReading[i] = reading;
    }
    if ((now - lastDebounceTime[i]) > DEBOUNCE_MS) {
      if (reading != buttonState[i]) {
        buttonState[i] = reading;
        if (buttonState[i] == LOW) {
          wakeUp();
          executeMacro(i);
          lastFire[i] = millis();
        }
      } else if (buttonState[i] == LOW && repeatDelay > 0) {
        if (millis() - lastFire[i] >= repeatDelay) {
          wakeUp();
          executeMacro(i);
          lastFire[i] = millis();
        }
      }
    }
  }
}
void managePower() {
  if (disableSleepWired && (bool)USB) {
    if (lowPower) {
      setCpuFrequencyMhz(240);
      lowPower = false;
    }
    return;
  }
  if (!lowPower && (millis() - lastActivity > (unsigned long)sleepTimeout * 60000UL)) {
    setCpuFrequencyMhz(80);
    lowPower = true;
    logLine("Idle timeout -> CPU 80MHz");
  }
}
void maintainSta() {
  if (millis() - lastStaCheck < 5000) return;
  lastStaCheck = millis();
  if (homeSsid[0] == '\0') {
    staConnected = false;
    if (!apActive) {
      logLine("Credentials blank, re-enabling AP setup mode");
      enableApMode();
    }
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (!staConnected) {
      staConnected = true;
      char m[64];
      snprintf(m, sizeof(m), "STA connected: %s", WiFi.localIP().toString().c_str());
      logLine(m);
    }
    if (apActive) {
      disableApMode();
    }
  } else {
    if (staConnected) logLine("STA connection lost");
    staConnected = false;
    if (!apActive) {
      logLine("Smart fallback: restoring AP + captive portal");
      enableApMode();
    }
    WiFi.begin(homeSsid, homePass);
  }
}
void loop() {
  if (apActive) dnsServer.processNextRequest();
  scanButtons();
  maintainSta();
  managePower();
}
