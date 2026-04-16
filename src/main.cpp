#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>
#include <SPI.h>
#include <SD.h>

String GOOGLE_SCRIPT_URL =

// ── Hardware Pins ─────────────────────────────────────────────────────────────
const int relayPin        = 27;
const int statusLedPin    = 2;
const int sdChipSelect    = 5;
const int configButtonPin = 0;   // <--- NEW: The built-in "BOOT" button on the ESP32
// ── RTC & SD ──────────────────────────────────────────────────────────────────
RTC_DS3231 rtc;
bool rtcOK = false;
bool sdOK  = false;

// ── Constants ─────────────────────────────────────────────────────────────────
const int   WIFIMANAGER_TIMEOUT  = 180;
const char* AP_NAME              = "Generator-Monitor";
const char* AP_PASSWORD          = "12345678";

const unsigned long RUNNING_LOG_INTERVAL = 30000;   // 30 seconds
const unsigned long WIFI_CHECK_INTERVAL  = 5000;    // 5 seconds
const unsigned long RECONNECT_INTERVAL   = 30000;   // 30 seconds 

// ── File Paths ────────────────────────────────────────────────────────────────
const char* PENDING_FILE  = "/pending.csv";
const char* LOGBOOK_FILE  = "/logbook.csv";
const char* UPLOADED_FILE = "/uploaded.csv";

// ── Preferences ───────────────────────────────────────────────────────────────
Preferences prefs;

// ── Debounce & Generator State ────────────────────────────────────────────────
int currentReading             = LOW;
int lastReading                = LOW;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

bool          isGeneratorOn      = false;
unsigned long genStartTime       = 0;
DateTime      genStartDateTime;
unsigned long lastRunningLogTime = 0;

// ── WiFi & Queue State ────────────────────────────────────────────────────────
bool          wifiOK               = false;
bool          wasOffline           = false;
unsigned long lastWifiCheck        = 0;
unsigned long lastReconnectAttempt = 0;
uint32_t      flushPos             = 0; // Remembers exact byte location in the SD card

// ── Daily Ping ────────────────────────────────────────────────────────────────
int lastDailyPingDay = -1;


// ── Forward Declarations ──────────────────────────────────────────────────────
String getRTCTimestamp();
String formatTimeAMPM(DateTime dt);
String formatDateShort(DateTime dt);
void   logSerial(String msg);
void   appendCSV(const char* path, String row);
void   writeToSD(String dateStr, String startStr, String runStr, String stopStr, String stateStr);
void   writeToPending(String dateStr, String startStr, String runStr, String stopStr, String stateStr, String durationSec, String tsStr);
bool   doHttpSend(String status, String duration, String ts, String dateStr, String startStr, String runStr, String stopStr);
void   processNextPendingRow();
bool   connectWiFi();
void   sendToGoogle(String status, String duration, String dateStr, String startStr, String runStr, String stopStr);


// ═════════════════════════════════════════════════════════════════════════════
// RTC HELPERS
// ═════════════════════════════════════════════════════════════════════════════

void initRTC() {
  Wire.begin(21, 22);
  if (!rtc.begin()) {
    Serial.println("⚠ RTC NOT FOUND! Check wiring.");
    rtcOK = false;
    return;
  }
  rtcOK = true;
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println("⚠ RTC lost power — set to compile time.");
  }
  Serial.println("✅ RTC OK — " + getRTCTimestamp());
}

String getRTCTimestamp() {
  if (!rtcOK) return "RTC_ERROR";
  DateTime now = rtc.now();
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  return String(buf);
}

int getRTCDay() {
  if (!rtcOK) return -1;
  return rtc.now().day();
}

String formatTimeAMPM(DateTime dt) {
  int h = dt.hour(), m = dt.minute(), s = dt.second();
  String ampm = (h >= 12) ? "PM" : "AM";
  if (h == 0) h = 12;
  else if (h > 12) h -= 12;
  char buf[15];
  snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", h, m, s, ampm.c_str());
  return String(buf);
}

String formatDateShort(DateTime dt) {
  String yr = String(dt.year()).substring(2);
  return String(dt.day()) + "-" + String(dt.month()) + "-" + yr;
}

void checkSerialTimeSet() {
  if (!Serial.available()) return;
  String input = Serial.readStringUntil('\n');
  input.trim();
  if (!input.startsWith("SET:")) return;
  String dt = input.substring(4);
  int yr = dt.substring(0,4).toInt(),  mo = dt.substring(5,7).toInt(),
      dy = dt.substring(8,10).toInt(), hr = dt.substring(11,13).toInt(),
      mn = dt.substring(14,16).toInt(),sc = dt.substring(17,19).toInt();
  if (yr > 2020 && mo >= 1 && mo <= 12) {
    rtc.adjust(DateTime(yr, mo, dy, hr, mn, sc));
    Serial.println("✅ RTC Updated: " + getRTCTimestamp());
  }
}


// ═════════════════════════════════════════════════════════════════════════════
// SD HELPERS
// ═════════════════════════════════════════════════════════════════════════════

void initSD() {
  if (!SD.begin(sdChipSelect)) {
    Serial.println("⚠ SD Card Mount Failed! Check wiring/format.");
    sdOK = false;
    return;
  }
  sdOK = true;
  Serial.println("✅ SD Card Mounted.");

  if (!SD.exists(LOGBOOK_FILE)) {
    File f = SD.open(LOGBOOK_FILE, FILE_WRITE);
    if (f) {
      f.println("Date,Start,Running Time,Stop,States");
      f.close();
      Serial.println("📝 Created logbook.csv");
    }
  }
  
  if (!SD.exists(UPLOADED_FILE)) {
    File uf = SD.open(UPLOADED_FILE, FILE_WRITE);
    if (uf) {
      uf.println("Date,Start,Running Time,Stop,States,Duration,Timestamp");
      uf.close();
    }
  }
}

void appendCSV(const char* path, String row) {
  if (!sdOK) return;
  File f = SD.open(path, FILE_APPEND);
  if (f) {
    f.println(row);
    f.close();
  } else {
    Serial.println("⚠ Failed to open: " + String(path));
  }
}

void writeToSD(String dateStr, String startStr, String runStr,
               String stopStr, String stateStr) {
  String row = dateStr + "," + startStr + "," + runStr + "," +
               stopStr + "," + stateStr;
  appendCSV(LOGBOOK_FILE, row);
  logSerial("💾 SD logbook: " + row);
}

void writeToPending(String dateStr, String startStr, String runStr,
                    String stopStr,  String stateStr,
                    String durationSec, String tsStr) {
  String row = dateStr   + "," + startStr + "," + runStr    + "," +
               stopStr   + "," + stateStr + "," + durationSec + "," + tsStr;
  appendCSV(PENDING_FILE, row);
  logSerial("📋 Pending queue: " + row);
}


// ═════════════════════════════════════════════════════════════════════════════
// LED & LOGGING
// ═════════════════════════════════════════════════════════════════════════════

void ledBlink(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(statusLedPin, HIGH); delay(delayMs);
    digitalWrite(statusLedPin, LOW);  delay(delayMs);
  }
}

void logSerial(String msg) {
  Serial.println("[" + getRTCTimestamp() + "] " + msg);
}


// ═════════════════════════════════════════════════════════════════════════════
// WIFI & HTTP
// ═════════════════════════════════════════════════════════════════════════════

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(); 

  logSerial("⏳ Trying to connect to WiFi...");

  unsigned long start = millis();
  
  while (millis() - start < 10000) { 
    if (WiFi.status() == WL_CONNECTED) { 
      wifiOK = true; 
      prefs.putBool("wifi_saved", true); 
      logSerial("✅ WiFi Connected!");
      return true; 
    }
    delay(500);
  }
  
  if (!prefs.getBool("wifi_saved", false)) {
    logSerial("⚠ No WiFi credentials saved. Starting WiFiManager...");
    WiFiManager wm;
    wm.setConfigPortalTimeout(WIFIMANAGER_TIMEOUT);
    if (wm.startConfigPortal(AP_NAME, AP_PASSWORD)) {
      prefs.putBool("wifi_saved", true);
      wifiOK = true;
      return true;
    }
  } else {
    logSerial("⚠ Router offline. Bypassing portal and continuing offline (will retry in background).");
  }
  
  wifiOK = false;
  wasOffline = true;
  return false;
}

bool doHttpSend(String status, String duration, String ts, 
                String dateStr, String startStr, String runStr, String stopStr) {
  HTTPClient http;
  
  String url = GOOGLE_SCRIPT_URL +
               "?status="    + status   +
               "&duration="  + duration +
               "&timestamp=" + ts       +
               "&date="      + dateStr  +
               "&start="     + startStr +
               "&run="       + runStr   +
               "&stop="      + stopStr;
               
  url.replace(" ", "%20");
  
  http.begin(url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  http.end();
  return (code == 200);
}


// ═════════════════════════════════════════════════════════════════════════════
// EXTREME OFFLINE PROCESSOR (CRASH-PROOF)
// ═════════════════════════════════════════════════════════════════════════════

void processNextPendingRow() {
  if (!sdOK || !SD.exists(PENDING_FILE)) return;

  File f = SD.open(PENDING_FILE, FILE_READ);
  if (!f) return;

  // Jump to the exact byte where we left off yesterday/last hour
  if (flushPos > f.size()) flushPos = 0; 
  f.seek(flushPos);

  if (!f.available()) {
      // Reached the very end of the file. Queue is empty!
      f.close();
      SD.remove(PENDING_FILE);
      flushPos = 0;
      prefs.putUInt("flushPos", 0);
      logSerial("🎉 All offline backlog data uploaded successfully!");
      return;
  }

  // Read one single line and close file immediately so SD card stays safe
  String line = f.readStringUntil('\n');
  uint32_t nextPos = f.position(); 
  f.close(); 

  line.trim();
  if (line.length() == 0) {
      flushPos = nextPos;
      prefs.putUInt("flushPos", flushPos);
      return;
  }

  // Break line into columns manually
  String cols[7];
  int colIdx = 0, start = 0;
  for (int c = 0; c <= (int)line.length() && colIdx < 7; c++) {
    if (c == (int)line.length() || line[c] == ',') {
      cols[colIdx++] = line.substring(start, c);
      start = c + 1;
    }
  }

  // If row is corrupted, skip it but save position
  if (colIdx < 7) {
      logSerial("⚠ Skipping malformed row.");
      flushPos = nextPos;
      prefs.putUInt("flushPos", flushPos);
      return;
  }

  // Send to Google
  if (doHttpSend(cols[4], cols[5], cols[6], cols[0], cols[1], cols[2], cols[3])) {
      appendCSV(UPLOADED_FILE, line);
      logSerial("📤 Backlog Sent: " + cols[4] + " @ " + cols[6]);
      // Save position to permanent memory. If power fails 1 ms from now, it remembers!
      flushPos = nextPos;
      prefs.putUInt("flushPos", flushPos);
  } else {
      logSerial("❌ Internet drop detected. Pausing backlog upload.");
  }
}


// ═════════════════════════════════════════════════════════════════════════════
// WIFI HEALTH CHECK
// ═════════════════════════════════════════════════════════════════════════════

void checkWiFiHealth() {
  if (millis() - lastWifiCheck < WIFI_CHECK_INTERVAL) return;
  lastWifiCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiOK) {
      wifiOK     = false;
      wasOffline = true;
      digitalWrite(statusLedPin, LOW);
      logSerial("⚠ WiFi lost — offline mode. Events buffered to pending.csv");
    }
    
    if (millis() - lastReconnectAttempt > RECONNECT_INTERVAL) {
      lastReconnectAttempt = millis();
      logSerial("🔄 Attempting background WiFi reconnect...");
      WiFi.reconnect(); 
    }
    
  } else {
    if (!wifiOK) {
      wifiOK = true;
      digitalWrite(statusLedPin, HIGH);
      logSerial("✅ WiFi restored — background syncing active...");
    }
  }
}


// ═════════════════════════════════════════════════════════════════════════════
// MAIN SEND FUNCTION 
// ═════════════════════════════════════════════════════════════════════════════

void sendToGoogle(String status, String duration,
                  String dateStr, String startStr,
                  String runStr,  String stopStr) {
  String ts = getRTCTimestamp();

  // If there's an existing backlog, we MUST append to SD card even if online
  // so the data arrives at Google Sheets in the correct chronological order.
  if (WiFi.status() != WL_CONNECTED || (sdOK && SD.exists(PENDING_FILE))) {
    writeToPending(dateStr, startStr, runStr, stopStr, status, duration, ts);
  } else {
    if (doHttpSend(status, duration, ts, dateStr, startStr, runStr, stopStr)) {
      String row = dateStr + "," + startStr + "," + runStr + "," + stopStr + "," + status + "," + duration + "," + ts;
      appendCSV(UPLOADED_FILE, row);
      logSerial("✅ Direct send to Google successful.");
    } else {
      logSerial("❌ Direct send failed (No Internet). Buffering to SD Card.");
      writeToPending(dateStr, startStr, runStr, stopStr, status, duration, ts);
    }
  }
}


// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(relayPin,        INPUT_PULLDOWN);
  pinMode(statusLedPin,    OUTPUT);
  pinMode(configButtonPin, INPUT_PULLUP);  // <--- NEW: Setup the BOOT button
  
  digitalWrite(statusLedPin, LOW);
  prefs.begin("genmon", false);

  currentReading = digitalRead(relayPin);
  lastReading    = currentReading;
  isGeneratorOn  = (currentReading == HIGH);

  initRTC();
  initSD();
  
  connectWiFi();

  // Load the exact byte position we left off at during the last power failure
  flushPos = prefs.getUInt("flushPos", 0);

  genStartDateTime = rtc.now();

  bool savedState = prefs.getBool("lastState", false);
  if (savedState == true && currentReading == LOW) {
    logSerial("⚠ Recovered missed STOP event from memory.");
    DateTime now = rtc.now();
    
    uint32_t savedStartUnix = prefs.getUInt("startTime", now.unixtime());
    DateTime originalStart(savedStartUnix);
    unsigned long durSec = now.unixtime() - savedStartUnix;

    writeToSD(formatDateShort(now), formatTimeAMPM(originalStart), "?", formatTimeAMPM(now), "OFF(recovered)");
    sendToGoogle("STOPPED_RECOVERED", String(durSec),
                 formatDateShort(now), formatTimeAMPM(originalStart), "?", formatTimeAMPM(now));
    prefs.putBool("lastState", false);

  } else if (savedState == true && currentReading == HIGH) {
    uint32_t savedStartUnix = prefs.getUInt("startTime", rtc.now().unixtime());
    genStartDateTime = DateTime(savedStartUnix);
    
    uint32_t elapsedSec = rtc.now().unixtime() - savedStartUnix;
    genStartTime = millis() - (elapsedSec * 1000);
    
    logSerial("⚠ Resumed tracking. Original start: " + formatTimeAMPM(genStartDateTime));
  }

  lastDailyPingDay     = getRTCDay();
  lastWifiCheck        = millis();
  lastReconnectAttempt = millis();
  lastRunningLogTime   = millis();

  logSerial("✅ Boot complete. Generator is " +
            String(isGeneratorOn ? "RUNNING" : "OFF") + ". Monitoring started.");
  ledBlink(3, 200);
}


// ═════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
  checkSerialTimeSet();
  checkWiFiHealth();

  // Background processor: Automatically uploads offline backlog 1 line at a time
  static unsigned long lastQueueProcess = 0;
  if (WiFi.status() == WL_CONNECTED && sdOK && SD.exists(PENDING_FILE)) {
      if (millis() - lastQueueProcess > 1500) { // Sends 1 row every 1.5 seconds
          lastQueueProcess = millis();
          processNextPendingRow();
      }
  }

  int reading = digitalRead(relayPin);
  if (reading != lastReading) lastDebounceTime = millis();

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != currentReading) {
      currentReading = reading;
      DateTime now   = rtc.now();

      if (currentReading == HIGH) {
        isGeneratorOn      = true;
        genStartTime       = millis();
        genStartDateTime   = now;
        lastRunningLogTime = millis();
        prefs.putBool("lastState", true);
        
        prefs.putUInt("startTime", now.unixtime());

        String dateStr = formatDateShort(now);
        String timeStr = formatTimeAMPM(now);

        writeToSD(dateStr, timeStr, timeStr, "", "ON");
        logSerial("🟢 Generator STARTED");
        sendToGoogle("STARTED", "0", dateStr, timeStr, timeStr, "");

      } else {
        isGeneratorOn = false;
        unsigned long durSec = (millis() - genStartTime) / 1000;
        prefs.putBool("lastState", false);

        DateTime now    = rtc.now();
        String dateStr  = formatDateShort(now);
        String startStr = formatTimeAMPM(genStartDateTime);
        String stopStr  = formatTimeAMPM(now);

        writeToSD(dateStr, startStr, stopStr, stopStr, "OFF");
        logSerial("🔴 Generator STOPPED — " + String(durSec) + "s");
        sendToGoogle("STOPPED", String(durSec),
                     dateStr, startStr, stopStr, stopStr);
      }
    }
  }
  // ── NEW: HOLD BOOT BUTTON FOR 3 SECONDS TO OPEN HOTSPOT ──
  if (digitalRead(configButtonPin) == LOW) {
    unsigned long pressStartTime = millis();
    bool buttonHeldLongEnough = false;
    
    // Check if the button is held down continuously for 3 seconds
    while (digitalRead(configButtonPin) == LOW) {
      if (millis() - pressStartTime > 3000) {
        buttonHeldLongEnough = true;
        break;
      }
      delay(10);
    }

    if (buttonHeldLongEnough) {
      logSerial("⚙️ Config Button held! Opening WiFi Hotspot...");
      digitalWrite(statusLedPin, HIGH); // Turn LED solid to show it's in Setup Mode
      
      WiFiManager wm;
      // Allow 3 minutes to enter the password
      wm.setConfigPortalTimeout(180); 
      
      if (wm.startConfigPortal(AP_NAME, AP_PASSWORD)) {
        logSerial("✅ New WiFi credentials saved! Rebooting to apply...");
        prefs.putBool("wifi_saved", true);
        delay(1000);
        ESP.restart(); // Reboot to connect to the new WiFi cleanly
      } else {
        logSerial("⚠ Setup timed out. Returning to offline monitoring.");
        digitalWrite(statusLedPin, LOW);
      }
    }
  }
  lastReading = reading;

  if (isGeneratorOn && (millis() - lastRunningLogTime >= RUNNING_LOG_INTERVAL)) {
    unsigned long runSec = (millis() - genStartTime) / 1000;
    DateTime now = rtc.now();

    String dateStr  = formatDateShort(now);
    String startStr = formatTimeAMPM(genStartDateTime);
    String runStr   = formatTimeAMPM(now);

    writeToSD(dateStr, startStr, runStr, "", "ON");
    logSerial("⚡ RUNNING — " + String(runSec) + "s");
    sendToGoogle("RUNNING_LOG", String(runSec),
                 dateStr, startStr, runStr, "");
    lastRunningLogTime = millis();
  }

  int today = getRTCDay();
  if (today != -1 && today != lastDailyPingDay) {
    DateTime now = rtc.now();
    sendToGoogle("DAILY_PING", isGeneratorOn ? "1" : "0",
                 formatDateShort(now), "", "", "");
    lastDailyPingDay = today;
  }
}
