#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <LCD_I2C.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ==========================================
//   WIFI SETTINGS (Station Mode Only)
// ==========================================
const char* sta_ssid     = "Vikram";
const char* sta_password = "12345678";

// ==========================================
//   TELEGRAM SETTINGS
// ==========================================
const char* telegramToken  = "8740893240:AAE51bjp03rMi1aRqbW0faUR8y3cDO6FSX4";
const char* telegramChatID = "5278123710";

// ==========================================
//   ALERT + AUTO RESTART SETTINGS
// ==========================================
const int alertCooldownSec    = 60;
const int autoRestartDelaySec = 120; // 2 Minutes

// ==========================================
//   PINS
// ==========================================
#define RELAY_PIN  D5
#define FLOW1_PIN  D6
#define FLOW2_PIN  D7
#define BUZZER_PIN D3

LCD_I2C lcd(0x27, 16, 2);
ESP8266WebServer server(80);

// ==========================================
//   SENSOR CALIBRATION
// ==========================================
const float pulsesPerLiter1        = 2290.0;
const float pulsesPerLiter2        = 5870.0;
const float outletCorrectionFactor = 0.85;

// ==========================================
//   DETECTION SETTINGS
// ==========================================
const float leakStartVolumeMl  = 300.0;
const float learnEndVolumeMl   = 900.0;
const float leakMarginPercent  = 0.25; // 25% deviation from baseline triggers leak
const int   leakConfirmSeconds = 4;
const float minFlowMlPerSec    = 8.0;
const int   startupIgnoreSec   = 5;

// ==========================================
//   STATE VARIABLES
// ==========================================
volatile unsigned long pulse1 = 0;
volatile unsigned long pulse2 = 0;

unsigned long lastTime         = 0;
unsigned long lastP1           = 0;
unsigned long lastP2           = 0;
unsigned long pumpStartTime    = 0;
unsigned long lastAlertTime    = 0;
unsigned long leakDetectedTime = 0;

float inletTotal     = 0;
float outletTotal    = 0;
float flowInlet      = 0;
float flowOutlet     = 0;
float flowRatio      = 0;
float baselineRatio  = 0;
float ratioDeviation = 0;
float alarmHigh      = 0;
float alarmLow       = 0;
float learnSumRatio  = 0;
int   learnCount     = 0;
int   phase          = 0;
int   restartCountdown = 0;

bool  pumpOn          = false;
bool  leakDetected    = false;
bool  internetOK      = false;
bool  telegramOK      = false;
bool  alertSent       = false;
int   leakCounter     = 0;
int   wifiDropCounter = 0;
bool  alertPending    = false;
bool  bootMsgPending  = false;
unsigned long bootMsgSendAt = 0;

// CSV log buffer (last 60 readings)
#define CSV_MAX 60
struct LogRow {
  unsigned long ts;
  bool pump;
  float inlet, outlet, flowIn, flowOut, ratio, baseline, deviation;
  int phase;
};
LogRow csvLog[CSV_MAX];
int csvHead = 0;
int csvCount = 0;

// ==========================================
//   INTERRUPTS
// ==========================================
void IRAM_ATTR count1() { pulse1++; }
void IRAM_ATTR count2() { pulse2++; }

void setBuzzer(bool state);
void setPump(bool state);

// ==========================================
//   SEND TELEGRAM TEXT
// ==========================================
bool sendTelegram(String message) {
  if (!internetOK) { Serial.println(F("[TG] No internet")); return false; }
  if (ESP.getFreeHeap() < 12000) { Serial.print(F("[TG] Heap low:")); Serial.println(ESP.getFreeHeap()); return false; }

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  if (!client.connect("api.telegram.org", 443)) {
    Serial.println(F("[TG] Connect failed"));
    return false;
  }

  String encodedMsg = "";
  for (int i = 0; i < (int)message.length(); i++) {
    char c = message[i];
    if      (c == '\n') encodedMsg += "%0A";
    else if (c == ' ')  encodedMsg += "%20";
    else if (c == ':')  encodedMsg += "%3A";
    else if (c == '/')  encodedMsg += "%2F";
    else if (c == '!')  encodedMsg += "%21";
    else if (c == '+')  encodedMsg += "%2B";
    else if (c == '=')  encodedMsg += "%3D";
    else if (c == '&')  encodedMsg += "%26";
    else if (c == '#')  encodedMsg += "%23";
    else if (c == '%')  encodedMsg += "%25";
    else if (c == '<')  encodedMsg += "%3C";
    else if (c == '>')  encodedMsg += "%3E";
    else                encodedMsg += c;
  }

  String path = String("/bot") + telegramToken + "/sendMessage"
              + "?chat_id=" + telegramChatID
              + "&text=" + encodedMsg
              + "&parse_mode=HTML";

  client.print(String("GET ") + path + " HTTP/1.1\r\n"
             + "Host: api.telegram.org\r\n"
             + "User-Agent: ESP8266\r\n"
             + "Connection: close\r\n\r\n");

  unsigned long t = millis();
  String status = "";
  while (client.connected() && millis() - t < 10000) {
    if (client.available()) {
      status = client.readStringUntil('\n');
      status.trim();
      break;
    }
  }
  client.stop();
  yield();

  Serial.print(F("[TG] ")); Serial.println(status);
  if (status.indexOf("200") >= 0) {
    telegramOK = true;
    Serial.println(F("[TG] SUCCESS"));
    return true;
  }
  return false;
}

// ==========================================
//   SEND TELEGRAM DOCUMENT (CSV)
// ==========================================
bool sendTelegramCSV(String caption) {
  if (!internetOK) return false;
  if (ESP.getFreeHeap() < 12000) return false;

  String csv = "Time_s,Pump,Inlet_mL,Outlet_mL,FlowIn_mLs,FlowOut_mLs,Ratio,Baseline,Deviation,Phase\n";
  int start = (csvCount < CSV_MAX) ? 0 : csvHead;
  int count = min(csvCount, CSV_MAX);
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % CSV_MAX;
    LogRow& r = csvLog[idx];
    csv += String(r.ts/1000) + ",";
    csv += String(r.pump ? "ON" : "OFF") + ",";
    csv += String(r.inlet, 1) + ",";
    csv += String(r.outlet, 1) + ",";
    csv += String(r.flowIn, 2) + ",";
    csv += String(r.flowOut, 2) + ",";
    csv += String(r.ratio, 3) + ",";
    csv += String(r.baseline, 3) + ",";
    csv += String(r.deviation, 3) + ",";
    csv += String(r.phase) + "\n";
  }

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  if (!client.connect("api.telegram.org", 443)) {
    Serial.println(F("[TG-CSV] Connect failed"));
    return false;
  }

  String boundary = "----ESP8266Boundary";
  String bodyStart = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
    + String(telegramChatID) + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
    + caption + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"document\"; filename=\"leak_data.csv\"\r\n"
    "Content-Type: text/csv\r\n\r\n";
  String bodyEnd = "\r\n--" + boundary + "--\r\n";

  int totalLen = bodyStart.length() + csv.length() + bodyEnd.length();

  client.print(String("POST /bot") + telegramToken + "/sendDocument HTTP/1.1\r\n"
             + "Host: api.telegram.org\r\n"
             + "User-Agent: ESP8266\r\n"
             + "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
             + "Content-Length: " + String(totalLen) + "\r\n"
             + "Connection: close\r\n\r\n");
  client.print(bodyStart);
  client.print(csv);
  client.print(bodyEnd);

  unsigned long t = millis();
  String status = "";
  while (client.connected() && millis() - t < 15000) {
    if (client.available()) {
      status = client.readStringUntil('\n');
      status.trim();
      break;
    }
  }
  client.stop();
  yield();

  Serial.print(F("[TG-CSV] ")); Serial.println(status);
  return status.indexOf("200") >= 0;
}

// ==========================================
//   SEND LEAK ALERT + CSV
// ==========================================
void sendLeakAlert() {
  unsigned long now = millis();
  if (alertSent && (now - lastAlertTime) < (alertCooldownSec * 1000UL)) {
    Serial.println(F("[ALERT] Cooldown"));
    return;
  }

  String vol = String(inletTotal / 1000.0, 2) + " L";

  String msg = "🚨 <b>LEAK DETECTED!</b>\n"
               "━━━━━━━━━━━━━━━━━━\n"
               "💧 Volume passed: " + vol + "\n"
               "📊 Baseline ratio: " + String(baselineRatio, 3) + "\n"
               "📉 Current ratio: " + String(flowRatio, 3) + "\n"
               "⚠️ Deviation: " + String(ratioDeviation, 3) + "\n"
               "━━━━━━━━━━━━━━━━━━\n"
               "🔴 Pump has been automatically STOPPED\n"
               "🔄 The full system will be restarted within two minutes\n"
               "📎 Sending data log...";

  sendTelegram(msg);
  delay(500);
  yield();

  String csvCaption = "📊 Leak event data log (Last 60 seconds)";
  sendTelegramCSV(csvCaption);

  lastAlertTime = now;
  alertSent     = true;
  Serial.println(F("[ALERT] Done"));
}

// ==========================================
//   PUMP CONTROL
// ==========================================
void setPump(bool state) {
  pumpOn = state;
  digitalWrite(RELAY_PIN, state ? HIGH : LOW);
  if (state) {
    pumpStartTime  = millis();
    leakCounter    = 0;
    leakDetected   = false;
    leakDetectedTime = 0;
    restartCountdown = 0;
    flowRatio = 0; baselineRatio = 0; ratioDeviation = 0;
    alarmHigh = 0; alarmLow = 0;
    learnSumRatio = 0; learnCount = 0;
    phase = 0; flowInlet = 0; flowOutlet = 0;
    alertSent = false;
    setBuzzer(false);
    noInterrupts(); pulse1 = 0; pulse2 = 0; interrupts();
    lastP1 = 0; lastP2 = 0;
    inletTotal = 0; outletTotal = 0;
  }
}

void setBuzzer(bool state) {
  digitalWrite(BUZZER_PIN, state ? LOW : HIGH);
}

// ==========================================
//   LCD
// ==========================================
void updateLCD() {
  lcd.clear();
  if (leakDetected && restartCountdown > 0) {
    lcd.setCursor(0, 0); lcd.print("!! LEAK ALERT !!");
    lcd.setCursor(0, 1); lcd.print("Restart:"); lcd.print(restartCountdown); lcd.print("s   ");
  } else if (leakDetected) {
    lcd.setCursor(0, 0); lcd.print("!! LEAK ALERT !!");
    lcd.setCursor(0, 1); lcd.print(alertSent ? "TG Sent!        " : "No Internet     ");
  } else if (phase == 0) {
    lcd.setCursor(0, 0); lcd.print("Filling Phase   ");
    lcd.setCursor(0, 1); lcd.print((int)inletTotal); lcd.print("/300mL          ");
  } else if (phase == 1) {
    lcd.setCursor(0, 0); lcd.print("Learning Phase  ");
    lcd.setCursor(0, 1); lcd.print((int)inletTotal); lcd.print("/900mL          ");
  } else {
    lcd.setCursor(0, 0);
    lcd.print("R:"); lcd.print(flowRatio, 2);
    lcd.print(" B:"); lcd.print(baselineRatio, 2);
    lcd.setCursor(0, 1);
    lcd.print("D:"); lcd.print(ratioDeviation, 3);
    lcd.print(internetOK ? " NET:OK" : " NET:--");
  }
}

// ==========================================
//   WEB DASHBOARD
// ==========================================
static const char PAGE[] PROGMEM = R"=====(
<!DOCTYPE html><html lang='en'><head>
<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>
<title>Leak Monitor Pro</title>
<link href='https://fonts.googleapis.com/css2?family=Rajdhani:wght@500;600;700&family=Inter:wght@400;500;600&display=swap' rel='stylesheet'>
<script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#060e1c;--s:#0b1826;--c:#101f33;--b:rgba(255,255,255,0.07);--ac:#00d4ff;--gn:#00ff9d;--rd:#ff3d6b;--or:#ffaa00;--pu:#a855f7;--tx:#dff0ff;--mt:#3a6080}
body{background:var(--bg);color:var(--tx);font-family:'Inter',sans-serif;min-height:100vh;padding:12px;overflow-x:hidden}
body::before{content:'';position:fixed;inset:0;background:radial-gradient(ellipse at 20% 0%,rgba(0,212,255,0.06) 0%,transparent 60%),radial-gradient(ellipse at 80% 100%,rgba(168,85,247,0.05) 0%,transparent 60%);pointer-events:none;z-index:0}
.wrap{max-width:480px;margin:0 auto;position:relative;z-index:1}
.hdr{background:var(--s);border:1px solid var(--b);border-radius:18px;padding:14px 18px;display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
.hdr-l{display:flex;align-items:center;gap:11px}
.hdr-ic{width:40px;height:40px;border-radius:12px;background:linear-gradient(135deg,#0af,#a855f7);display:flex;align-items:center;justify-content:center;font-size:20px;flex-shrink:0}
.hdr-name{font-family:'Rajdhani',sans-serif;font-size:1.25em;font-weight:700;letter-spacing:1px}
.hdr-sub{font-size:0.6em;color:var(--mt);letter-spacing:2px;text-transform:uppercase;margin-top:1px}
.hdr-heap{font-size:0.68em;color:var(--mt);background:var(--c);border:1px solid var(--b);padding:4px 10px;border-radius:8px}
.pills{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:10px}
.pill{background:var(--s);border:1px solid var(--b);border-radius:12px;padding:9px 10px;display:flex;align-items:center;gap:7px}
.pdot{width:8px;height:8px;border-radius:50%;flex-shrink:0}
.pg{background:var(--gn);box-shadow:0 0 8px var(--gn);animation:blink 2s infinite}
.po{background:var(--or)} .pb{background:var(--ac)} .pr{background:var(--rd)}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.35}}
.pill-txt .lbl{font-size:0.58em;color:var(--mt);text-transform:uppercase;letter-spacing:.5px}
.pill-txt .val{font-size:0.72em;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:80px}
.leak{display:none;background:rgba(255,61,107,0.1);border:1px solid var(--rd);border-left:4px solid var(--rd);border-radius:14px;padding:14px 16px;margin-bottom:10px}
.leak.show{display:flex;align-items:center;gap:12px;animation:lpulse 1.2s infinite}
@keyframes lpulse{0%,100%{box-shadow:0 0 0 0 rgba(255,61,107,.25)}50%{box-shadow:0 0 18px rgba(255,61,107,.15)}}
.leak-ic{font-size:26px;animation:shake .6s infinite}
@keyframes shake{0%,100%{transform:rotate(0)}25%{transform:rotate(-8deg)}75%{transform:rotate(8deg)}}
.leak-txt{flex:1}
.leak-title{font-family:'Rajdhani',sans-serif;font-size:1.1em;font-weight:700;color:var(--rd)}
.leak-sub{font-size:.72em;color:rgba(255,100,120,.85);margin-top:2px}
.leak-cd{font-family:'Rajdhani',sans-serif;font-size:2.2em;font-weight:700;color:var(--rd)}
.phase{background:var(--s);border:1px solid var(--b);border-radius:14px;padding:14px 16px;margin-bottom:10px}
.phase-top{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
.phase-name{font-family:'Rajdhani',sans-serif;font-size:.95em;font-weight:600;color:var(--ac);display:flex;align-items:center;gap:6px}
.pbadge{font-size:.6em;padding:3px 10px;border-radius:99px;font-family:'Inter',sans-serif;font-weight:600;letter-spacing:.8px;text-transform:uppercase}
.bf{background:rgba(168,85,247,.15);color:var(--pu);border:1px solid rgba(168,85,247,.3)}
.bl{background:rgba(0,212,255,.1);color:var(--ac);border:1px solid rgba(0,212,255,.2)}
.bm{background:rgba(0,255,157,.1);color:var(--gn);border:1px solid rgba(0,255,157,.2)}
.ptrack{background:rgba(255,255,255,.05);border-radius:99px;height:7px;overflow:hidden}
.pbar{height:100%;border-radius:99px;transition:width 1s ease}
.pbar.f{background:linear-gradient(90deg,var(--pu),var(--ac))}
.pbar.l{background:linear-gradient(90deg,var(--ac),var(--gn))}
.pbar.m{background:var(--gn)}
.plabels{display:flex;justify-content:space-between;font-size:.65em;color:var(--mt);margin-top:5px}
.mg{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-bottom:9px}
.mc{background:var(--c);border:1px solid var(--b);border-radius:13px;padding:13px 15px;position:relative;overflow:hidden}
.mc::after{content:'';position:absolute;top:-10px;right:-10px;width:55px;height:55px;border-radius:50%;opacity:.07}
.mc.mi::after{background:var(--ac)}.mc.mo::after{background:var(--gn)}.mc.mf::after{background:var(--pu)}.mc.mp::after{background:var(--or)}
.mc-lbl{font-size:.62em;color:var(--mt);text-transform:uppercase;letter-spacing:.7px;margin-bottom:5px;display:flex;align-items:center;gap:5px}
.mc-val{font-family:'Rajdhani',sans-serif;font-size:2em;font-weight:700;line-height:1}
.mc-unit{font-size:.45em;font-family:'Inter',sans-serif;color:var(--mt);margin-left:2px}
.ci{color:var(--ac)}.co{color:var(--gn)}.cp{color:var(--pu)}.cr{color:var(--rd)}.cg{color:var(--gn);animation:blink 1s infinite}
.rg{display:grid;grid-template-columns:1fr 1fr 1fr;gap:9px;margin-bottom:9px}
.rc{background:var(--c);border:1px solid var(--b);border-radius:13px;padding:11px 13px}
.rc-lbl{font-size:.6em;color:var(--mt);text-transform:uppercase;letter-spacing:.7px;margin-bottom:5px}
.rc-val{font-family:'Rajdhani',sans-serif;font-size:1.45em;font-weight:700}
.chrt{background:var(--c);border:1px solid var(--b);border-radius:13px;padding:14px;margin-bottom:9px}
.chrt-hdr{font-size:.65em;color:var(--mt);text-transform:uppercase;letter-spacing:1px;margin-bottom:10px;display:flex;align-items:center;gap:6px}
.ldot{width:6px;height:6px;border-radius:50%;background:var(--gn);animation:blink 1.5s infinite;flex-shrink:0}
canvas{max-height:140px!important}
.strip{background:var(--s);border:1px solid var(--b);border-radius:11px;padding:9px 14px;margin-bottom:9px;display:flex;align-items:center;justify-content:space-between;font-size:.78em}
.strip-l{display:flex;align-items:center;gap:6px;color:var(--mt)}
.brow{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-bottom:9px}
.btn{border:none;border-radius:13px;padding:14px 10px;font-family:'Rajdhani',sans-serif;font-size:1.05em;font-weight:700;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:7px;letter-spacing:.5px;transition:transform .15s,box-shadow .15s;width:100%}
.btn:active{transform:scale(.97)}
.bon{background:linear-gradient(135deg,#00c47a,#00ff9d);color:#001a0d;box-shadow:0 4px 18px rgba(0,255,157,.2)}
.bof{background:linear-gradient(135deg,#c01535,#ff3d6b);color:#fff;box-shadow:0 4px 18px rgba(255,61,107,.2)}
.bfull{width:100%;background:var(--c);border:1px solid var(--b);color:var(--tx);border-radius:13px;padding:13px;font-family:'Rajdhani',sans-serif;font-size:1em;font-weight:600;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:8px;letter-spacing:.5px;transition:border-color .2s,background .2s;margin-bottom:8px}
.bfull:hover{border-color:var(--ac);background:rgba(0,212,255,.05)}
.btg{border-color:rgba(41,182,246,.35);color:#29b6f6}
.btg:hover{border-color:#29b6f6!important;background:rgba(41,182,246,.07)!important}
</style>
</head>
<body>
<div class='wrap'>
  <div class='hdr'>
    <div class='hdr-l'><div class='hdr-ic'>💧</div>
      <div><div class='hdr-name'>LEAK MONITOR</div><div class='hdr-sub'>Pro &nbsp;·&nbsp; Station Mode</div></div>
    </div>
    <div class='hdr-heap'>Heap: <span id='heap'>--</span></div>
  </div>

  <div class='pills'>
    <div class='pill'><div class='pdot pg' id='wifiDot'></div><div class='pill-txt'><div class='lbl'>WiFi IP</div><div class='val' id='ipVal'>--</div></div></div>
    <div class='pill'><div class='pdot po' id='netDot'></div><div class='pill-txt'><div class='lbl'>Internet</div><div class='val' id='netVal'>Checking</div></div></div>
    <div class='pill'><div class='pdot pb' id='tgDot'></div><div class='pill-txt'><div class='lbl'>Telegram</div><div class='val' id='tgVal'>--</div></div></div>
  </div>

  <div class='leak' id='leakBanner'>
    <div class='leak-ic'>🚨</div>
    <div class='leak-txt'>
      <div class='leak-title'>LEAK DETECTED</div>
      <div class='leak-sub' id='leakSub'>Pump stopped</div>
    </div>
    <div class='leak-cd' id='leakCD'></div>
  </div>

  <div class='phase'>
    <div class='phase-top'>
      <div class='phase-name'>⚙ <span id='phName'>Phase 1: Filling</span></div>
      <div class='pbadge bf' id='phBadge'>FILLING</div>
    </div>
    <div class='ptrack'><div class='pbar f' id='pbar' style='width:0%'></div></div>
    <div class='plabels'><span id='pL'>0 mL</span><span id='pR'>300 mL</span></div>
  </div>

  <div class='mg'>
    <div class='mc mi'><div class='mc-lbl'>↗ Inlet Total</div><div class='mc-val ci' id='inlet'>0<span class='mc-unit'>mL</span></div></div>
    <div class='mc mo'><div class='mc-lbl'>↘ Outlet Total</div><div class='mc-val co' id='outlet'>0<span class='mc-unit'>mL</span></div></div>
    <div class='mc mf'><div class='mc-lbl'>〜 Inlet Flow</div><div class='mc-val cp' id='flow'>0.0<span class='mc-unit'>mL/s</span></div></div>
    <div class='mc mp'><div class='mc-lbl'>⏻ Pump</div><div class='mc-val cr' id='pump'>STOPPED</div></div>
  </div>

  <div class='rg'>
    <div class='rc'><div class='rc-lbl'>Ratio</div><div class='rc-val' id='ratio'>--</div></div>
    <div class='rc'><div class='rc-lbl'>Baseline</div><div class='rc-val ci' id='base'>--</div></div>
    <div class='rc'><div class='rc-lbl'>Deviation</div><div class='rc-val' id='dev'>--</div></div>
  </div>

  <div class='chrt'>
    <div class='chrt-hdr'><div class='ldot'></div>Flow History &nbsp;(Last 30s)</div>
    <canvas id='ch'></canvas>
  </div>

  <div class='strip'>
    <div class='strip-l'><div class='ldot'></div>Live Monitoring</div>
    <div id='smsg'>Waiting for pump...</div>
  </div>

  <div class='brow'>
    <button class='btn bon' onclick='ctrl(1)'>&#9654; Pump ON</button>
    <button class='btn bof' onclick='ctrl(0)'>&#9632; Pump OFF</button>
  </div>
  <button class='bfull' onclick='dlCSV()'>&#128190; Download Data Log (CSV)</button>
  <button class='bfull btg' onclick='testAlert()'>&#128248; Send Test Alert</button>
</div>

<script>
const g = (id) => document.getElementById(id);
let lb=[],iD=[],oD=[],lg=[];

const ch = new Chart(g('ch').getContext('2d'),{
  type:'line',
  data:{labels:lb,datasets:[
    {label:'Inlet mL/s',data:iD,borderColor:'#00d4ff',backgroundColor:'rgba(0,212,255,0.06)',borderWidth:2,tension:0.4,fill:true,pointRadius:0},
    {label:'Outlet mL/s',data:oD,borderColor:'#00ff9d',backgroundColor:'rgba(0,255,157,0.06)',borderWidth:2,tension:0.4,fill:true,pointRadius:0}
  ]},
  options:{responsive:true,animation:{duration:300},plugins:{legend:{labels:{color:'#3a6080',font:{size:10},boxWidth:18}}},
  scales:{x:{display:false},y:{beginAtZero:true,grid:{color:'rgba(255,255,255,0.03)'},ticks:{color:'#3a6080',font:{size:10}}}}}
});

const fmt = (ml) => ml>=1000?(ml/1000).toFixed(2)+' L':Math.round(ml)+' mL';
const ctrl = (s) => fetch('/control?state='+s);
const testAlert = () => fetch('/testalert').then(r=>r.text()).then(t=>alert(t=='Sent!'?'Test alert sent to Telegram!':'Failed - check Serial')).catch(()=>alert('Request failed'));

const poll = () => {
  fetch('/data').then(r=>r.json()).then(d=>{
    const ml=parseFloat(d.inlet),ph=parseInt(d.phase),bl=parseFloat(d.baseline),rat=parseFloat(d.ratio),dev=parseFloat(d.deviation),cd=parseInt(d.countdown);
    
    g('heap').textContent = Math.round(parseInt(d.heap)/1024)+'KB';
    g('ipVal').textContent = d.ip;
    g('inlet').innerHTML = fmt(ml) + (ml<1000?'<span class="mc-unit">mL</span>':'');
    g('outlet').innerHTML = fmt(parseFloat(d.outlet)) + (parseFloat(d.outlet)<1000?'<span class="mc-unit">mL</span>':'');
    g('flow').innerHTML = parseFloat(d.flow).toFixed(1)+'<span class="mc-unit">mL/s</span>';

    const pe=g('pump');
    if(d.pump==='ON'){pe.textContent='RUNNING';pe.className='mc-val cg';}else{pe.textContent='STOPPED';pe.className='mc-val cr';}

    const inet=d.internet==='YES';
    g('netDot').className='pdot '+(inet?'pg':'pr');
    g('netVal').textContent=inet?'Connected':'Offline';
    const tg=d.telegram==='YES';
    g('tgDot').className='pdot '+(tg?'pg':'po');
    g('tgVal').textContent=tg?'Ready':'Standby';

    // Smart UI colors scaled dynamically based on baseline proportion
    const ad=Math.abs(dev), danger=bl*0.20, warn=bl*0.10;
    const dc=(bl>0 && ad>danger)?'var(--rd)':(bl>0 && ad>warn)?'var(--or)':'var(--gn)';
    g('ratio').style.color=dc; g('ratio').textContent=rat>0?rat.toFixed(3):'--';
    g('base').textContent=bl>0?bl.toFixed(3):'--';
    const dv=g('dev'); dv.style.color=dc; dv.textContent=rat>0?((dev>=0?'+':'')+dev.toFixed(3)):'--';

    const pb=g('pbar'),pn=g('phName'),pbg=g('phBadge');
    if(ph===0){pn.textContent='Phase 1: Filling (0-300 mL)';pbg.textContent='FILLING';pbg.className='pbadge bf';pb.className='pbar f';pb.style.width=Math.min(100,ml/3)+'%';g('pL').textContent=Math.round(ml)+' mL';g('pR').textContent='300 mL';}
    else if(ph===1){pn.textContent='Phase 2: Learning (300-900 mL)';pbg.textContent='LEARNING';pbg.className='pbadge bl';pb.className='pbar l';pb.style.width=Math.min(100,(ml-300)/6)+'%';g('pL').textContent=Math.round(ml)+' mL';g('pR').textContent='900 mL';}
    else{pn.textContent='Phase 3: Monitoring Active';pbg.textContent='MONITORING';pbg.className='pbadge bm';pb.className='pbar m';pb.style.width='100%';}

    const lb2=g('leakBanner');
    if(d.leak==='YES'){lb2.className='leak show';g('leakSub').textContent=tg?'Pump stopped \u2014 Telegram alert sent':'Pump stopped \u2014 no internet';g('leakCD').textContent=cd>0?cd+'s':'';}
    else lb2.className='leak';

    const sm=g('smsg');
    if(d.leak==='YES')sm.textContent='LEAK \u2014 pump stopped, restarting in '+cd+'s';
    else if(d.pump==='ON')sm.textContent=ph===0?'Filling up...':ph===1?'Learning baseline...':'Monitoring active';
    else sm.textContent='Pump is off';

    const t=new Date().toLocaleTimeString();
    if(lb.length>=30){lb.shift();iD.shift();oD.shift();}
    lb.push(t);iD.push(parseFloat(d.flow));oD.push(parseFloat(d.outflow));ch.update('none');
    lg.push({t,pump:d.pump,inlet:d.inlet,outlet:d.outlet,flow:d.flow,outflow:d.outflow,ratio:d.ratio,baseline:d.baseline,deviation:d.deviation,phase:d.phase,leak:d.leak});
  }).catch(()=>{g('smsg').textContent='Connection lost - retrying...';});
};

setInterval(poll,1000); poll();

const dlCSV = () => {
  const hd='Time,Pump,Inlet_mL,Outlet_mL,FlowIn,FlowOut,Ratio,Baseline,Deviation,Phase,Leak\n';
  const rows=lg.map(r=>[r.t,r.pump,r.inlet,r.outlet,r.flow,r.outflow,r.ratio,r.baseline,r.deviation,r.phase,r.leak].join(','));
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([hd+rows.join('\n')],{type:'text/csv'}));
  a.download='leak_log_'+new Date().toISOString().slice(0,10)+'.csv';
  a.click();
};
</script>
</body></html>
)=====";

void handleRoot() {
  server.send_P(200, "text/html", PAGE);
}

// ==========================================
//   DATA API
// ==========================================
void handleData() {
  String j = "{";
  j += "\"pump\":\""     + String(pumpOn ? "ON" : "OFF")       + "\",";
  j += "\"inlet\":"      + String(inletTotal, 1)               + ",";
  j += "\"outlet\":"     + String(outletTotal, 1)              + ",";
  j += "\"flow\":"       + String(flowInlet, 1)                + ",";
  j += "\"outflow\":"    + String(flowOutlet, 1)               + ",";
  j += "\"ratio\":"      + String(flowRatio, 3)                + ",";
  j += "\"baseline\":"   + String(baselineRatio, 3)            + ",";
  j += "\"deviation\":"  + String(ratioDeviation, 3)           + ",";
  j += "\"phase\":"      + String(phase)                       + ",";
  j += "\"countdown\":"  + String(restartCountdown)            + ",";
  j += "\"heap\":"       + String(ESP.getFreeHeap())           + ",";
  j += "\"ip\":\""       + WiFi.localIP().toString()           + "\",";
  j += "\"internet\":\"" + String(internetOK ? "YES" : "NO")   + "\",";
  j += "\"telegram\":\"" + String(telegramOK ? "YES" : "NO")   + "\",";
  j += "\"leak\":\""     + String(leakDetected ? "YES" : "NO") + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

void handleControl() {
  if (server.hasArg("state")) {
    bool s = server.arg("state") == "1";
    if (!s || !leakDetected) setPump(s);
  }
  server.send(200, "text/plain", "OK");
}

void handleTestAlert() {
  String msg = "✅ <b>Leak Monitor Pro</b>\n"
               "Test alert from your device.\n"
               "Telegram alerts are working!";
  bool ok = sendTelegram(msg);
  server.send(200, "text/plain", ok ? "Sent!" : "Failed");
}

// ==========================================
//   SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== Leak Monitor Pro v2.0 ==="));

  pinMode(RELAY_PIN,  OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(RELAY_PIN,  LOW);
  digitalWrite(BUZZER_PIN, HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(sta_ssid, sta_password);
  Serial.print(F("Connecting WiFi"));
  
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    internetOK = true;
    Serial.print(F("\nSTA IP: ")); Serial.println(WiFi.localIP());
    
    configTime(5 * 3600 + 30 * 60, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print(F("NTP sync"));
    time_t now = time(nullptr);
    int ntpTry = 0;
    while (now < 100000 && ntpTry < 40) {
      delay(500); Serial.print("."); now = time(nullptr); ntpTry++;
    }
    Serial.println(now >= 100000 ? F("\nNTP OK!") : F("\nNTP timed out"));
  } else {
    Serial.println(F("\nWiFi failed!"));
  }

  server.on("/",          handleRoot);
  server.on("/data",      handleData);
  server.on("/control",   handleControl);
  server.on("/testalert", handleTestAlert);
  server.begin();
  Serial.println(F("HTTP server started"));

  pinMode(FLOW1_PIN, INPUT_PULLUP);
  pinMode(FLOW2_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW1_PIN), count1, FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW2_PIN), count2, FALLING);

  Wire.begin(D2, D1);
  lcd.begin();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Leak Monitor Pro");
  if (internetOK) {
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
  } else {
    lcd.setCursor(0, 1); lcd.print("No WiFi!        ");
  }
  delay(2000);

  Serial.print(F("Boot heap: ")); Serial.println(ESP.getFreeHeap());

  if (internetOK) {
    bootMsgPending = true;
    bootMsgSendAt  = millis() + 8000;
  }

  lastTime = millis();
}

// ==========================================
//   MAIN LOOP
// ==========================================
void loop() {
  server.handleClient();

  // Custom Boot Telegram message
  if (bootMsgPending && millis() >= bootMsgSendAt) {
    bootMsgPending = false;
    String msg = "✅ <b>Leak Monitor Pro Online</b>\n"
                 "━━━━━━━━━━━━━━━━━━\n"
                 "📶 IP: " + WiFi.localIP().toString() + "\n"
                 "🌐 Dashboard: http://" + WiFi.localIP().toString() + "\n"
                 "━━━━━━━━━━━━━━━━━━\n"
                 "The system is ready. Open this IP in the browser with the same Wi-Fi.\n"
                 "Monitoring active. You will receive an alert if a leak is detected.";
    bool ok = sendTelegram(msg);
    Serial.println(ok ? F("[BOOT] Telegram sent!") : F("[BOOT] Telegram failed."));
  }

  // Leak alert
  if (alertPending) {
    alertPending = false;
    delay(50);
    sendLeakAlert();
  }

  // 1-second tick
  unsigned long now = millis();
  if (now - lastTime < 1000) return;
  lastTime = now;

  if (WiFi.status() != WL_CONNECTED) {
    internetOK = false;
    wifiDropCounter++;
    if (wifiDropCounter == 1) {
      Serial.println(F("[WiFi] Connection lost!"));
    }
    if (wifiDropCounter >= 60) {
      wifiDropCounter = 0;
      Serial.println(F("[WiFi] Attempting reconnect..."));
      WiFi.disconnect();
      delay(100);
      WiFi.begin(sta_ssid, sta_password);
      
      unsigned long wt = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - wt < 10000) {
        server.handleClient();
        delay(200);
      }
      if (WiFi.status() == WL_CONNECTED) {
        internetOK = true;
        Serial.print(F("[WiFi] Reconnected! IP: "));
        Serial.println(WiFi.localIP());
      } else {
        Serial.println(F("[WiFi] Reconnect failed, will retry in 60s"));
      }
    }
  } else {
    if (!internetOK) {
      Serial.print(F("[WiFi] Back online! IP: "));
      Serial.println(WiFi.localIP());
    }
    internetOK = true;
    wifiDropCounter = 0;
  }

  // Auto-restart after leak
  if (leakDetected && leakDetectedTime > 0) {
    unsigned long elapsed = (millis() - leakDetectedTime) / 1000;
    if (elapsed < (unsigned long)autoRestartDelaySec) {
      restartCountdown = autoRestartDelaySec - (int)elapsed;
    } else {
      restartCountdown = 0;
      Serial.println(F("[PUMP] Auto restarting"));
      setPump(true);
      leakDetectedTime = 0;
      return;
    }
  }

  noInterrupts();
  unsigned long p1 = pulse1, p2 = pulse2;
  interrupts();

  inletTotal  = (p1 * 1000.0) / pulsesPerLiter1;
  outletTotal = ((p2 * 1000.0) / pulsesPerLiter2) * outletCorrectionFactor;

  unsigned long d1 = p1 - lastP1, d2 = p2 - lastP2;
  lastP1 = p1; lastP2 = p2;

  flowInlet  = (d1 * 1000.0) / pulsesPerLiter1;
  flowOutlet = ((d2 * 1000.0) / pulsesPerLiter2) * outletCorrectionFactor;

  flowRatio = 0; ratioDeviation = 0;

  bool warmup   = (millis() - pumpStartTime) > ((unsigned long)startupIgnoreSec * 1000);
  bool goodFlow = (flowInlet > minFlowMlPerSec);

  if (inletTotal < leakStartVolumeMl) {
    phase = 0; leakCounter = 0;

  } else if (inletTotal < learnEndVolumeMl) {
    phase = 1; leakCounter = 0;
    if (pumpOn && goodFlow && warmup) {
      float r = flowOutlet / flowInlet;
      if (r > 0.1) { // Removed the arbitrary 2.0 ceiling so your ~7.9 ratio actually learns!
        learnSumRatio += r; learnCount++;
        baselineRatio  = learnSumRatio / learnCount;
        // Visual bounds scaled to your baseline
        alarmHigh      = baselineRatio + (baselineRatio * leakMarginPercent);
        alarmLow       = baselineRatio - (baselineRatio * leakMarginPercent);
      }
    }

  } else {
    phase = 2;
    if (pumpOn && goodFlow && warmup && baselineRatio > 0) {
      flowRatio      = flowOutlet / flowInlet;
      ratioDeviation = flowRatio - baselineRatio;

      // Leak condition dynamically scales: e.g. 25% of baseline
      float currentMargin = baselineRatio * leakMarginPercent;
      bool leakCond  = (ratioDeviation < -currentMargin) || (ratioDeviation > currentMargin);

      if (leakCond) {
        leakCounter++;
      } else {
        if (leakCounter > 0) leakCounter--; // Anti-jitter decay prevents bubbles from resetting the leak
      }

      // Automatically Stop Pump and Prepare Alerts
      if (leakCounter >= leakConfirmSeconds) {
        leakDetected     = true;
        leakDetectedTime = millis();
        restartCountdown = autoRestartDelaySec; // Set to 120s
        setPump(false); // 🔴 This guarantees the pump stops instantly!
        setBuzzer(true);
        alertPending = true; // Signals the background task to send Telegram + CSV
      }
    }
  }

  // Log to CSV buffer
  csvLog[csvHead] = {
    millis(), pumpOn,
    inletTotal, outletTotal,
    flowInlet, flowOutlet,
    flowRatio, baselineRatio, ratioDeviation,
    phase
  };
  csvHead = (csvHead + 1) % CSV_MAX;
  if (csvCount < CSV_MAX) csvCount++;

  Serial.printf("\n[%lus] Heap:%uB Pump:%s Phase:%d\n",
    millis()/1000, ESP.getFreeHeap(), pumpOn?"ON":"OFF", phase);
  Serial.printf("  Inlet:%.0fmL FlowIn:%.1f FlowOut:%.1f\n",
    inletTotal, flowInlet, flowOutlet);
  Serial.printf("  Ratio:%.3f Base:%.3f Dev:%.3f\n",
    flowRatio, baselineRatio, ratioDeviation);
  if (leakDetected)
    Serial.printf("  *** LEAK! Restart in %ds ***\n", restartCountdown);

  updateLCD();
}
