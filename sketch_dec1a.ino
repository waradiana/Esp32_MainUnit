/*
 * Gabungan Kode Enroll RFID dan Fingerprint untuk ESP32 dengan Telegram Bot
 * - SOLUSI UNTUK ERROR KOMPILASI UniversalTelegramBot dan String::trim()
 * - PERBAIKAN: Masalah RFID tidak membaca saat STATE_WAITING_RFID_CARD
 * - TAMBAHAN: Bot Notifikasi Terpisah (botLog)
 */

// ======================= LIBRARY & CONFIG =======================
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

// ---------------- LED CONTROL ----------------
// Note: ESP32 pins are usually referred to by their GPIO number, 
// using defines like 0x01 is often for direct registers or proprietary libraries, 
// assuming standard GPIO output control for LED_OK/LED_FAIL.
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
// GANTI DENGAN KREDENSIAL ANDA
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
// BOT 1: ADMIN & ENROLLMENT (Token Lama)
#define BOT_TOKEN_ADMIN "8419291391:AAFqxD-k1n9-TMleOZVKR1a2BXbFN0mMzCE"
// BOT 2: NOTIFIKASI & LOG (HARUS BOT BARU)
#define BOT_TOKEN_LOG "7893459259:AAGAwjOYaLDlPb6yiS51q9kmNX53cti5yHk" 
// CHAT ID ADMIN (ID PENGGUNA)
#define CHAT_ID "8385631359" 

WiFiClient espMqtt;
PubSubClient mqttClient(espMqtt);
String lastSwitchStatus = "";

// ---------------- Sensor & Display ----------------
MFRC522 rfid(SS_PIN, RST_PIN);
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- Telegram Object ----------------
WiFiClientSecure client;
// Bot Admin (Menggantikan 'bot')
UniversalTelegramBot botAdmin(BOT_TOKEN_ADMIN, client);
// Bot Notifikasi (BARU)
UniversalTelegramBot botLog(BOT_TOKEN_LOG, client);

unsigned long lastTimeBotCheck;
// PERBAIKAN 1: Deklarasi lastUpdateID secara global
long lastUpdateID = 0; 


// ---------------- Globals ----------------
String inputLine = "";
bool enrolling = false;
bool timeSynced = false;
unsigned long lastLedUpdate = 0;
bool authorizedOpen = false;
unsigned long authorizedTimer = 0;
const unsigned long AUTH_DOOR = 10000;

// ======================= STATE MACHINE ENROLLMENT =======================
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

// ======================= FUNCTION PROTOTYPES =======================
// [WiFi & Time]
void wifiConnect();
void setupTime();
String getTimeString();
String getTimestampKey();

// [Firebase]
void fbPUT(String path, String json);
void fbDELETE(String path);
void fbLog(uint8_t id, String name, String uid, String eventType);
String escapeJsonString(String s);

// [EEPROM]
void initEEPROM();
void saveUserToEEPROM(uint8_t id, String name, String uid);
String readNameFromEEPROM(uint8_t id);
String readUIDFromEEPROM(uint8_t id);

// [LED & Display]
void ledStandby();
void ledAccessGranted();
void ledAccessDenied();
void updateDisplay(String line1, String line2, String line3);

// [Core Logic]
void processCommand(const String &cmd);
void scanFingerprint();
void scanRFID();
void enrollFingerprint(uint8_t id);
// PERBAIKAN 4: Ubah nama fungsi untuk mengatasi redefinition
void enrollRFID_Telegram(uint8_t id, String name, String uid, String chat_id);
void enrollRFID_Serial(uint8_t id, String name); 
String listUsersTelegram(); 
void deleteUser(uint8_t id, bool viaTelegram);
int findEmptyID();
void enrollStepFP1();
void enrollStepFP2();

// [Telegram Bot]
void handleNewMessages(int numNewMessages);
String getTelegramCommand(String text);

// Fungsi baru untuk mendapatkan data log dari Firebase 
String getLogsFromFirebase(int limit);

// [MQTT]
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();

// [Alarm Buzzer]
void triggerAlarm();


// =======================================================
//                       SETUP
// =======================================================
void setup() {
    Serial.begin(9600);
    while (!Serial) {
        delay(10); 
    }
    EEPROM.begin(EEPROM_SIZE);
    initEEPROM();
    wifiConnect();
    setupTime();
    client.setInsecure();
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);

    // RFID
    SPI.begin(18, 19, 23, SS_PIN);
    rfid.PCD_Init();
    delay(10);
    
    // Fingerprint
    mySerial.begin(57600, SERIAL_8N1, 16, 17);
    finger.begin(57600);

    Serial.println("\n[ SYSTEM STARTED ]");
    if (!finger.verifyPassword()) {
        Serial.println("Sensor fingerprint TIDAK TERDETEKSI!");
    } else {
        finger.getTemplateCount();
        Serial.print("Jumlah sidik jari tersimpan (sensor): ");
        Serial.println(finger.templateCount);
    }

    pinMode(LED_OK, OUTPUT);
    pinMode(LED_FAIL, OUTPUT);
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_FAIL, LOW);

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED tidak terdeteksi!");
    } else {
        updateDisplay("Smart Lock Ready", "", "Mode: SCAN");
    }

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    ledStandby();
}

// =======================================================
//                       LOOP
// =======================================================
void loop() {
    // --- 1. Serial Command Handler ---
    if (Serial.available()) {
        inputLine = Serial.readStringUntil('\n');
        inputLine.trim();
        inputLine.toLowerCase();
        if (inputLine.length() > 0) processCommand(inputLine);
    }

    // --- 2. Scanning Mode (Non-Blocking) ---
    if (currentEnrollState == STATE_IDLE) {
        scanFingerprint();
        scanRFID();
    }
    
    // --- 3. Telegram Enrollment State Machine (Non-Blocking) ---
    if (currentEnrollState == STATE_WAITING_FP_1) {
        enrollStepFP1();
    } else if (currentEnrollState == STATE_WAITING_FP_2) {
        enrollStepFP2();
    } else if (currentEnrollState == STATE_WAITING_RFID_CARD) {
        // Logic non-blocking untuk membaca kartu RFID
        if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
            // Jika kartu belum terdeteksi, kembali (non-blocking)
            delay(10); 
            return;
        }
        
        // Kartu Terdeteksi, Lanjutkan Proses:
        String uid = "";
        for (byte i = 0; i < rfid.uid.size; i++) {
            if (rfid.uid.uidByte[i] < 0x10) uid += "0";
            uid += String(rfid.uid.uidByte[i], HEX);
        }
        uid.toUpperCase();
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();

        // Lanjutkan proses enroll RFID dengan data dari sensor
        enrollRFID_Telegram(currentEnrollID, currentEnrollName, uid, currentAdminChatID);
        currentEnrollState = STATE_IDLE; // Kembali ke mode scan
        currentEnrollID = 0;
        currentEnrollName = "";
        currentAdminChatID = "";
    }

    // --- 4. Telegram Bot Handler ---
    if (WiFi.status() == WL_CONNECTED) {
        if (millis() > lastTimeBotCheck + 1000) { // Cek pesan setiap 1 detik
            // Menggunakan botAdmin
            int numNewMessages = botAdmin.getUpdates(lastUpdateID + 1); 
            while(numNewMessages) {
                handleNewMessages(numNewMessages);
                // Menggunakan botAdmin
                numNewMessages = botAdmin.getUpdates(lastUpdateID + 1);
            }
            lastTimeBotCheck = millis();
        }
    }

    // --- 5. WiFi & Time Maintenance ---
    static unsigned long lastWifiTry = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 15000) {
        lastWifiTry = millis();
        wifiConnect();
        if (WiFi.status() == WL_CONNECTED && !timeSynced) setupTime();
    }
    static unsigned long lastTimeTry = 0;
    if (!timeSynced && WiFi.status() == WL_CONNECTED && millis() - lastTimeTry > 30000) {
        lastTimeTry = millis();
        setupTime();
    }

    if (!mqttClient.connected()) mqttReconnect();
    mqttClient.loop();

    if(authorizedOpen && millis() - authorizedTimer > AUTH_DOOR) {
        authorizedOpen = false;
        Serial.println("[AUTH] Pintu kembali ke deteksi pembobolan");
    }

    delay(10); 

    handleAdminBot();
    handleLogBot();
}

// =======================================================
//                       COMMAND HANDLER
// =======================================================
void processCommand(const String &cmd) {
    if (currentEnrollState != STATE_IDLE) {
        Serial.println("⚠️ Sedang dalam proses enrollment Telegram, batalkan/selesaikan dulu.");
        return;
    }
    
    if (cmd == "daftar") {
        currentEnrollState = STATE_ENROLL_SERIAL; 
        int id = findEmptyID();
        if (id <= 0) {
            Serial.println("❌ Penyimpanan penuh!");
            currentEnrollState = STATE_IDLE;
            return;
        }

        Serial.print("📌 ID tersedia: ");
        Serial.println(id);
        Serial.print("Masukkan nama: ");

        while (!Serial.available()) delay(10);
        String nama = Serial.readStringUntil('\n');
        nama.trim();
        if (nama.length() == 0) {
            Serial.println("❌ Nama tidak boleh kosong!");
            currentEnrollState = STATE_IDLE;
            return;
        }

        enrollFingerprint((uint8_t)id);

        // Setelah enrollFingerprint (yang blocking), kita cek apakah berhasil
        if (finger.loadModel(id) == FINGERPRINT_OK) {
            // PERBAIKAN 4: Panggil fungsi untuk Serial
            enrollRFID_Serial((uint8_t)id, nama);
        } else {
            Serial.println("❌ Enroll fingerprint gagal, batalkan.");
            currentEnrollState = STATE_IDLE;
            return;
        }

        Serial.println("\nKembali ke MODE SCAN...\n");
        currentEnrollState = STATE_IDLE;
        return;
    }

    else if (cmd == "list") {
        Serial.println("\n" + listUsersTelegram());
        return;
    }

    else if (cmd == "hapus") { // Ubah untuk menerima hanya "hapus"
        Serial.println("Masukan ID yang ingin anda hapus:");

        // Tunggu input ID dari Serial
        unsigned long startWait = millis();
        while (!Serial.available()) {
            if (millis() - startWait > 30000) { // Timeout 30 detik
                Serial.println("❌ Timeout. Batalkan mode hapus.");
                updateDisplay("Timeout", "Mode Hapus", "Dibatalkan");
                return;
            }
            delay(10);
        }
        
        String idStr = Serial.readStringUntil('\n');
        idStr.trim();
        int id = idStr.toInt();

        if (id <= 0 || id > MAX_ID) {
            Serial.println("❌ ID tidak valid (1-127). Batalkan.");
            return;
        }
        deleteUser((uint8_t)id, false);
        Serial.println("\nKembali ke MODE SCAN...\n");
        return;
    }

    else {
        Serial.print("Perintah tidak dikenal: ");
        Serial.println(cmd);
        Serial.println("Perintah yang dikenali: daftar, list, hapus <id>");
        return;
    }
}

// =======================================================
//                       TELEGRAM HANDLER
// =======================================================
String getTelegramCommand(String text) {
    if (text.startsWith("/")) {
        int spaceIndex = text.indexOf(' ');
        if (spaceIndex == -1) return text;
        return text.substring(0, spaceIndex);
    }
    return "";
}

void handleNewMessages(int numNewMessages) {
    Serial.print("[TELE] Menerima "); Serial.print(numNewMessages); Serial.println(" pesan baru.");
    
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = botAdmin.messages[i].chat_id; // Menggunakan botAdmin
        String text = botAdmin.messages[i].text;      // Menggunakan botAdmin
        String sender_name = botAdmin.messages[i].from_name;  // Menggunakan botAdmin
        
        if (chat_id != CHAT_ID) {
            botAdmin.sendMessage(chat_id, "❌ Maaf, Anda tidak memiliki izin admin untuk mengelola user.");
            continue;
        }
        
        String command = getTelegramCommand(text);

        // --- 1. Penanganan Enrollment State ---
        if (currentEnrollState != STATE_IDLE) {
            if (currentEnrollState == STATE_WAITING_NAME) {
                // PERBAIKAN 3: Panggil trim() dulu, lalu salin
                text.trim();
                currentEnrollName = text; 
                
                if (currentEnrollName.length() > 0) {
                    botAdmin.sendMessage(chat_id, "Nama: *" + currentEnrollName + "*. ✅\n\nSekarang, **tempelkan jari pertama** pada sensor fingerprint (max 30 detik).", "Markdown");
                    updateDisplay("Enroll ID " + String(currentEnrollID), "Tempel Jari (1/2)", currentEnrollName);
                    currentEnrollState = STATE_WAITING_FP_1;
                } else {
                    botAdmin.sendMessage(chat_id, "❌ Nama tidak valid, ulangi.");
                }
            } else if (currentEnrollState == STATE_WAITING_DELETE_ID) { // <-- PENANGANAN STATE DELETE BARU
                text.trim();
                int idToDelete = text.toInt();
                
                if (idToDelete > 0 && idToDelete <= MAX_ID) {
                    botAdmin.sendMessage(chat_id, "Memproses penghapusan user ID *" + String(idToDelete) + "*...", "Markdown");
                    deleteUser((uint8_t)idToDelete, true);
                    currentEnrollState = STATE_IDLE; // Kembali ke IDLE
                    currentAdminChatID_Delete = "";
                    updateDisplay("Smart Lock Ready", "", "Mode: SCAN");
                } else {
                    botAdmin.sendMessage(chat_id, "❌ ID tidak valid (1-127). Masukkan ID yang benar, atau ketik /batal.");
                }
            }
            continue;
        }

        // --- 2. Penanganan Perintah Utama (IDLE State) ---
        if (command == "/start") {
            String welcome = "Selamat datang, " + sender_name + "!\n";
            welcome += "Gunakan perintah berikut:\n";
            welcome += "/daftar - Mulai proses pendaftaran user baru.\n";
            welcome += "/list - Lihat daftar semua user yang terdaftar.\n";
            welcome += "/hapus <id> - Hapus user berdasarkan ID sidik jari (1-127).\n";
            // TAMBAHAN: Menu Log
            welcome += "/log - Dapatkan 5 log akses terakhir dari Firebase.\n"; 
            botAdmin.sendMessage(chat_id, welcome);
        } else if (command == "/daftar") {
            int id = findEmptyID();
            if (id <= 0) {
                botAdmin.sendMessage(chat_id, "❌ Penyimpanan penuh (Max " + String(MAX_ID) + " ID).");
            } else {
                currentEnrollID = id;
                currentAdminChatID = chat_id;
                currentEnrollState = STATE_WAITING_NAME;
                botAdmin.sendMessage(chat_id, "📌 ID tersedia: *" + String(id) + "*. Kirimkan **Nama Lengkap** user:", "Markdown");
                updateDisplay("Mode: Enroll", "ID " + String(id), "Kirim Nama via Bot");
            }
        } else if (command == "/list") {
            String userList = listUsersTelegram();
            botAdmin.sendMessage(chat_id, userList);
        } else if (command == "/hapus") {
            int sp = text.indexOf(' ');
            if (sp == -1) {
                botAdmin.sendMessage(chat_id, "Masukan ID yang ingin anda hapus (1-127)");
                currentEnrollState = STATE_WAITING_DELETE_ID;
                currentAdminChatID_Delete = chat_id;
                continue;
            }
            int id = text.substring(sp + 1).toInt();
            if (id <= 0 || id > MAX_ID) {
                botAdmin.sendMessage(chat_id, "❌ ID tidak valid (1-127).");
                continue;
            }
            
            deleteUser((uint8_t)id, true); 
        } else {
            botAdmin.sendMessage(chat_id, "Perintah tidak dikenal. Coba /start");
        }
    }
    
    // PERBAIKAN 5: Update lastUpdateID setelah semua pesan diproses
    if (numNewMessages > 0) {
        lastUpdateID = botAdmin.messages[numNewMessages - 1].update_id; 
    }
}

// === Handle perintah dari BOT ADMIN ===
void handleAdminBot() {
  int num = botAdmin.getUpdates(botAdmin.last_message_received + 1);

  for (int i = 0; i < num; i++) {
      String command = botAdmin.messages[i].text;
      String chat_id = botAdmin.messages[i].chat_id;

      if (command == "/log") {
          botAdmin.sendMessage(chat_id, "*Mengambil log 5 akses terakhir...*", "Markdown");
          String logs = getLogsFromFirebase(5);
          botAdmin.sendMessage(chat_id, logs, "Markdown");
      } else {
          botAdmin.sendMessage(chat_id, "Perintah tidak dikenal. Coba /start");
      }
  }
}

// === Handle perintah dari BOT LOG ===
void handleLogBot() {
  int num = botLog.getUpdates(botLog.last_message_received + 1);

  for (int i = 0; i < num; i++) {
      String command = botLog.messages[i].text;
      String chat_id = botLog.messages[i].chat_id;

      if (command == "/log") {
          botLog.sendMessage(chat_id, "*Mengambil log 5 akses terakhir...*", "Markdown");
          String logs = getLogsFromFirebase(5);
          botLog.sendMessage(chat_id, logs, "Markdown");
      } else {
          botLog.sendMessage(chat_id, "Perintah tidak dikenal. /log untuk menampilkan aktivitas log.");
      }
  }
}

// =======================================================
//                       ENROLL STEP FINGERPRINT (NON-BLOCKING)
// =======================================================
void enrollStepFP1() {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(1);
        if (p == FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "Gambar pertama terambil. ✅\n\n**Lepaskan jari** dan **tempelkan jari yang sama** lagi untuk konfirmasi (max 30 detik).", "Markdown");
            updateDisplay("Enroll ID " + String(currentEnrollID), "Lepas & Tempel", "Jari (2/2)");
            currentEnrollState = STATE_WAITING_FP_2;
        } else {
            botAdmin.sendMessage(currentAdminChatID, "❌ Gagal convert gambar pertama. Ulangi proses `/daftar`.");
            currentEnrollState = STATE_IDLE;
            updateDisplay("X Enroll Gagal X", "Ulangi Daftar", "");
        }
    } else if (p == FINGERPRINT_NOFINGER) {
        delay(10);
    } else {
        delay(10);
    }
}

void enrollStepFP2() {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(2);
        if (p != FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "❌ Gagal convert gambar kedua. Ulangi proses `/daftar`.");
            currentEnrollState = STATE_IDLE;
            return;
        }

        p = finger.createModel();
        if (p != FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "❌ Sidik jari TIDAK cocok. Ulangi proses `/daftar`.");
            currentEnrollState = STATE_IDLE;
            return;
        }

        p = finger.storeModel(currentEnrollID);
        if (p == FINGERPRINT_OK) {
            // 🚨 PERBAIKAN UNTUK MASALAH RFID TIDAK MEMBACA:
            // Inisialisasi ulang modul RFID untuk memastikan buffer SPI bersih dan siap.
            rfid.PCD_Init(); 
            // ---------------------------------------------
            
            botAdmin.sendMessage(currentAdminChatID, "✅ Sidik jari berhasil disimpan! \n\nSekarang, **tempelkan kartu RFID** ke sensor (max 30 detik).", "Markdown");
            updateDisplay("Enroll ID " + String(currentEnrollID), "FP OK", "Tempel Kartu RFID");
            currentEnrollState = STATE_WAITING_RFID_CARD;
        } else {
            botAdmin.sendMessage(currentAdminChatID, "❌ Gagal menyimpan sidik jari di sensor. Ulangi proses `/daftar`.");
            currentEnrollState = STATE_IDLE;
        }
    } else if (p == FINGERPRINT_NOFINGER) {
        delay(10);
    } else {
        delay(10);
    }
}


// =======================================================
//                       ENROLL & DELETE UTILITY
// =======================================================

// PERBAIKAN 4: Fungsi untuk Telegram
void enrollRFID_Telegram(uint8_t id, String name, String uid, String chat_id) {
    // Cek duplikasi UID
    for (uint8_t i = 1; i <= MAX_ID; i++) {
        if (readUIDFromEEPROM(i).equalsIgnoreCase(uid)) {
            botAdmin.sendMessage(chat_id, "❌ UID sudah terdaftar di ID " + String(i) + ". Enroll gagal!");
            finger.deleteModel(id); 
            updateDisplay("X Enroll Gagal X", "UID Duplikat", "");
            delay(1000);
            return;
        }
    }

    // Simpan ke EEPROM
    saveUserToEEPROM(id, name, uid);

    // Push ke Firebase
    String json = "{";
    json += "\"name\":\"" + escapeJsonString(name) + "\",";
    json += "\"uid\":\"" + uid + "\",";
    json += "\"time_registered\":\"" + getTimeString() + "\"";
    json += "}";
    fbPUT(String(NODE_USERS) + "/" + String(id), json);

    String msg = "✅ Pendaftaran Selesai!\n";
    msg += "ID: *" + String(id) + "*\n";
    msg += "Nama: *" + name + "*\n";
    msg += "UID RFID: *" + uid + "*";
    botAdmin.sendMessage(chat_id, msg, "Markdown");
    
    updateDisplay("Enroll Sukses", "ID " + String(id), name);
    //digitalWrite(LED_OK, HIGH);
    ledAccessGranted();
    //digitalWrite(LED_OK, LOW);
}

// PERBAIKAN 4: Fungsi untuk Serial Monitor
void enrollRFID_Serial(uint8_t id, String name) {
    Serial.println("\n[ENROLL RFID] Tempelkan kartu RFID...");
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
                    Serial.println("❌ UID sudah terdaftar!");
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

            Serial.println("✅ Kartu RFID berhasil disimpan!");
            Serial.print("Nama: ");
            Serial.println(name);
            Serial.print("UID: ");
            Serial.println(uid);

            //digitalWrite(LED_OK, HIGH);
            ledAccessGranted();
            updateDisplay("Enroll RFID Berhasil", "ID " + String(id), name);
            delay(1000);
            //digitalWrite(LED_OK, LOW);
            return;
        }
        delay(10);
    }
    Serial.println("❌ Timeout. Kartu tidak terbaca.");
    finger.deleteModel(id); 
    //digitalWrite(LED_FAIL, HIGH);
    ledAccessDenied();
    updateDisplay("X Enroll RFID Gagal X", "Timeout", "");
    delay(1000);
    //digitalWrite(LED_FAIL, LOW);
}


String listUsersTelegram() {
    String list = "📋 *Daftar User Terdaftar*:\n\n";
    finger.getTemplateCount();
    int found = 0;
    for (int i = 1; i <= MAX_ID; i++) {
        // Cek sidik jari
        if (finger.loadModel(i) == FINGERPRINT_OK) {
            String name = readNameFromEEPROM(i);
            String uid = readUIDFromEEPROM(i);
            
            list += "ID: *" + String(i) + "* | Nama: *" + name + "* | UID: `" + uid + "`\n";
            found++;
        }
        // Cek hanya RFID (jika ada UID tapi tidak ada FP)
        else {
            String uid = readUIDFromEEPROM(i);
            if(uid.length() == 8 || uid.length() == 10) { // Cek panjang UID yang valid
                String name = readNameFromEEPROM(i);
                list += "ID: *" + String(i) + "* | Nama: *" + name + "* | UID: `" + uid + "` (FP Kosong)\n";
                found++;
            }
        }
    }
    
    if (found == 0) {
        return "Tidak ada user terdaftar.";
    }
    
    return list + "\n*Total: " + String(found) + " user.*";
}


void deleteUser(uint8_t id, bool viaTelegram) {
    String statusMsg = "";

    if (id < 1 || id > MAX_ID) {
        statusMsg = "ID tidak valid!";
    } 
    
    // Hapus dari sensor
    uint8_t fp_del_result = finger.deleteModel(id);
    
    // Hapus dari EEPROM
    saveUserToEEPROM(id, "", ""); 
    
    // Hapus dari Firebase
    if (WiFi.status() == WL_CONNECTED) fbDELETE(String(NODE_USERS) + "/" + String(id));
    
    if (fp_del_result == FINGERPRINT_OK) {
        statusMsg = "✅ Data user ID *" + String(id) + "* (Fingerprint, EEPROM, Firebase) berhasil dihapus.";
        
        updateDisplay("User ID " + String(id), "Berhasil Dihapus", "");
        digitalWrite(LED_OK, HIGH);
        delay(1000);
        digitalWrite(LED_OK, LOW);
    } 
    
    else {
        // Asumsi kegagalan hapus FP karena slot kosong/tidak ada data
        statusMsg = "⚠️ Fingerprint ID *" + String(id) + "* tidak terdaftar di sensor. Data EEPROM & Firebase telah dibersihkan.";
        
        updateDisplay("User ID " + String(id), "Data Bersih", "FP Kosong");
        
        for (int i = 0; i < 5; i++) {
            digitalWrite(LED_FAIL, HIGH);
            delay(200);
            digitalWrite(LED_FAIL, LOW);
            delay(200);
        }
    }
    
    if (viaTelegram) {
        botAdmin.sendMessage(CHAT_ID, statusMsg, "Markdown");
    } else {
        Serial.println(statusMsg);
    }
}


// =======================================================
//                       UTILITY & ORIGINAL FUNCTIONS
// =======================================================

// --- WIFI, TIME, FIREBASE, EEPROM (DARI KODE ASLI) ---

void wifiConnect() {
    if (WiFi.status() == WL_CONNECTED) return;
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("\n[WiFi] Connecting");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("\n[WiFi] Connected! IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n[WiFi] Failed to connect (will retry later).");
    }
}

void setupTime() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TIME] WiFi belum tersambung, tunda sync...");
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
        delay(500);
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
    if (!timeSynced) {
    return String("fallback_") + String(millis());
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return String("fallback_") + String(millis());
  }
  char buf[40];
  strftime(buf, sizeof(buf), "%d-%m-%Y|%H:%M:%S", &timeinfo);
  return String(buf);
}

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
        // ok
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
    json += "\"name\": \"" + name + "\",";
    json += "\"uid\": \"" + uid + "\",";
    json += "\"event\": \"" + eventType + "\"";
    json += "}";

    fbPUT(path, json);
    Serial.print("[FB] Log ");
    Serial.print(eventType);
    Serial.print(" -> ");
    Serial.println(key);

    // --- TAMBAHAN: KIRIM NOTIFIKASI VIA BOT LOG ---
    if (WiFi.status() == WL_CONNECTED) {
        String logMsg;
        if (eventType == "access_granted") {
            logMsg = "*Akses Diterima*:\n";
            logMsg += "ID: `" + String(id) + "` \nNama: *" + name + "*\n";
            logMsg += String("Metode: ") + (uid.length() > 0 ? "RFID" : "Fingerprint") + "\n";
        } else {
            logMsg = "*AKSES DITOLAK!*\n";
            logMsg += "*Ada Percobaan Akses yang Tidak Sah*.\n";
        }
        logMsg += "Waktu: " + timeStr;
        
        botLog.sendMessage(CHAT_ID, logMsg, "Markdown");
    }
    // ---------------------------------------------
}

String getLogsFromFirebase(int limit) {
    if (WiFi.status() != WL_CONNECTED) {
        return "❌ WiFi tidak terhubung. Gagal mengambil log.";
    }

    HTTPClient http;
    // Menggunakan query order by key (timestamp) descending dan limit
    String url = String(FIREBASE_URL) + NODE_LOG + ".json?auth=" + FIREBASE_AUTH + "&orderBy=\"$key\"&limitToLast=" + String(limit);
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();

            if (payload == "null" || payload.length() <= 2) {
                http.end();
                return "📋 *Log Akses Terakhir (0)*:\n\nTidak ada data log di Firebase.";
            }

            // Hapus kurung kurawal luar
            payload.remove(0, 1);
            payload.remove(payload.length() - 1);

            int start = 0;
            int counter = 0;
            String formattedLog = "📋 *Log Akses Terakhir (" + String(limit) + ")*:\n\n"; 

            while (start < payload.length() && counter < limit) {
                // 1. Cari Key (timestamp) dan hilangkan tanda kutip: "2025-12-01_10-00-00-500":
                int keyStart = payload.indexOf("\"", start);
                if (keyStart == -1) break;
                int keyEnd = payload.indexOf("\":", keyStart + 1);
                if (keyEnd == -1) break;

                // FIX 1: Deklarasi dan inisialisasi fullKey
                String fullKey = payload.substring(keyStart + 1, keyEnd);
                
                // 2. Cari Data Object: { ... }
                int dataStart = payload.indexOf("{", keyEnd);
                if (dataStart == -1) break;
                int dataEnd = payload.indexOf("}", dataStart);
                if (dataEnd == -1) break;
                
                String data = payload.substring(dataStart + 1, dataEnd);
                
                // 3. Format data menjadi mudah dibaca
                data.replace("\",\"", "\n");
                data.replace("\":", ": ");
                data.replace("\"", "");
                data.replace(",", "");
                data.replace("event:", "\nevent:");
                data.replace("name:", "\nname :");
                data.replace("access_granted", "access granted");
                data.replace("access_denied", "access denied");


                formattedLog += data + "\n=========================";
                
                // Pindah ke posisi setelah blok data saat ini
                start = dataEnd + 1;
                if (start < payload.length() && payload.charAt(start) == ',') {
                    start++; // Lewati koma pemisah jika ada
                }
                
                counter++;
            }
            
            http.end();
            return formattedLog;
        } else {
            http.end();
            return "❌ Gagal mengambil log Firebase. HTTP Code: " + String(httpCode);
        }
    } else {
        http.end();
        return "❌ Gagal terhubung ke Firebase Server.";
    }
}

// --- EEPROM FUNCTIONS (DARI KODE ASLI) ---

void initEEPROM() {
    // Implementasi inisialisasi EEPROM (tidak perlu diulang)
    // ...
}

void saveUserToEEPROM(uint8_t id, String name, String uid) {
    // Implementasi simpan ke EEPROM (tidak perlu diulang)
    int addr = RECORD_START + (id - 1) * RECORD_SIZE;
    
    // Simpan Nama
    for (int i = 0; i < NAME_LENGTH; i++) {
        EEPROM.write(addr + i, (i < name.length()) ? name[i] : 0);
    }

    // Simpan UID
    for (int i = 0; i < UID_LENGTH; i++) {
        EEPROM.write(addr + NAME_LENGTH + i, (i < uid.length()) ? uid[i] : 0);
    }
    
    EEPROM.commit();
}

String readNameFromEEPROM(uint8_t id) {
    // Implementasi baca Nama dari EEPROM (tidak perlu diulang)
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
    // Implementasi baca UID dari EEPROM (tidak perlu diulang)
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

// --- LED & DISPLAY FUNCTIONS (DARI KODE ASLI) ---

void ledStandby() {
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_BLUE);
    lastLedUpdate = millis();
    //digitalWrite(LED_OK, LOW);
    //digitalWrite(LED_FAIL, LOW);
}

void ledAccessGranted() {
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_GREEN);
    delay(1500);
    ledStandby();
    digitalWrite(LED_OK, HIGH);
    delay(1000);
    digitalWrite(LED_OK, LOW);
    Serial.println("[MQTT] Relay aktif");
    mqttClient.publish(TOPIC_SOLENOID, "on");
    authorizedOpen = true;
    authorizedTimer = millis();
    Serial.println("[AUTH] Akses berhasil");
}

void ledAccessDenied() {
    finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_RED, 10);
    delay(1500);

    for (int i = 0; i < 5; i++){
        digitalWrite(LED_FAIL, HIGH);
        delay(25);
        digitalWrite(LED_FAIL, LOW);
        delay(25);
    }

    ledStandby();
}

void updateDisplay(String line1, String line2, String line3) {
    // Implementasi updateDisplay (tidak perlu diulang)
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(line1);
    display.setCursor(0, 20);
    display.println(line2);
    display.setCursor(0, 40);
    display.println(line3);
    display.display();
}

int findEmptyID() {
    // Implementasi findEmptyID (tidak perlu diulang)
    for (int i = 1; i <= MAX_ID; i++) {
        if (readNameFromEEPROM(i).length() == 0 && finger.loadModel(i) != FINGERPRINT_OK) {
            return i;
        }
    }
    return 0; // Penuh
}

// --- SCAN MODE (MODIFIKASI) ---

void scanFingerprint() {
    // Hentikan scanning jika sedang enroll
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
        updateDisplay("Akses Diterima", name, "ID: " + String(id));
        //digitalWrite(LED_OK, HIGH);
        ledAccessGranted();

        // Memanggil fbLog yang kini juga mengirim notifikasi via botLog
        if (WiFi.status() == WL_CONNECTED)
            fbLog(id, name, uid, "access_granted");
        else {
            Serial.println("[FB] Offline, skipping log.");
        }
    } else {
        Serial.println("[FP] Not found.");
        updateDisplay("AKSES DITOLAK", "Fingerprint", "Tidak Terdaftar");
        //digitalWrite(LED_FAIL, HIGH);
        ledAccessDenied();

        // Memanggil fbLog yang kini juga mengirim notifikasi via botLog
        if (WiFi.status() == WL_CONNECTED)
            fbLog(0, String("UNKNOWN"), String(""), String("access_denied")); 
        else
            Serial.println("[FB] Offline, skipping denied log.");
    }
}

void scanRFID() {
    // Hentikan scanning jika sedang enroll
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
            updateDisplay("Akses Diterima", name, "ID: " + String(i));
            //digitalWrite(LED_OK, HIGH);
            ledAccessGranted();
            
            // Memanggil fbLog yang kini juga mengirim notifikasi via botLog
            if (WiFi.status() == WL_CONNECTED)
                fbLog(i, name, uid, String("access_granted"));
            else {
                Serial.println("[FB] Offline, skipping log.");
            }
            return;
        }
    }

    // Logika akses ditolak
    Serial.print("[RFID] UID not found: "); Serial.println(uid);
    updateDisplay("AKSES DITOLAK", "Kartu RFID", "Tidak Terdaftar");
    //digitalWrite(LED_FAIL, HIGH);
    ledAccessDenied();
    
    // Memanggil fbLog yang kini juga mengirim notifikasi via botLog
    if (WiFi.status() == WL_CONNECTED)
        fbLog(0, String("UNKNOWN"), uid, String("access_denied")); // Kirim UID jika ada tapi tidak terdaftar
    else
        Serial.println("[FB] Offline, skipping denied log.");
}

// --- ENROLL MODE (DARI KODE ASLI) ---

void enrollFingerprint(uint8_t id) {
    int p = -1;
    Serial.println("\n[ENROLL FP] Tempatkan jari pertama...");
    
    while (p != FINGERPRINT_OK) {
        p = finger.getImage();
        switch (p) {
        case FINGERPRINT_OK:
            Serial.println("Gambar terambil.");
            break;
        case FINGERPRINT_NOFINGER:
            Serial.print(".");
            break;
        case FINGERPRINT_PACKETRECIEVEERR:
            Serial.println("Kirim ulang paket.");
            break;
        default:
            Serial.println("Error tidak diketahui.");
            return;
        }
    }
    
    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
        Serial.println("Gagal konversi gambar pertama.");
        return;
    }

    Serial.println("Lepas jari dan tempatkan jari yang sama untuk kedua kalinya...");
    p = -1;
    while (p != FINGERPRINT_NOFINGER) {
        p = finger.getImage();
    }
    
    p = -1;
    while (p != FINGERPRINT_OK) {
        p = finger.getImage();
        switch (p) {
        case FINGERPRINT_OK:
            Serial.println("Gambar kedua terambil.");
            break;
        case FINGERPRINT_NOFINGER:
            Serial.print(".");
            break;
        default:
            Serial.println("Error tidak diketahui.");
            return;
        }
    }
    
    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
        Serial.println("Gagal konversi gambar kedua.");
        return;
    }
    
    p = finger.createModel();
    if (p != FINGERPRINT_OK) {
        Serial.println("Sidik jari tidak cocok.");
        return;
    }
    
    p = finger.storeModel(id);
    if (p == FINGERPRINT_OK) {
        Serial.println("✅ Sidik jari berhasil disimpan di sensor!");
    } else {
        Serial.println("❌ Gagal menyimpan sidik jari di sensor.");
    }
}

// ------------ MQTT Subscribe ------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (int i = 0; i<length; i++) msg += (char) payload[i];

    Serial.print("[MQTT] ");
    Serial.print(topic);
    Serial.print(" => ");
    Serial.println(msg);

    if (String(topic) == TOPIC_SWITCH) {
        lastSwitchStatus = msg;
        Serial.print("[DOOR] Status: ");
        Serial.print(msg);
        if (msg == "open") {
            if (authorizedOpen) {
                Serial.println("Pintu Terbuka dengan akses sah");
            } else {
                Serial.println("[PENYUSUP!] Pintu dibuka tanpa autentikasi");
                triggerAlarm();
            } 
        }
    }
}

//------------ MQTT Reconnect ------------
void mqttReconnect() {
    while (!mqttClient.connected()) {
        Serial.print("[MQTT] Connecting...");

        if (mqttClient.connect("Esp32_MainUnit")) {
            Serial.println("Connected!");
            mqttClient.subscribe(TOPIC_SWITCH);
            Serial.println("[MQTT] Subscribed: " TOPIC_SWITCH);
        } else {
            Serial.print("Failed rc=");
            Serial.println(mqttClient.state());
            delay(200);
        }
    }
}

//------------ Alarm Buzzer ------------
void triggerAlarm() {
    String alertMsg = "⚠️ *PENYUSUP TERDETEKSI* ⚠️\nPintu dibuka tanpa autentikasi!";
    botLog.sendMessage(CHAT_ID, alertMsg, "Markdown");
    for (int i = 0; i < 10; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(1000);
        digitalWrite(BUZZER_PIN, LOW);
        delay(200);
    }
}