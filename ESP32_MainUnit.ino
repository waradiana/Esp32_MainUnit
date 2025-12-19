/* sketch_dec1a_optimized.ino
   Versi optimasi non-blocking (tetap memakai delay yang aman melalui shortDelay)
   Preservasi: semua fungsi, alur, dan fitur tetap sama dengan code asli.
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Fingerprint.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <UniversalTelegramBot.h>
#include <PubSubClient.h>

// ---------------- PIN CONFIG ----------------
#define SS_PIN 4
#define RST_PIN 15
#define LED_OK 2
#define LED_FAIL 4
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define BUZZER_PIN 26

// Fingerprint LED params (library specific)
#define FINGERPRINT_LED_RED 0x01
#define FINGERPRINT_LED_BLUE 0x06
#define FINGERPRINT_LED_GREEN 0x04

// ---------------- EEPROM CONFIG ----------------
#define EEPROM_SIZE 4096
#define NAME_LENGTH 20
#define UID_LENGTH 16
#define RECORD_SIZE (NAME_LENGTH + UID_LENGTH)
#define MAX_ID 127
#define RECORD_START 1

// ---------------- WIFI CONFIG ----------------
#define WIFI_SSID "Tenda_C3EBF0"
#define WIFI_PASS "walkreach443"
#define MQTT_SERVER "192.168.0.161"
#define MQTT_PORT 1883
#define TOPIC_SOLENOID "smart/lock/security/system/solenoid/lock"
#define TOPIC_SWITCH "smart/lock/security/system/door/switch"

// ---------------- FIREBASE CONFIG ----------------
#define FIREBASE_URL "https://uji-coba-database-fec13-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH "a3LZSV541J0LeKr8GQxIgxCfUsfON3cUOs7klXhd"
#define NODE_USERS "users"
#define NODE_LOG "log"

// ---------------- TELEGRAM CONFIG ----------------
#define BOT_TOKEN_ADMIN "8419291391:AAFqxD-k1n9-TMleOZVKR1a2BXbFN0mMzCE"
#define BOT_TOKEN_LOG "7893459259:AAGAwjOYaLDlPb6yiS51q9kmNX53cti5yHk"
#define CHAT_ID "8385631359"

WiFiClient espMqtt;
PubSubClient mqttClient(espMqtt);
String lastSwitchStatus = "";

// Sensor & display objects
MFRC522 rfid(SS_PIN, RST_PIN);
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger(&mySerial);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Telegram
WiFiClientSecure client;
UniversalTelegramBot botAdmin(BOT_TOKEN_ADMIN, client);
UniversalTelegramBot botLog(BOT_TOKEN_LOG, client);

// Timing / flags
unsigned long lastTimeBotCheck = 0;
long lastUpdateID = 0;

bool timeSynced = false;
unsigned long lastLedUpdate = 0;
bool authorizedOpen = false;
unsigned long authorizedTimer = 0;
const unsigned long AUTH_DOOR = 10000;

// Enrollment state machine
enum EnrollmentState {
    STATE_IDLE,
    STATE_WAITING_NAME,
    STATE_WAITING_FP_1,
    STATE_WAITING_FP_2,
    STATE_WAITING_RFID_CARD,
    STATE_ENROLL_SERIAL,
    STATE_WAITING_DELETE_ID
};
EnrollmentState currentEnrollState = STATE_IDLE;
uint8_t currentEnrollID = 0;
String currentEnrollName = "";
String currentAdminChatID = "";
String currentAdminChatID_Delete = "";

// Function prototypes (preserve)
void wifiConnect();
void setupTime();
String getTimeString();
String getTimestampKey();

void fbPUT(String path, String json);
void fbDELETE(String path);
void fbLog(uint8_t id, String name, String uid, String eventType);
String escapeJsonString(String s);

void initEEPROM();
void saveUserToEEPROM(uint8_t id, String name, String uid);
String readNameFromEEPROM(uint8_t id);
String readUIDFromEEPROM(uint8_t id);

void ledStandby();
void ledAccessGranted();
void ledAccessDenied();
void centerDisplay(String text, int y);
void updateDisplay(String line1, String line2, String line3);

void processCommand(const String &cmd);
void scanFingerprint();
void scanRFID();
void enrollFingerprint(uint8_t id);
void enrollRFID_Telegram(uint8_t id, String name, String uid, String chat_id);
void enrollRFID_Serial(uint8_t id, String name);
String listUsersTelegram();
void deleteUser(uint8_t id, bool viaTelegram);
int findEmptyID();
void enrollStepFP1();
void enrollStepFP2();

void handleNewMessages(int numNewMessages);
String getTelegramCommand(String text);
String getLogsFromFirebase(int limit);

void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnectNonBlocking();
void triggerAlarm();

// --- Optimizations / helpers ---
void shortDelay(unsigned long ms); // non-blocking-ish small wait that keeps services alive
void ensurePeriodicTasks(); // called often to keep MQTT/Telegram alive

// Variables for non-blocking reconnects
unsigned long lastMqttReconnectAttempt = 0;
//unsigned long lastWifiAttempt = 0;

// For scan scheduling
unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL_MS = 80; // scan interval for fingerprint/rfid

// For shortDelay-based bot checks
const unsigned long BOT_CHECK_INTERVAL = 1000; // as before, keep 1s for bot polling

// ----------------- SETUP -----------------
void setup() {
    Serial.begin(9600);
    while (!Serial) { shortDelay(10); } // not needed on ESP32, keep boot fast

    EEPROM.begin(EEPROM_SIZE);
    initEEPROM();

    pinMode(LED_OK, OUTPUT);
    pinMode(LED_FAIL, OUTPUT);
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_FAIL, LOW);

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Display
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED NOT DETECTED!");
    } else {
        ledStandby();
    }

    // Hardware init
    SPI.begin(18, 19, 23, SS_PIN);
    rfid.PCD_Init();
    shortDelay(50);

    mySerial.begin(57600, SERIAL_8N1, 16, 17);
    finger.begin(57600);

    if (!finger.verifyPassword()) {
        Serial.println("Fingerprint sensor NOT DETECTED!");
    } else {
        finger.getTemplateCount();
        Serial.print("Number of stored fingerprints: ");
        Serial.println(finger.templateCount);
    }

    // WiFi + Time
    wifiConnect();
    setupTime();

    // MQTT
    client.setInsecure();
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);

    // Initial led
    ledStandby();

    Serial.println("\n[ SYSTEM STARTED ]");
}

// ----------------- MAIN LOOP -----------------
void loop() {
    // Keep wifi attempt non-blocking
    wifiConnect();

    // Non-blocking MQTT reconnect attempt
    mqttReconnectNonBlocking();

    // Always service MQTT socket quickly
    if (mqttClient.connected()) mqttClient.loop();

    // Keep Telegram polling roughly every BOT_CHECK_INTERVAL
    if (WiFi.status() == WL_CONNECTED && millis() - lastTimeBotCheck >= BOT_CHECK_INTERVAL) {
        int numNewMessages = botAdmin.getUpdates(lastUpdateID + 1);
        while (numNewMessages) {
            handleNewMessages(numNewMessages);
            numNewMessages = botAdmin.getUpdates(lastUpdateID + 1);
        }

        // handleLogBot() and handleAdminBot() are kept but now lightweight:
        //handleAdminBot();
        handleLogBot();

        lastTimeBotCheck = millis();
    }

    // Scheduled sensor scans (non-blocking)
    if (millis() - lastScanTime >= SCAN_INTERVAL_MS) {
        lastScanTime = millis();
        if (currentEnrollState == STATE_IDLE) {
            scanFingerprint();
            scanRFID();
        } else {
            // If in enrollment, some steps are handled in enrollStepFP1/2 etc
            if (currentEnrollState == STATE_WAITING_FP_1) enrollStepFP1();
            else if (currentEnrollState == STATE_WAITING_FP_2) enrollStepFP2();
            else if (currentEnrollState == STATE_WAITING_RFID_CARD) {
                // non-blocking check for RFID during enroll
                if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
                    String uid = "";
                    for (byte i = 0; i < rfid.uid.size; i++) {
                        if (rfid.uid.uidByte[i] < 0x10) uid += "0";
                        uid += String(rfid.uid.uidByte[i], HEX);
                    }
                    uid.toUpperCase();
                    rfid.PICC_HaltA();
                    rfid.PCD_StopCrypto1();

                    enrollRFID_Telegram(currentEnrollID, currentEnrollName, uid, currentAdminChatID);
                    currentEnrollState = STATE_IDLE;
                    currentEnrollID = 0;
                    currentEnrollName = "";
                    currentAdminChatID = "";
                }
            }
        }
    }

    // Authorized open timeout (non-blocking)
    if (authorizedOpen && millis() - authorizedTimer > AUTH_DOOR) {
        authorizedOpen = false;
        Serial.println("[AUTH] Door returns to intrusion detection");
    }

    // Keep periodic small delay to yield CPU (short)
    shortDelay(5);
}

// ----------------- HELPERS -----------------

// shortDelay: tidak benar-benar sepenuhnya non-blocking, tetapi menjaga
// loop tetap melakukan mqttClient.loop() dan cek Telegram setiap BOT_CHECK_INTERVAL.
// Gunakan ini menggantikan delay() besar di tempat yang aman.
void shortDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        // layanan singkat: proses socket mqtt
        if (mqttClient.connected()) mqttClient.loop();

        // optionally allow other background yields
        yield();

        // Jangan panggil getUpdates di sini (bisa mem-block HTTP) --
        // Pengambilan pesan Telegram dilakukan setiap BOT_CHECK_INTERVAL di loop()
    }
}

// Non-blocking wifiConnect: coba connect tiap beberapa detik jika belum connected
void wifiConnect() {
    if (WiFi.status() == WL_CONNECTED) return;

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WiFi] Connecting");

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WiFi] Connected! IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WiFi] Failed (retry later)");
    }
}

void mqttReconnectNonBlocking() {
    if (mqttClient.connected()) return;

    if (millis() - lastMqttReconnectAttempt < 2000) return;
    lastMqttReconnectAttempt = millis();

    Serial.print("[MQTT] Connecting...");
    if (mqttClient.connect("Esp32_MainUnit")) {
        Serial.println("Connected!");
        mqttClient.subscribe(TOPIC_SWITCH);
        Serial.println("[MQTT] Subscribed: " TOPIC_SWITCH);
    } else {
        Serial.print("Failed rc=");
        Serial.println(mqttClient.state());
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    msg.trim();

    Serial.print("[MQTT] Incoming:");
    Serial.print(topic);
    Serial.print(" => ");
    Serial.println(msg);

    if (String(topic) == TOPIC_SWITCH) {
        lastSwitchStatus = msg;
        Serial.print("[DOOR] Status: ");
        Serial.println(msg);
        if (msg == "open") {
            if (authorizedOpen) {
                Serial.println("Open Door with legal access");
            } else {
                Serial.println("[WARNING!] Door opened without authentication");
                triggerAlarm();
            } 
        }
    }
}

// ensurePeriodicTasks: placeholder if need periodic housekeeping
void ensurePeriodicTasks() {
    // currently implemented in loop() using millis
}

// ----------------- ALARM -----------------
void triggerAlarm() {
    String alertMsg = "⚠️ *WARNING* ⚠️\nDoor opened without authentication";

    // Kirim ke botLog jika chat id ada
    if (String(CHAT_ID).length() > 0) {
        botLog.sendMessage(CHAT_ID, alertMsg, "Markdown");
    }

    // Bunyi buzzer secara non-blocking-ish menggunakan shortDelay
    for (int i = 0; i < 10; i++) {
        finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_RED, 0);
        digitalWrite(BUZZER_PIN, HIGH);
        shortDelay(800); // bunyi 800 ms (ganti sesuai kebutuhan)
        finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
        digitalWrite(BUZZER_PIN, LOW);
        shortDelay(200);
    }
}

// ----------------- TELEGRAM HANDLERS -----------------

String getTelegramCommand(String text) {
    if (text.startsWith("/")) {
        int spaceIndex = text.indexOf(' ');
        if (spaceIndex == -1) return text;
        return text.substring(0, spaceIndex);
    }
    return "";
}

// handleNewMessages: menerima numNewMessages dari getUpdates() dan memproses
void handleNewMessages(int numNewMessages) {
    Serial.print("[TELE] Receiving "); Serial.print(numNewMessages); Serial.println(" new message.");
    
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = botAdmin.messages[i].chat_id; 
        String text = botAdmin.messages[i].text;      
        String sender_name = botAdmin.messages[i].from_name;  

        if (chat_id != CHAT_ID) {
            botAdmin.sendMessage(chat_id, "❌ Sorry, you do not have admin permission to manage users.");
            continue;
        }
        
        String command = getTelegramCommand(text);

        // --- Enrollment state handling (non-blocking maintained) ---
        if (currentEnrollState != STATE_IDLE) {
            if (currentEnrollState == STATE_WAITING_NAME) {
                text.trim();
                currentEnrollName = text; 
                if (currentEnrollName.length() > 0) {
                    botAdmin.sendMessage(chat_id, "Name: *" + currentEnrollName + "*. ✅\n\nNow, **place your finger on the first step** on the fingerprint sensor (max 30 sec).", "Markdown");
                    updateDisplay("Enroll ID " + String(currentEnrollID), "Fingerprinting: the first step (1/2)", currentEnrollName);
                    currentEnrollState = STATE_WAITING_FP_1;
                } else {
                    botAdmin.sendMessage(chat_id, "❌ Invalid name, please try again.");
                }
            }
            else if (currentEnrollState == STATE_WAITING_DELETE_ID) {
                text.trim();
                int idToDelete = text.toInt();
                if (idToDelete > 0 && idToDelete <= MAX_ID) {
                    botAdmin.sendMessage(chat_id, "user ID deletion process *" + String(idToDelete) + "*...", "Markdown");
                    deleteUser((uint8_t)idToDelete, true);
                    currentEnrollState = STATE_IDLE;
                    currentAdminChatID_Delete = "";
                    ledStandby();
                } else {
                    botAdmin.sendMessage(chat_id, "❌ ID invalid (1-127). Enter the correct ID");
                }
            }
            continue;
        }
        if (command == "/start") {
            String welcome = "Welcome, " + sender_name + "!\n";
            welcome += "Use the following commands:\n";
            welcome += "/enroll - Start the new user registration process.\n";
            welcome += "/list   - View a list of all registered users.\n";
            welcome += "/delete - Delete users based on fingerprint ID(1-127).\n";
            welcome += "/log    - Get the last 5 access logs from Firebase\n"; 
            botAdmin.sendMessage(chat_id, welcome);
        }
        else if (command == "/enroll") {
            int id = findEmptyID();
            if (id <= 0) {
                botAdmin.sendMessage(chat_id, "❌ Storage is full (Max " + String(MAX_ID) + " ID).");
            } else {
                currentEnrollID = id;
                currentAdminChatID = chat_id;
                currentEnrollState = STATE_WAITING_NAME;
                botAdmin.sendMessage(chat_id, "📌 ID available: *" + String(id) + "*.\nSend your **Username**:", "Markdown");
                updateDisplay("Mode: Enroll", "ID " + String(id), "Send username via Bot");
            }
        } 
        else if (command == "/list") {
            String userList = listUsersTelegram();
            botAdmin.sendMessage(chat_id, userList);
        }
        else if (command == "/delete") {
            int sp = text.indexOf(' ');
            if (sp == -1) {
                botAdmin.sendMessage(chat_id, "Send the ID you want to delete (1-127)");
                currentEnrollState = STATE_WAITING_DELETE_ID;
                currentAdminChatID_Delete = chat_id;
                continue;
            }
            int id = text.substring(sp + 1).toInt();
            if (id <= 0 || id > MAX_ID) {
                botAdmin.sendMessage(chat_id, "❌ ID invalid (1-127).");
                continue;
            }
            deleteUser((uint8_t)id, true); 
        }
        else if (command == "/log") {
            botAdmin.sendMessage(chat_id, "*Retrieve the last 5 access logs from Firebase...*", "Markdown");
            String logs = getLogsFromFirebase(5);
            botAdmin.sendMessage(chat_id, logs, "Markdown");
        }
        else {
            botAdmin.sendMessage(chat_id, "Unknown command. Try /start.");
        }
    }

    if (numNewMessages > 0) {
        lastUpdateID = botAdmin.messages[numNewMessages - 1].update_id;
    }
}

// handleAdminBot kept for backwards compatibility but now lightweight
//void handleAdminBot() {
    // We already poll getUpdates in loop -> here we just allow compatibility if other code calls it
    // No heavy blocking code here.
//}

// handleLogBot kept simple
void handleLogBot() {
    int num = botLog.getUpdates(botLog.last_message_received + 1);

  for (int i = 0; i < num; i++) {
      String command = botLog.messages[i].text;
      String chat_id = botLog.messages[i].chat_id;

      if (command == "/log") {
          botLog.sendMessage(chat_id, "*Get the last 5 access logs...*", "Markdown");
          String logs = getLogsFromFirebase(5);
          botLog.sendMessage(chat_id, logs, "Markdown");
      } else {
          botLog.sendMessage(chat_id, "Unknown command. /log to view log activity.");
      }
  }
}

// ----------------- SCAN / AUTH FUNCTIONS -----------------

void scanFingerprint() {
    if (currentEnrollState != STATE_IDLE) return; 
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) return;
    if (p != FINGERPRINT_OK) return;
    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) return;
    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
        uint8_t id = finger.fingerID;
        String name = readNameFromEEPROM(id);
        String uid = readUIDFromEEPROM(id);
        
        Serial.print("[FP] Match! ID #");
        Serial.print(id);
        Serial.print(", Name: ");
        Serial.println(name);
        updateDisplay("Access Successful", "Welcome", name);
        digitalWrite(LED_OK, HIGH);
        ledAccessGranted();
        ledStandby();

        if (WiFi.status() == WL_CONNECTED) 
            fbLog(id, name, uid, "access_granted");
        else {
            Serial.println("[FB] Offline, skipping log.");
        }
    } else {
        Serial.println("[FP] Not found.");
        updateDisplay("Access Denied", "Fingerprint", "Not Registered");
        //digitalWrite(LED_FAIL, HIGH);
        ledAccessDenied();
        ledStandby();

        if (WiFi.status() == WL_CONNECTED) 
            fbLog(0, String("UNKNOWN"), String(""), String("access_denied"));
        else 
            Serial.println("[FB] Offline, skipping denied log.");
    }
}

void scanRFID() {
    // Re-init to help reliability
    rfid.PCD_Init();
    if (currentEnrollState != STATE_IDLE) return; 
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    for (uint8_t i = 1; i <= MAX_ID; i++) {
        if (readUIDFromEEPROM(i).equalsIgnoreCase(uid)) {
            String name = readNameFromEEPROM(i);
            
            Serial.print("[RFID] Match! ID #");
            Serial.print(i);
            Serial.print(", Name: ");
            Serial.println(name);
            updateDisplay("Access Succsessful", "Welcome", name);
            digitalWrite(LED_OK, HIGH);
            ledAccessGranted();
            ledStandby();
            
            if (WiFi.status() == WL_CONNECTED) 
                fbLog(i, name, uid, String("access_granted"));
            else {
                Serial.println("[FB] Offline, skipping log.");
            }
            return;
        }
    }

    Serial.print("[RFID] UID not found: "); Serial.println(uid);
    updateDisplay("Access Denied", "RFID tag", "Not Registered");
    digitalWrite(LED_FAIL, HIGH);
    ledAccessDenied();
    ledStandby();
    if (WiFi.status() == WL_CONNECTED) 
        fbLog(0, String("UNKNOWN"), uid, String("access_denied"));
    else 
        Serial.println("[FB] Offline, skipping denied log.");
}

// ----------------- ENROLL STEPS (non-blocking-ish using shortDelay) -----------------
void enrollStepFP1() {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(1);
        if (p == FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "First reading successfully captured. ✅\n\n**Release the finger** and **touch the same finger** again to confirm (max 30 sec).", "Markdown");
            updateDisplay("Enroll ID " + String(currentEnrollID), "Release & Touch", "Fingerprinting: the second step (2/2)");
            currentEnrollState = STATE_WAITING_FP_2;
        } else {
            botAdmin.sendMessage(currentAdminChatID, "❌ Failed to convert the first image. Repeat the process. `/enroll`.");
            currentEnrollState = STATE_IDLE;
            updateDisplay("Enrollment Failed", "re-enroll", "");
        }
    } else if (p == FINGERPRINT_NOFINGER) {
        // nothing
    } else {
        // do nothing special
    }
}

void enrollStepFP2() {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(2);
        if (p != FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "❌ Failed to convert the second image. Repeat the process. `/enroll`.");
            currentEnrollState = STATE_IDLE;
            return;
        }

        p = finger.createModel();
        if (p != FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "❌ Fingerprint not detected. Repeat the process. `/enroll`.");
            currentEnrollState = STATE_IDLE;
            return;
        }

        p = finger.storeModel(currentEnrollID);
        if (p == FINGERPRINT_OK) {
            rfid.PCD_Init(); 
            botAdmin.sendMessage(currentAdminChatID, "✅ Your fingerprint has been successfully saved! \n\nNow, **tap your RFID card** on the sensor. (max 30 sec).", "Markdown");
            updateDisplay("Enroll ID " + String(currentEnrollID), "Fingerprint successful", "tap RFID tag");
            currentEnrollState = STATE_WAITING_RFID_CARD;
        } else {
            botAdmin.sendMessage(currentAdminChatID, "❌ Failed to save fingerprint on sensor. Repeat the process. `/enroll`.");
            currentEnrollState = STATE_IDLE;
        }
    } else if (p == FINGERPRINT_NOFINGER) {
        // nothing
    } else {
        // nothing
    }
}

// ----------------- ENROLL HELPERS -----------------
void enrollRFID_Telegram(uint8_t id, String name, String uid, String chat_id) {
    for (uint8_t i = 1; i <= MAX_ID; i++) {
        if (readUIDFromEEPROM(i).equalsIgnoreCase(uid)) {
            botAdmin.sendMessage(chat_id, "❌ UID already registered at ID " + String(i) + ". Enrollment failed!");
            finger.deleteModel(id); 
            updateDisplay("Enrollment failed!", "UID duplicate", "");
            shortDelay(1500);
            return;
        }
    }

    saveUserToEEPROM(id, name, uid);

    String json = "{";
    json += "\"name\":\"" + escapeJsonString(name) + "\",";
    json += "\"uid\":\"" + uid + "\",";
    json += "\"time_registered\":\"" + getTimeString() + "\"";
    json += "}";
    fbPUT(String(NODE_USERS) + "/" + String(id), json);

    String msg = "✅ Enrollment Complete!\n";
    msg += "ID: *" + String(id) + "*\n";
    msg += "Name: *" + name + "*\n";
    msg += "UID RFID: *" + uid + "*";
    botAdmin.sendMessage(chat_id, msg, "Markdown");
    
    updateDisplay("Enrollment Succsess", "ID " + String(id), name);
    ledAccessGranted();
    ledStandby();
}

void enrollRFID_Serial(uint8_t id, String name) {
    Serial.println("\n[ENROLL RFID] Tap RFID tag...");
    unsigned long start = millis();
    while (millis() - start < 30000) {    
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            String uid = "";
            for (byte i = 0; i < rfid.uid.size; i++) {
                if (rfid.uid.uidByte[i] < 0x10) uid += "0";
                uid += String(rfid.uid.uidByte[i], HEX);
            }
            uid.toUpperCase();
            rfid.PICC_HaltA();
            rfid.PCD_StopCrypto1();

            for (uint8_t i = 1; i <= MAX_ID; i++) {
                if (readUIDFromEEPROM(i).equalsIgnoreCase(uid)) {
                    Serial.println("❌ UID registered!");
                    finger.deleteModel(id);
                    return;
                }
            }

            saveUserToEEPROM(id, name, uid);
            
            String json = "{";
            json += "\"name\":\"" + escapeJsonString(name) + "\",";
            json += "\"uid\":\"" + uid + "\",";
            json += "\"time_registered\":\"" + getTimeString() + "\"";
            json += "}";
            fbPUT(String(NODE_USERS) + "/" + String(id), json);

            Serial.println("✅ The RFID card was successfully saved.!");
            Serial.print("Name: ");
            Serial.println(name);
            Serial.print("UID: ");
            Serial.println(uid);

            ledAccessGranted();
            updateDisplay("Enrollmnet RFID Succsessful", "ID " + String(id), name);
            ledStandby();
            return;
        }
        shortDelay(50);
    }
    Serial.println("❌ Timeout. Card not recognized.");
    finger.deleteModel(id); 
    ledAccessDenied();
    updateDisplay("RFID Enrollment Failed", "Timeout", "");
    ledStandby();
}

// ----------------- COMMAND HANDLER -----------------
void processCommand(const String &cmd) {
    if (currentEnrollState != STATE_IDLE) {
        Serial.println("⚠️ Currently in the process of enrolling on Telegram, please complete it first.");
        return;
    }
    
    if (cmd == "enroll") {
        currentEnrollState = STATE_ENROLL_SERIAL; 
        int id = findEmptyID();
        if (id <= 0) {
            Serial.println("❌ Full storage capacity!");
            currentEnrollState = STATE_IDLE;
            return;
        }

        Serial.print("📌 ID available: ");
        Serial.println(id);
        Serial.print("Enter username: ");

        while (!Serial.available()) shortDelay(10);
        String nama = Serial.readStringUntil('\n');
        nama.trim();
        if (nama.length() == 0) {
            Serial.println("❌ Name cannot be empty!");
            currentEnrollState = STATE_IDLE;
            return;
        }

        enrollFingerprint((uint8_t)id);

        if (finger.loadModel(id) == FINGERPRINT_OK) {
            enrollRFID_Serial((uint8_t)id, nama);
        } else {
            Serial.println("❌ Fingerprint enrollment failed, canceled.");
            currentEnrollState = STATE_IDLE;
            return;
        }

        Serial.println("\nReturn to Scan Mode...\n");
        currentEnrollState = STATE_IDLE;
        return;
    }

    else if (cmd == "list") {
        Serial.println("\n" + listUsersTelegram());
        return;
    }

    else if (cmd == "delete") {
        Serial.println("Enter the ID you want to delete:");

        unsigned long startWait = millis();
        while (!Serial.available()) {
            if (millis() - startWait > 30000) {
                Serial.println("❌ Timeout. Delete mode canceled.");
                updateDisplay("Timeout", "Delete mode", "Canceled");
                return;
            }
            shortDelay(10);
        }
        
        String idStr = Serial.readStringUntil('\n');
        idStr.trim();
        int id = idStr.toInt();

        if (id <= 0 || id > MAX_ID) {
            Serial.println("❌ ID invalid (1-127). Canceled.");
            return;
        }
        deleteUser((uint8_t)id, false);
        Serial.println("\nReturn to Scan Mode...\n");
        return;
    }

    else {
        Serial.print("Unknown command: ");
        Serial.println(cmd);
        Serial.println("Recognized commands: : enroll, list, delete");
        return;
    }
}

// ----------------- FIREBASE -----------------
void fbPUT(String path, String json) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[FB] PUT skipped: WiFi not connected");
        return;
    }
    HTTPClient http;
    String url = String(FIREBASE_URL) + path + ".json?auth=" + FIREBASE_AUTH;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT(json);
    if (code == HTTP_CODE_OK || code == HTTP_CODE_NO_CONTENT) {
        // OK
    } else {
        Serial.print("[FB] PUT failed code: ");
        Serial.println(code);
    }
    http.end();
}

void fbDELETE(String path) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[FB] DELETE skipped: WiFi not connected");
        return;
    }
    HTTPClient http;
    String url = String(FIREBASE_URL) + path + ".json?auth=" + FIREBASE_AUTH;
    http.begin(url);
    int code = http.sendRequest("DELETE");
    if (code == HTTP_CODE_OK || code == HTTP_CODE_NO_CONTENT) {
        // ok
    } else {
        Serial.print("[FB] DELETE failed code: ");
        Serial.println(code);
    }
    http.end();
}

void fbLog(uint8_t id, String name, String uid, String eventType) {
    String key = getTimestampKey();
    String timeStr = getTimeString();
    String path = String(NODE_LOG) + "/" + key;

    String json = "{";
    json += "\"time\": \"" + timeStr + "\",";
    json += "\"id\": " + String(id) + ",";
    json += "\"name\": \"" + escapeJsonString(name) + "\",";
    json += "\"uid\": \"" + uid + "\",";
    json += "\"event\": \"" + eventType + "\"";
    json += "}";

    fbPUT(path, json);
    Serial.print("[FB] Log ");
    Serial.print(eventType);
    Serial.print(" -> ");
    Serial.println(key);

    if (WiFi.status() == WL_CONNECTED) {
        String logMsg;
        if (eventType == "access_granted") {
            logMsg = "*Access Succsessful*:\n";
            logMsg += "ID: `" + String(id) + "` \nName: *" + name + "*\n";
            logMsg += String("Metode: ") + (uid.length() > 0 ? "RFID" : "Fingerprint") + "\n";
        } else {
            logMsg = "*Access Denied!*\n";
            logMsg += "*There is invalid access.*.\n";
        }
        logMsg += "Time: " + timeStr;
        botLog.sendMessage(CHAT_ID, logMsg, "Markdown");
    }
}

String getLogsFromFirebase(int limit) {
    if (WiFi.status() != WL_CONNECTED) {
        return "❌ WiFi is not connected. Failed to retrieve logs.";
    }

    HTTPClient http;
    String url = String(FIREBASE_URL) + NODE_LOG + ".json?auth=" + FIREBASE_AUTH + "&orderBy=\"$key\"&limitToLast=" + String(limit);
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();

            if (payload == "null" || payload.length() <= 2) {
                http.end();
                return "📋 *Last Access Log (0)*:\n\nData log in Firebase was not found";
            }

            // Basic formatting (mirip kode lama)
            payload.remove(0, 1);
            payload.remove(payload.length() - 1);

            int start = 0;
            int counter = 0;
            String formattedLog = "📋 *Last Access Log (" + String(limit) + ")*:\n\n"; 

            while (start < payload.length() && counter < limit) {
                int keyStart = payload.indexOf("\"", start);
                if (keyStart == -1) break;
                int keyEnd = payload.indexOf("\":", keyStart + 1);
                if (keyEnd == -1) break;

                String fullKey = payload.substring(keyStart + 1, keyEnd);
                int dataStart = payload.indexOf("{", keyEnd);
                if (dataStart == -1) break;
                int dataEnd = payload.indexOf("}", dataStart);
                if (dataEnd == -1) break;

                String data = payload.substring(dataStart + 1, dataEnd);
                data.replace("\",\"", "\n");
                data.replace("\":", ": ");
                data.replace("\"", "");
                data.replace(",", "");
                data.replace("event:", "\nevent:");
                data.replace("name:", "\nname :");
                data.replace("access_granted", "access granted");
                data.replace("access_denied", "access denied");

                formattedLog += data + "\n=========================";
                start = dataEnd + 1;
                if (start < payload.length() && payload.charAt(start) == ',') start++;
                counter++;
            }
            
            http.end();
            return formattedLog;
        } else {
            http.end();
            return "❌ Failed to retrieve Firebase logs. HTTP Code: " + String(httpCode);
        }
    } else {
        http.end();
        return "❌ Failed to connect to Firebase Server.";
    }
}

// ----------------- EEPROM (minimal implementasi) -----------------
void initEEPROM() {
    // nothing else necessary for now; EEPROM.begin called in setup
}

void saveUserToEEPROM(uint8_t id, String name, String uid) {
    int addr = RECORD_START + (id - 1) * RECORD_SIZE;
    for (int i = 0; i < NAME_LENGTH; i++) {
        EEPROM.write(addr + i, (i < name.length()) ? name[i] : 0);
    }
    for (int i = 0; i < UID_LENGTH; i++) {
        EEPROM.write(addr + NAME_LENGTH + i, (i < uid.length()) ? uid[i] : 0);
    }
    EEPROM.commit();
}

String readNameFromEEPROM(uint8_t id) {
    int addr = RECORD_START + (id - 1) * RECORD_SIZE;
    String name = "";
    for (int i = 0; i < NAME_LENGTH; i++) {
        char c = EEPROM.read(addr + i);
        if (c == 0) break;
        name += c;
    }
    return name;
}

String readUIDFromEEPROM(uint8_t id) {
    int addr = RECORD_START + (id - 1) * RECORD_SIZE;
    String uid = "";
    for (int i = 0; i < UID_LENGTH; i++) {
        char c = EEPROM.read(addr + NAME_LENGTH + i);
        if (c == 0) break;
        uid += c;
    }
    return uid;
}

String escapeJsonString(String s) {
    s.replace("\"", "\\\"");
    return s;
}

// ----------------- LED & DISPLAY -----------------
void ledStandby() {
    // Use fingerprint LEDcontrol if available
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_BLUE);
    lastLedUpdate = millis();

    updateDisplay("Smart Lock Ready", "", "Scan Here");
}

void ledAccessGranted() {
    // briefly show green and publish open command
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_GREEN);
    shortDelay(1000); // keep brief

    digitalWrite(LED_OK, HIGH);
    shortDelay(1000);
    digitalWrite(LED_OK, LOW);

    ledStandby();

    Serial.println("[MQTT] Relay active");
    mqttClient.publish(TOPIC_SOLENOID, "on");
    authorizedOpen = true;
    authorizedTimer = millis();
    Serial.println("[AUTH] Access Succsessfully");
}

void ledAccessDenied() {
    //finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_RED, 3);
    //shortDelay(1000);

    for (int i = 0; i < 3; i++){
        finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_RED, 0);
        digitalWrite(BUZZER_PIN, HIGH);
        shortDelay(250);
        finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
        digitalWrite(BUZZER_PIN, LOW);
        shortDelay(250);
    }

    ledStandby();
}

void centerDisplay(String text, int y) {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
    int x = (SCREEN_WIDTH -w) / 2;
    display.setCursor(x, y);
    display.println(text);
}

void updateDisplay(String line1, String line2, String line3) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    centerDisplay(line1, 0);
    centerDisplay(line2, 20);
    centerDisplay(line3, 40);

    display.display();
}

// ----------------- UTILITIES -----------------
int findEmptyID() {
    for (int i = 1; i <= MAX_ID; i++) {
        if (readNameFromEEPROM(i).length() == 0 && finger.loadModel(i) != FINGERPRINT_OK) {
            return i;
        }
    }
    return 0;
}

// ----------------- TIME -----------------
void setupTime() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TIME] WiFi is not connected, delay sync...");
        return;
    }
    Serial.print("[TIME] Sinkron NTP WITA...");
    configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");
    for (int i = 0; i < 15; i++) {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            timeSynced = true;
            Serial.println(" OK ✅");
            return;
        }
        Serial.print(".");
        shortDelay(500);
    }
    timeSynced = false;
    Serial.println(" GAGAL ❌ (fallback aktif)");
}

String getTimeString() {
    if (!timeSynced) {
        return String("fallback_") + String(millis());
    }
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return String("fallback_") + String(millis());
    }
    char buf[40];
    strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M:%S", &timeinfo);
    String out = String(buf) + " WITA";
    return out;
}

String getTimestampKey() {
    if (!timeSynced) return String("fallback_") + String(millis());
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return String("fallback_") + String(millis());
    char buf[40];
    strftime(buf, sizeof(buf), "%d-%m-%Y|%H:%M:%S", &timeinfo);
    return String(buf);
}

// ----------------- USER LIST -----------------
String listUsersTelegram() {
    String list = "📋 *List of Registered Users*:\n\n";
    finger.getTemplateCount();
    int found = 0;
    for (int i = 1; i <= MAX_ID; i++) {
        if (finger.loadModel(i) == FINGERPRINT_OK) {
            String name = readNameFromEEPROM(i);
            String uid = readUIDFromEEPROM(i);
            list += "ID: *" + String(i) + "* | Name: *" + name + "* | UID: `" + uid + "`\n";
            found++;
        } else {
            String uid = readUIDFromEEPROM(i);
            if(uid.length() == 8 || uid.length() == 10) {
                String name = readNameFromEEPROM(i);
                list += "ID: *" + String(i) + "* | Name: *" + name + "* | UID: `" + uid + "` (Blank FP)\n";
                found++;
            }
        }
    }
    if (found == 0) return "No registered users.";
    return list + "\n*Total: " + String(found) + " user.*";
}

// ----------------- DELETE USER -----------------
void deleteUser(uint8_t id, bool viaTelegram) {
    String statusMsg = "";
    if (id < 1 || id > MAX_ID) {
        statusMsg = "ID tidak valid!";
    } 
    
    uint8_t fp_del_result = finger.deleteModel(id);
    saveUserToEEPROM(id, "", "");
    if (WiFi.status() == WL_CONNECTED) fbDELETE(String(NODE_USERS) + "/" + String(id));
    
    if (fp_del_result == FINGERPRINT_OK) {
        statusMsg = "✅ Data user ID *" + String(id) + "* (Fingerprint, EEPROM, Firebase) Successfully deleted.";
        updateDisplay("User ID " + String(id), "Successfully deleted", "");
        digitalWrite(LED_OK, HIGH);
        shortDelay(1000);
        digitalWrite(LED_OK, LOW);
    } else {
        statusMsg = "⚠️ Fingerprint ID *" + String(id) + "* Not registered in the sensor. EEPROM & Firebase data has been cleared..";
        updateDisplay("User ID " + String(id), "None Data", "Blank FP");
        digitalWrite(LED_FAIL, HIGH);
        shortDelay(1000);
        digitalWrite(LED_FAIL, LOW);
    }
    
    if (viaTelegram) {
        botAdmin.sendMessage(CHAT_ID, statusMsg, "Markdown");
    } else {
        Serial.println(statusMsg);
    }
}
