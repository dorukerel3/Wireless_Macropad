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

static const uint8_t LED_PIN = 1;
static const uint8_t DNS_PORT = 53;
static const uint8_t MACRO_LEN = 100;
static const uint8_t SSID_LEN = 33;
static const uint8_t PASS_LEN = 65;
static const uint8_t MAC_STR_LEN = 18;

char apSsid[SSID_LEN] = "Dongle_Setup";
char apPass[SSID_LEN] = "macro1234";
char staSsid[SSID_LEN];
char staPass[PASS_LEN];
char txMacStr[MAC_STR_LEN] = "00:00:00:00:00:00";
uint8_t txMac[6] = {0, 0, 0, 0, 0, 0};
char rxLayout[5] = "US";

bool txPeerReady = false;
bool staConnected = false;
unsigned long lastStaCheck = 0;
unsigned long lastIpBcast = 0;

typedef struct struct_message {
  char layout[5];
  char macro[100];
} struct_message;

volatile bool macroPending = false;
struct_message pendingMsg;
volatile bool wifiPending = false;
char wifiBuf[160];
volatile int wifiLen = 0;
volatile bool srcPending = false;
uint8_t srcMac[6];

char statusBuf[360];

static const int LOG_LINES = 30;
static const int LOG_WIDTH = 96;
char logBuf[LOG_LINES][LOG_WIDTH];
int logHead = 0;
int logCount = 0;

void logLine(const char *msg) {
  snprintf(logBuf[logHead], LOG_WIDTH, "[%lus] %s", millis() / 1000, msg);
  Serial.println(logBuf[logHead]);
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

void macToStr(const uint8_t *m, char *out) {
  snprintf(out, MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

bool parseMac(const char *str, uint8_t *out) {
  int v[6];
  if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

bool macIsZero(const uint8_t *m) {
  for (int i = 0; i < 6; i++) if (m[i] != 0) return false;
  return true;
}

void loadPrefs() {
  prefs.begin("dongle", true);
  if (prefs.getString("ssid", staSsid, SSID_LEN) == 0) staSsid[0] = '\0';
  if (prefs.getString("pass", staPass, PASS_LEN) == 0) staPass[0] = '\0';
  if (prefs.getString("txmac", txMacStr, MAC_STR_LEN) == 0) strcpy(txMacStr, "00:00:00:00:00:00");
  prefs.end();
  parseMac(txMacStr, txMac);
}

void savePrefs() {
  prefs.begin("dongle", false);
  prefs.putString("ssid", staSsid);
  prefs.putString("pass", staPass);
  prefs.putString("txmac", txMacStr);
  prefs.end();
}

void registerTxPeer() {
  if (macIsZero(txMac)) return;
  if (txPeerReady) {
    esp_now_del_peer(txMac);
    txPeerReady = false;
  }
  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, txMac, 6);
  p.channel = 0;
  p.encrypt = false;
  if (esp_now_add_peer(&p) == ESP_OK) {
    txPeerReady = true;
    char m[48];
    snprintf(m, sizeof(m), "Transmitter peer set: %s", txMacStr);
    logLine(m);
  }
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  memcpy(srcMac, info->src_addr, 6);
  srcPending = true;
  if (len >= 6 && memcmp(data, "<WIFI>", 6) == 0) {
    int n = len;
    if (n > (int)sizeof(wifiBuf) - 1) n = sizeof(wifiBuf) - 1;
    memcpy(wifiBuf, data, n);
    wifiBuf[n] = '\0';
    wifiLen = n;
    wifiPending = true;
    return;
  }
  if (len == (int)sizeof(struct_message)) {
    memcpy(&pendingMsg, data, sizeof(struct_message));
    macroPending = true;
  }
}

void broadcastIp() {
  if (!txPeerReady) return;
  char buf[24];
  int n = snprintf(buf, sizeof(buf), "<IP>%s", WiFi.localIP().toString().c_str());
  if (n < 0) return;
  esp_now_send(txMac, (uint8_t *)buf, n);
  char m[48];
  snprintf(m, sizeof(m), "Sent IP handshake: %s", WiFi.localIP().toString().c_str());
  logLine(m);
}

char translateChar(char c) {
  if (strcmp(rxLayout, "FR") == 0) {
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
  } else if (strcmp(rxLayout, "DE") == 0) {
    switch (c) {
      case 'y': return 'z';
      case 'z': return 'y';
      case 'Y': return 'Z';
      case 'Z': return 'Y';
      default: break;
    }
  } else if (strcmp(rxLayout, "JP") == 0) {
    if (c == '@') return '[';
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
  if (!strncasecmp(t, "LAYER:", 6)) return;
  if (!strncasecmp(t, "DELAY:", 6)) {
    long ms = atol(t + 6);
    if (ms > 0) delay(ms);
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

void executeMacro(const char *layoutIn, const char *macro) {
  strncpy(rxLayout, layoutIn, sizeof(rxLayout) - 1);
  rxLayout[sizeof(rxLayout) - 1] = '\0';
  char m[80];
  snprintf(m, sizeof(m), "RX macro [%s] -> %s", rxLayout, macro[0] ? macro : "(empty)");
  logLine(m);
  if (macro[0] == '\0') return;
  digitalWrite(LED_PIN, HIGH);
  char work[MACRO_LEN];
  strncpy(work, macro, MACRO_LEN - 1);
  work[MACRO_LEN - 1] = '\0';
  char *save;
  char *token = strtok_r(work, ",", &save);
  while (token != NULL) {
    sendToken(token);
    token = strtok_r(NULL, ",", &save);
  }
  digitalWrite(LED_PIN, LOW);
}

void buildStatusJson() {
  snprintf(statusBuf, sizeof(statusBuf),
    "{\"selfMac\":\"%s\",\"staConnected\":%s,\"staIp\":\"%s\",\"apIp\":\"%s\",\"txMac\":\"%s\",\"ssid\":\"%s\"}",
    WiFi.macAddress().c_str(),
    staConnected ? "true" : "false",
    WiFi.localIP().toString().c_str(),
    WiFi.softAPIP().toString().c_str(),
    txMacStr,
    staSsid);
}

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MacroPad Receiver</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1c1e21;--muted:#65676b;--accent:#3b6cf6;--border:#d8dadf;--tabbg:#e9eaee;--danger:#e23b3b;}
[data-theme="dark"]{--bg:#15171c;--card:#1e2128;--fg:#e7e9ee;--muted:#9aa0aa;--accent:#5b8cff;--border:#30343d;--tabbg:#262a32;--danger:#ff5d5d;}
*{box-sizing:border-box;}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg);transition:.2s;}
header{display:flex;align-items:center;justify-content:flex-end;padding:16px 24px 16px 104px;background:var(--card);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:5;}
h1{font-size:20px;margin:0;}
.theme-switch{position:fixed;top:16px;left:20px;z-index:50;background:none;border:none;padding:0;margin:0;cursor:pointer;display:inline-flex;align-items:center;-webkit-tap-highlight-color:transparent;}
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
.wrap{max-width:560px;margin:0 auto;padding:22px;}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:18px;margin-bottom:16px;}
h3{margin:0 0 8px;font-size:15px;}
label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px;}
input{width:100%;padding:10px;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--fg);font-size:14px;}
.macbox{font-family:monospace;font-size:18px;background:var(--tabbg);padding:12px;border-radius:8px;text-align:center;letter-spacing:1px;}
.status{font-size:13px;color:var(--muted);background:var(--tabbg);padding:10px 12px;border-radius:8px;}
button{background:var(--accent);color:#fff;border:none;padding:11px 18px;border-radius:8px;cursor:pointer;font-size:14px;margin-top:12px;width:100%;}
button.ghost{background:var(--tabbg);border:1px solid var(--border);color:var(--fg);}
.small{font-size:12px;color:var(--muted);margin-top:6px;}
#toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:var(--accent);color:#fff;padding:12px 20px;border-radius:8px;opacity:0;transition:.3s;pointer-events:none;}
#toast.show{opacity:1;}
</style>
</head>
<body>
<header>
<h1>MacroPad Receiver</h1>
</header>
<button class="theme-switch" id="themeBtn" type="button" role="switch" aria-label="Toggle light and dark theme">
<span class="switch-track">
<span class="switch-icon sun"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="4"></circle><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"></path></svg></span>
<span class="switch-icon moon"><svg viewBox="0 0 24 24" fill="currentColor"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"></path></svg></span>
<span class="switch-thumb"></span>
</span>
</button>
<div class="wrap">

<div class="card">
<h3>This Device MAC</h3>
<div class="macbox" id="mac">--:--:--:--:--:--</div>
<button class="ghost" id="copyBtn">Copy MAC</button>
<p class="small">Enter this address in the Transmitter's "Device Pairing" tab so it can sync Wi-Fi, stream macros, and flash this dongle.</p>
</div>

<div class="card">
<h3>Status</h3>
<div class="status" id="stat">Loading...</div>
</div>

<div class="card">
<h3>Manual Pairing (Fallback)</h3>
<p class="small">Normally everything auto-syncs over ESP-NOW. Use this only if you want to set values manually.</p>
<label>Transmitter MAC</label>
<input type="text" id="txmac" placeholder="AA:BB:CC:DD:EE:FF">
<label>Wi-Fi SSID</label>
<input type="text" id="ssid">
<label>Wi-Fi Password</label>
<input type="password" id="pass" placeholder="(unchanged if blank)">
<button id="saveBtn">Save Manual Settings</button>
</div>

<div class="card">
<h3>Local Firmware Update</h3>
<p class="small">Direct OTA for this dongle. The Transmitter can also flash this device remotely via the learned IP.</p>
<input type="file" id="fwfile" accept=".bin">
<button id="otaBtn">Upload &amp; Flash</button>
<div class="small" id="otaStat"></div>
</div>
</div>
<div id="toast">Saved</div>

<script>
function toast(t){const e=document.getElementById('toast');e.textContent=t;e.classList.add('show');setTimeout(()=>e.classList.remove('show'),1800);}
const themeBtn=document.getElementById('themeBtn');
function applyTheme(m){document.documentElement.setAttribute('data-theme',m);themeBtn.setAttribute('aria-checked',m==='dark'?'true':'false');localStorage.setItem('mp_theme',m);}
applyTheme(localStorage.getItem('mp_theme')||'light');
themeBtn.onclick=()=>applyTheme(document.documentElement.getAttribute('data-theme')==='dark'?'light':'dark');
function st(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    document.getElementById('mac').textContent=s.selfMac;
    document.getElementById('stat').textContent='STA: '+(s.staConnected?('Connected '+s.staIp):'Disconnected')+' | AP: '+s.apIp+' | Paired TX: '+s.txMac+' | SSID: '+(s.ssid||'(none)');
    document.getElementById('txmac').placeholder=s.txMac;
    if(!document.getElementById('ssid').value)document.getElementById('ssid').placeholder=s.ssid||'';
  });
}
document.getElementById('copyBtn').onclick=()=>{
  const t=document.getElementById('mac').textContent;
  if(navigator.clipboard){navigator.clipboard.writeText(t).then(()=>toast('MAC copied'));}else{toast(t);}
};
document.getElementById('saveBtn').onclick=()=>{
  const p=new URLSearchParams();
  p.append('txmac',document.getElementById('txmac').value);
  p.append('ssid',document.getElementById('ssid').value);
  p.append('pass',document.getElementById('pass').value);
  fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()})
    .then(r=>r.json()).then(j=>{toast(j.status||'Saved');st();});
};
document.getElementById('otaBtn').onclick=()=>{
  const f=document.getElementById('fwfile').files[0];
  const stat=document.getElementById('otaStat');
  if(!f){toast('Select a .bin file');return;}
  const fd=new FormData();fd.append('update',f,f.name);
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/api/update');
  xhr.upload.onprogress=e=>{if(e.lengthComputable)stat.textContent='Uploading... '+Math.round(e.loaded/e.total*100)+'%';};
  xhr.onload=()=>{try{const j=JSON.parse(xhr.responseText);stat.textContent=j.status;toast(j.status);}catch(e){stat.textContent='Flashed, rebooting...';}};
  xhr.onerror=()=>{stat.textContent='Connection lost (device may be rebooting)';};
  stat.textContent='Starting upload...';
  xhr.send(fd);
};
st();
setInterval(st,2000);
</script>
</body>
</html>
)rawliteral";

void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
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

void handleSave(AsyncWebServerRequest *request) {
  bool changed = false;
  if (request->hasParam("txmac", true)) {
    const char *m = request->getParam("txmac", true)->value().c_str();
    uint8_t tmp[6];
    if (parseMac(m, tmp)) {
      strncpy(txMacStr, m, MAC_STR_LEN - 1);
      txMacStr[MAC_STR_LEN - 1] = '\0';
      memcpy(txMac, tmp, 6);
      registerTxPeer();
      changed = true;
    }
  }
  if (request->hasParam("ssid", true)) {
    strncpy(staSsid, request->getParam("ssid", true)->value().c_str(), SSID_LEN - 1);
    staSsid[SSID_LEN - 1] = '\0';
    changed = true;
  }
  if (request->hasParam("pass", true)) {
    const char *p = request->getParam("pass", true)->value().c_str();
    if (strlen(p) > 0) {
      strncpy(staPass, p, PASS_LEN - 1);
      staPass[PASS_LEN - 1] = '\0';
      changed = true;
    }
  }
  if (changed) {
    savePrefs();
    logLine("Manual settings saved");
    staConnected = false;
    lastStaCheck = 0;
    if (staSsid[0] != '\0') WiFi.begin(staSsid, staPass);
  }
  request->send(200, "application/json", "{\"status\":\"Saved\"}");
}

void startNetwork() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, apPass);
  delay(150);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  char m[64];
  snprintf(m, sizeof(m), "AP up: %s @ %s", apSsid, WiFi.softAPIP().toString().c_str());
  logLine(m);
  if (staSsid[0] != '\0') {
    WiFi.begin(staSsid, staPass);
    snprintf(m, sizeof(m), "Connecting STA to: %s", staSsid);
    logLine(m);
  }
  if (MDNS.begin("macropadreceiver")) {
    MDNS.addService("http", "tcp", 80);
    logLine("mDNS: macropadreceiver.local");
  }
}

void initEspNow() {
  if (esp_now_init() != ESP_OK) {
    logLine("ESP-NOW init FAILED");
    return;
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  if (!macIsZero(txMac)) registerTxPeer();
  uint8_t ch;
  wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  char m[48];
  snprintf(m, sizeof(m), "ESP-NOW ready on channel %u", ch);
  logLine(m);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  staSsid[0] = '\0';
  staPass[0] = '\0';

  loadPrefs();
  logLine("Prefs loaded");

  Keyboard.begin();
  Mouse.begin();
  Consumer.begin();
  USB.begin();
  logLine("USB HID started");

  startNetwork();
  initEspNow();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    buildStatusJson();
    request->send(200, "application/json", statusBuf);
  });
  server.on("/api/save", HTTP_POST, handleSave);
  server.on("/api/update", HTTP_OPTIONS, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *res = request->beginResponse(204);
    res->addHeader("Access-Control-Allow-Origin", "*");
    res->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    res->addHeader("Access-Control-Allow-Headers", "*");
    request->send(res);
  });
  server.on("/api/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    bool ok = !Update.hasError();
    AsyncWebServerResponse *res = request->beginResponse(200, "application/json",
      ok ? "{\"status\":\"Receiver flash OK, rebooting\"}" : "{\"status\":\"Receiver flash FAILED\"}");
    res->addHeader("Access-Control-Allow-Origin", "*");
    res->addHeader("Connection", "close");
    request->send(res);
    if (ok) {
      delay(800);
      ESP.restart();
    }
  }, handleUpload);
  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });
  server.begin();
  logLine("Receiver web server live");
}

void handleEvents() {
  if (srcPending) {
    srcPending = false;
    if (!macIsZero(srcMac) && memcmp(srcMac, txMac, 6) != 0) {
      memcpy(txMac, srcMac, 6);
      macToStr(txMac, txMacStr);
      savePrefs();
      registerTxPeer();
      char m[48];
      snprintf(m, sizeof(m), "Auto-paired transmitter: %s", txMacStr);
      logLine(m);
    }
  }
  if (wifiPending) {
    wifiPending = false;
    char *body = wifiBuf + 6;
    char *sep = strchr(body, '|');
    if (sep) {
      *sep = '\0';
      strncpy(staSsid, body, SSID_LEN - 1);
      staSsid[SSID_LEN - 1] = '\0';
      strncpy(staPass, sep + 1, PASS_LEN - 1);
      staPass[PASS_LEN - 1] = '\0';
      savePrefs();
      char m[64];
      snprintf(m, sizeof(m), "Wi-Fi synced from TX: %s", staSsid);
      logLine(m);
      staConnected = false;
      lastStaCheck = 0;
      WiFi.begin(staSsid, staPass);
    }
  }
  if (macroPending) {
    macroPending = false;
    executeMacro(pendingMsg.layout, pendingMsg.macro);
  }
}

void maintainSta() {
  if (millis() - lastStaCheck < 3000) return;
  lastStaCheck = millis();
  if (staSsid[0] == '\0') {
    staConnected = false;
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (!staConnected) {
      staConnected = true;
      char m[64];
      snprintf(m, sizeof(m), "STA connected: %s", WiFi.localIP().toString().c_str());
      logLine(m);
      broadcastIp();
      lastIpBcast = millis();
    } else if (millis() - lastIpBcast > 10000) {
      broadcastIp();
      lastIpBcast = millis();
    }
  } else {
    if (staConnected) logLine("STA connection lost");
    staConnected = false;
    WiFi.begin(staSsid, staPass);
  }
}

void loop() {
  dnsServer.processNextRequest();
  handleEvents();
  maintainSta();
}
