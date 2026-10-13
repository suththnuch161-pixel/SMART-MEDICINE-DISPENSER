#include <WiFi.h>
#include <WebServer.h>      
#include <DNSServer.h>      
#include <WiFiManager.h>    
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <DHT.h>
#include "time.h"

// 🔗 ลิงก์ Web App URL ปัจจุบันของคุณ (ตั้งค่าไว้เป็นไอดีเดิมเรียบร้อยครับ)
const char* webAppUrl = "https://script.google.com/macros/s/AKfycbx0TlR-akVJaW92Ux2kJq_vvahA2jcUxngZX84vcc9ypct54amsxiqfjosjC_E7taFA/exec";

// --- การตั้งค่าขาพอร์ตและดีไฟน์สำคัญ ---
#define RELAY_PIN         25    // พินสั่งงานรีเลย์พัดลมระบายอากาศ
#define DHTPIN            4     // พินเชื่อมต่อเซ็นเซอร์ DHT22
#define DHTTYPE           DHT22   

#define SERVO1_PIN        13
#define SERVO2_PIN        12
#define SERVO3_PIN        14
#define SERVO4_PIN        27

#define IR1_PIN           32
#define IR2_PIN           34
#define IR3_PIN           35
#define IR4_PIN           39

// --- ตัวแปรควบคุมระบบพัดลม ---
volatile float target_temp = 30; 
bool relay_state = false;

// --- ตัวแปรเก็บหน้าปัจจุบันของ Nextion ---
int current_page = 99; // เริ่มต้นที่หน้า Welcome

// --- ตัวแปรสำหรับ NTP Server เวลา ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;      // เวลาประเทศไทย GMT+7
const int   daylightOffset_sec = 0;

// --- ประกาศอินสแตนซ์ของอุปกรณ์ต่าง ๆ ---
DHT dht(DHTPIN, DHTTYPE);
Servo servo1; Servo servo2; Servo servo3; Servo servo4;

// --- ตัวแปรเก็บสถานะการทำงานของเซอร์โวมอเตอร์ ---
bool servo1_state = false;
bool servo2_state = false;
bool servo3_state = false;
bool servo4_state = false;

// --- ตัวแปรส่งยืนยันเพื่อล้างสถานะปุ่มฝั่ง Google Sheets (Handshake) ---
volatile int clearedMorn = 0;
volatile int clearedNoon = 0;
volatile int clearedEve = 0;
volatile int clearedBed = 0;

// --- ตัวแปรสำหรับแชร์ข้อมูลข้ามไป Core 0 (Thread-Safe) ---
volatile float shared_temp = 0.0;
volatile float shared_humi = 0.0;
volatile int shared_ir1 = 1;
volatile int shared_ir2 = 1;
volatile int shared_ir3 = 1;
volatile int shared_ir4 = 1;

// --- สัญญาณทริกเกอร์จากฝั่งเว็บแอปคลาวด์ส่งกลับมา Core 1 ---
volatile bool web_trigger_morn = false;
volatile bool web_trigger_noon = false;
volatile bool web_trigger_eve = false;
volatile bool web_trigger_bed = false;

// --- ตัวแปรสถานะและการหน่วงเวลาของ Core 1 ---
bool lastWifiState = false; 
unsigned long lastWifiCheckTime = 0;
unsigned long lastDHTTime = 0;
unsigned long lastTimeUpdate = 0;
unsigned long lastSensorTime = 0;
unsigned long lastAutoTime = 0; 

// =========================================================================
// ➕ [เพิ่มใหม่] ตัวแปรเก็บเวลาที่ดึงมาจาก Web App (สำหรับโหมด Auto)
// =========================================================================
volatile int web_hour_morn = 8,  web_min_morn = 0;  // มื้อเช้า (ค่าเริ่มต้น 08:00)
volatile int web_hour_noon = 12, web_min_noon = 0;  // มื้อเที่ยง (ค่าเริ่มต้น 12:00)
volatile int web_hour_eve  = 18, web_min_eve  = 0;  // มื้อเย็น (ค่าเริ่มต้น 18:00)
volatile int web_hour_bed  = 21, web_min_bed  = 0;  // ก่อนนอน (ค่าเริ่มต้น 21:00)

// =========================================================================
// ➕ [เพิ่มใหม่] ตัวแปรนับเวลาเปิดไฟสีเขียวค้าง 5 นาที (300,000 ms) บนหน้าจอ
// =========================================================================
unsigned long greenTimer1 = 0;
unsigned long greenTimer2 = 0;
unsigned long greenTimer3 = 0;
unsigned long greenTimer4 = 0;

bool is_manual_active = false; // ปกติเป็น false (ระบบ Auto ทำงานได้ทุกหน้า)

// =================================================================
// ตัวแปรเก็บค่า Trigger ที่รับมาจาก Google Sheets
// =================================================================
int trig_morn = 0;
int trig_noon = 0;
int trig_eve  = 0;
int trig_bed  = 0;

int manual_morn = 0;
int manual_noon = 0;
int manual_eve  = 0;
int manual_bed  = 0;

// --- ฟังก์ชันช่วยเหลือในการส่งชุดคำสั่งไปที่หน้าจอ Nextion ---
void sendNextion(String cmd) {
  Serial.print(cmd);
  Serial.write(0xff);
  Serial.write(0xff);
  Serial.write(0xff);
}

// --- ฟังก์ชันศูนย์กลางการสั่งหมุน Servo ---
void triggerServo(int slot) {
  if (slot == 1) {
    servo1_state = !servo1_state;
    if (servo1_state) { servo1.write(180); } else { servo1.write(0); }
  } 
  else if (slot == 2) {
    servo2_state = !servo2_state;
    if (servo2_state) { servo2.write(180); } else { servo2.write(0); }
  } 
  else if (slot == 3) {
    servo3_state = !servo3_state;
    if (servo3_state) { servo3.write(180); } else { servo3.write(0); }
  } 
  else if (slot == 4) {
    servo4_state = !servo4_state;
    if (servo4_state) { servo4.write(180); } else { servo4.write(0); }
  }
  delay(800); // หน่วงเวลาให้เฟืองสไลด์ยาทำงานได้สำเร็จเรียบร้อย
}

// =========================================================================
// 🧠 สมองซีกที่ 2 (Core 0): แอบเชื่อมต่อคลาวด์ในเบื้องหลัง ป้องกันหน้าจอค้าง
// =========================================================================
void TaskWebSync(void *pvParameters) {
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClientSecure client;
      client.setInsecure(); // ข้ามขั้นตอนการตรวจจับใบรับรองเพื่อการดึงข้อมูลที่รวดเร็ว
      
      HTTPClient http;
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      
      // ดึงสแนปช็อตสถานะล่าสุดที่ Core 1 อัปเดตไว้ส่งขึ้นคลาวด์
      String fullUrl = String(webAppUrl) + 
                       "?temp=" + String((float)shared_temp, 1) +
                       "&humi=" + String((float)shared_humi, 0) +
                       "&morn_ir=" + String(shared_ir1) +
                       "&noon_ir=" + String(shared_ir2) +
                       "&eve_ir=" + String(shared_ir3) +
                       "&bed_ir=" + String(shared_ir4) +
                       "&cleared_morn=" + String(clearedMorn) +
                       "&cleared_noon=" + String(clearedNoon) +
                       "&cleared_eve=" + String(clearedEve) +
                       "&cleared_bed=" + String(clearedBed);

      http.begin(client, fullUrl);
      int httpCode = http.GET(); 

      if (httpCode > 0) {
        // 📌 [เพิ่มใหม่] ถ้าส่งค่า cleared ออกไปหา Google Sheet สำเร็จแล้ว ให้เคลียร์เป็น 0 ทันที
        if (clearedMorn == 1) clearedMorn = 0;
        if (clearedNoon == 1) clearedNoon = 0;
        if (clearedEve == 1)  clearedEve  = 0;
        if (clearedBed == 1)  clearedBed  = 0;

        String payload = http.getString();
        if (payload.startsWith("{")) {
          StaticJsonDocument<512> resDoc;
          DeserializationError error = deserializeJson(resDoc, payload);

          if (!error) {
            // 📌 1. ดึงค่า Trigger (สีส้ม - โหมด AUTO เมื่อถึงเวลา)
            trig_morn = resDoc["Trigger_Morn"];
            trig_noon = resDoc["Trigger_Noon"];
            trig_eve  = resDoc["Trigger_Eve"]; 
            trig_bed  = resDoc["Trigger_Bed"];

            // 📌 2. ดึงค่า Manual (สีม่วง - โหมดกดสั่งจ่ายยาผ่านเว็บ)
            manual_morn = resDoc["Manual_Morn"];
            manual_noon = resDoc["Manual_Noon"];
            manual_eve  = resDoc["Manual_Eve"];
            manual_bed  = resDoc["Manual_Bed"];

            // ❌ [ลบส่วน 📌 3 เดิมออกทั้งหมด!] 
            // ไม่ต้องใส่บรรทัด clearedMorn = (trig_morn == 1) ? 1 : 0; ตรงนี้แล้ว
            // เพราะให้ลูปสั่งจ่ายยา (เช็ค manual == 1 หรือ trig == 1) เป็นคนตั้งค่า cleared... = 1 เองครับ
          }
        }
      }
      http.end();
    }
    // สั่ง Sleep ตัวเองชั่วขณะ (2 วินาที)
    vTaskDelay(pdMS_TO_TICKS(2000)); 
  }
}

// =========================================================================
// 🛠️ โหมดการตั้งค่าและเตรียมฮาร์ดแวร์ก่อนทำงาน
// =========================================================================
void setup() {
  // ⚡ บังคับให้ CPU ของ ESP32 ทำงานที่ความเร็วสูงสุด 240MHz เพื่อคำนวณ SSL ได้เร็วขึ้น
  //setCpuFrequencyMhz(240); // รันความเร็วจากคริสตัลหลัก
  // ตั้งค่าเสถียรภาพสูงสุดในการสื่อสารกับบอร์ดหน้าจอ Nextion HMI ที่ 9600 bps
  Serial.begin(9600); 
  
  pinMode(IR1_PIN, INPUT);
  pinMode(IR2_PIN, INPUT);
  pinMode(IR3_PIN, INPUT);
  pinMode(IR4_PIN, INPUT);
  
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // ป้องกันมอเตอร์รีเลย์สปาร์คตอนบูต

  dht.begin();
  
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo3.attach(SERVO3_PIN);
  servo4.attach(SERVO4_PIN);

  //servo1.write(0);
  //servo2.write(0);
  //servo3.write(0);
  //servo4.write(0);

  // สตาร์ทระบบเชื่อมต่อไวไฟอัจฉริยะ WiFiManager
  WiFiManager wm;
  if(!wm.autoConnect("SMART MEDICINE DISPENSER")) {
      WiFi.disconnect(true); 
      WiFi.mode(WIFI_OFF);   
      lastWifiState = false;
  } 
  else {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      lastWifiState = true;
  }

  // 🔥 สรรสร้างและเปิดหน้าต่างให้สมองซีก Core 0 รันฟังก์ชันซิงค์เว็บแอปเบื้องหลังไร้รอยต่อ
  xTaskCreatePinnedToCore(
    TaskWebSync,       /* ฟังก์ชันงาน */
    "WebSyncTask",     /* ชื่อของงานที่สร้าง */
    10000,             /* ขนาดสแต็ก (HTTPS ต้องการหน่วยความจำกว้างเพื่อความปลอดภัย) */
    NULL,              /* พารามิเตอร์ */
    1,                 /* ลำดับความสำคัญการรัน */
    NULL,              /* ตัวแปรเก็บงาน */
    0                  /* สั่งปักหลักทำงานที่ Core 0 */
  );
}

// =========================================================================
// 🧠 สมองซีกที่ 1 (Core 1): ทำงานตาม loop() หลักอย่างอิสระ ไม่ดีเลย์แน่นอน
// =========================================================================
void loop() {
  struct tm timeinfo;
  bool timeValid = getLocalTime(&timeinfo, 0); 

  // =========================================================================
  // ☁️ 0. ตรวจเช็คคำสั่งกดจ่ายยาแบบ Manual จาก Web App (ป้องกันการสั่งซ้ำด้วย Debounce)
  // =========================================================================
  static unsigned long lastManualTime1 = 0;
  static unsigned long lastManualTime2 = 0;
  static unsigned long lastManualTime3 = 0;
  static unsigned long lastManualTime4 = 0;

  // --- ช่องยาที่ 1 ---
  if (manual_morn == 1) { 
    manual_morn = 0; // 📌 เคลียร์ค่าตัวแปรใน ESP32 ทันที
    clearedMorn = 1; // 📌 สั่งแจ้ง Google Sheets ให้เคลียร์ค่าบนเว็บด้วย
    
    // บล็อกไม่ให้หมุนซ้ำถ้าระยะเวลาห่างจากรอบล่าสุดไม่ถึง 5 วินาที (5000 ms)
    if (millis() - lastManualTime1 >= 5000) {
      if (digitalRead(IR1_PIN) == LOW) {
        triggerServo(1);
        //greenTimer1 = millis(); 
      }
      lastManualTime1 = millis(); // บันทึกเวลาที่สั่งงานล่าสุด
    }
  }

  // --- ช่องยาที่ 2 ---
  if (manual_noon == 1) { 
    manual_noon = 0; 
    clearedNoon = 1;
    
    if (millis() - lastManualTime2 >= 5000) {
      if (digitalRead(IR2_PIN) == LOW) {
        triggerServo(2);
        //greenTimer2 = millis(); 
      }
      lastManualTime2 = millis();
    }
  }

  // --- ช่องยาที่ 3 ---
  if (manual_eve == 1) { 
    manual_eve = 0; 
    clearedEve = 1;
    
    if (millis() - lastManualTime3 >= 5000) { // 📌 ล็อคช่อง 3 ไม่ให้หมุนซ้ำภายใน 5 วินาที
      if (digitalRead(IR3_PIN) == LOW) {
        triggerServo(3);
        //greenTimer3 = millis(); 
      }
      lastManualTime3 = millis();
    }
  }

  // --- ช่องยาที่ 4 ---
  if (manual_bed == 1) { 
    manual_bed = 0; 
    clearedBed = 1;
    
    if (millis() - lastManualTime4 >= 5000) { // 📌 ล็อคช่อง 4 ไม่ให้หมุนซ้ำภายใน 5 วินาที
      if (digitalRead(IR4_PIN) == LOW) {
        triggerServo(4);
        //greenTimer4 = millis(); 
      }
      lastManualTime4 = millis();
    }
  }

  // --- 1. จัดการเข็มเวลานาฬิกาส่งไปวาดบนจอ Nextion (ทำงานทุก ๆ 1 วินาที) ---
  if (millis() - lastTimeUpdate >= 1000) {
    lastTimeUpdate = millis();
    if (timeValid && (WiFi.status() == WL_CONNECTED)) {
      char timeString[9]; 
      sprintf(timeString, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      sendNextion("t2.txt=\"" + String(timeString) + "\"");
    }
  }
  
  // --- 2. วัดระดับอุณหภูมิและความชื้นตู้ยา (ทำงานทุก ๆ 2 วินาที) ---
 if (millis() - lastDHTTime >= 2000) {
    lastDHTTime = millis();
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
      shared_temp = t;
      shared_humi = h;

      sendNextion("t0.txt=\"" + String(t, 1) + " \"");
      sendNextion("t1.txt=\"" + String(h, 0) + " \"");
      
      if (t >= target_temp) {
        if (!relay_state) {
          relay_state = true;
          digitalWrite(RELAY_PIN, HIGH); 
          Serial.println("พัดลมติด");
        }
      } 
      else if (t < (target_temp- 0.5)) {
        if (relay_state) {
          relay_state = false;
          digitalWrite(RELAY_PIN, LOW); 
          Serial.println("พัดลมดับ");
        }
      }
    }
  }

  // --- 3. วิเคราะห์สเตตัสกระปุกยาสแกนด้วยลำแสง IR และรายงานสีปุ่ม (ทำงานทุก ๆ 500ms) ---
  if (millis() - lastSensorTime >= 500) {
    lastSensorTime = millis();

    int ir1 = digitalRead(IR1_PIN);
    int ir2 = digitalRead(IR2_PIN);
    int ir3 = digitalRead(IR3_PIN);
    int ir4 = digitalRead(IR4_PIN);

    shared_ir1 = (ir1 == LOW) ? 1 : 0;
    shared_ir2 = (ir2 == LOW) ? 1 : 0;
    shared_ir3 = (ir3 == LOW) ? 1 : 0;
    shared_ir4 = (ir4 == LOW) ? 1 : 0;
    
    if (ir1 == LOW) { sendNextion("b50.bco=2016"); } else { sendNextion("b50.bco=63488"); }
    sendNextion("ref b50");

    if (ir2 == LOW) { sendNextion("b60.bco=2016"); } else { sendNextion("b60.bco=63488"); }
    sendNextion("ref b60");

    if (ir3 == LOW) { sendNextion("b70.bco=2016"); } else { sendNextion("b70.bco=63488"); }
    sendNextion("ref b70");

    if (ir4 == LOW) { sendNextion("b80.bco=2016"); } else { sendNextion("b80.bco=63488"); }
    sendNextion("ref b80");
  }

  // --- 3.5 ระบบสแกนระดับความเข้มคลื่น Wi-Fi (ทำงานทุก ๆ 5 วินาที) ---
  if (millis() - lastWifiCheckTime >= 500) {
    lastWifiCheckTime = millis();
    bool currentWifiState = (WiFi.status() == WL_CONNECTED);

    if (currentWifiState != lastWifiState) {
        lastWifiState = currentWifiState;
        while(Serial.available() > 0) { Serial.read(); } 
    }

    if (currentWifiState == true) {
      long rssi = WiFi.RSSI(); 
      if (rssi >= -67) { sendNextion("pWifi.pic=4"); } else { sendNextion("pWifi.pic=5"); } 

      sendNextion("tWifiName.txt=\"" + WiFi.SSID() + "\"");
      sendNextion("tWifiRssi.txt=\"" + String(rssi) + " dBm\"");
      sendNextion("tWifiIp.txt=\"" + WiFi.localIP().toString() + "\"");
      sendNextion("ref pWifi");
    } else {
      sendNextion("pWifi.pic=6");
      sendNextion("tWifiName.txt=\"Disconnected\"");
      sendNextion("tWifiRssi.txt=\"0 dBm\"");
      sendNextion("tWifiIp.txt=\"0.0.0.0\"");
      sendNextion("ref pWifi");
    }
  }

// =========================================================================
// ⏰ 4. [โหมด AUTO] จ่ายยาตามเวลาที่ตั้งค่ามาจากเว็บ
// =========================================================================
static bool auto_triggered1 = false;
static bool auto_triggered2 = false;
static bool auto_triggered3 = false;
static bool auto_triggered4 = false;

// 📌 ตัวแปรป้องกันการหมุนซ้ำซ้อนจาก JSON ค้าง (Debounce Timer 5 วินาที)
static unsigned long lastAutoExec1 = 0;
static unsigned long lastAutoExec2 = 0;
static unsigned long lastAutoExec3 = 0;
static unsigned long lastAutoExec4 = 0;

if (timeValid && (WiFi.status() == WL_CONNECTED) && (millis() - lastAutoTime >= 1000)) {
  lastAutoTime = millis();
  
  // --- มื้อเช้า (Morn) ---
  if (trig_morn == 1) {
    if (is_manual_active) {
      trig_morn = 0;
      clearedMorn = 1; // อยู่หน้า Manual ให้ยกเลิกและเคลียร์บน Sheets ทิ้ง
    } else if (!auto_triggered1) {
      auto_triggered1 = true;
      trig_morn = 0;
      clearedMorn = 1; 

      // เช็คว่าไม่ได้เพิ่งสั่งหมุนไปภายใน 5 วินาที และมียาขวางอยู่
      if ((millis() - lastAutoExec1 >= 5000) && digitalRead(IR1_PIN) == LOW) {
        triggerServo(1);
        greenTimer1 = millis(); 
        lastAutoExec1 = millis(); // บันทึกเวลาที่หมุนล่าสุด
      }
    }
  } else {
    auto_triggered1 = false; 
  }

  // --- มื้อเที่ยง (Noon) ---
  if (trig_noon == 1) {
    if (is_manual_active) {
      trig_noon = 0;
      clearedNoon = 1;
    } else if (!auto_triggered2) {
      auto_triggered2 = true;
      trig_noon = 0;
      clearedNoon = 1;

      if ((millis() - lastAutoExec2 >= 5000) && digitalRead(IR2_PIN) == LOW) {
        triggerServo(2);
        greenTimer2 = millis(); 
        lastAutoExec2 = millis();
      }
    }
  } else {
    auto_triggered2 = false;
  }

  // --- มื้อเย็น (Eve) ---
  if (trig_eve == 1) {
    if (is_manual_active) {
      trig_eve = 0;
      clearedEve = 1;
    } else if (!auto_triggered3) {
      auto_triggered3 = true;
      trig_eve = 0;
      clearedEve = 1;

      // 📌 ล็อคช่อง 3 ป้องกันการสั่งหมุนซ้ำซ้อนภายใน 5 วินาที
      if ((millis() - lastAutoExec3 >= 5000) && digitalRead(IR3_PIN) == LOW) {
        triggerServo(3);
        greenTimer3 = millis(); 
        lastAutoExec3 = millis();
      }
    }
  } else {
    auto_triggered3 = false;
  }

  // --- มื้อก่อนนอน (Bed) ---
  if (trig_bed == 1) {
    if (is_manual_active) {
      trig_bed = 0;
      clearedBed = 1;
    } else if (!auto_triggered4) {
      auto_triggered4 = true;
      trig_bed = 0;
      clearedBed = 1;

      // 📌 ล็อคช่อง 4 ป้องกันการสั่งหมุนซ้ำซ้อนภายใน 5 วินาที
      if ((millis() - lastAutoExec4 >= 5000) && digitalRead(IR4_PIN) == LOW) {
        triggerServo(4);
        greenTimer4 = millis(); 
        lastAutoExec4 = millis();
      }
    }
  } else {
    auto_triggered4 = false;
  }
}

  // =========================================================================
  // 🟢 4.5 ควบคุมการแสดงไฟสีเขียวค้าง 5 นาที (300,000 ms) บนหน้าจอ Nextion
  // =========================================================================
  static unsigned long lastGreenUpdate = 0;
  if (millis() - lastGreenUpdate >= 2000) {
    lastGreenUpdate = millis();

    // มื้อเช้า
    if (greenTimer1 > 0 && (millis() - greenTimer1 < 300000)) {
      sendNextion("bMorn.bco=2016"); 
    } else {
      sendNextion("bMorn.bco=65535"); 
      greenTimer1 = 0;
    }
    sendNextion("ref bMorn");

    // มื้อเที่ยง
    if (greenTimer2 > 0 && (millis() - greenTimer2 < 300000)) {
      sendNextion("bNoon.bco=2016");
    } else {
      sendNextion("bNoon.bco=65535");
      greenTimer2 = 0;
    }
    sendNextion("ref bNoon");

    // มื้อเย็น
    if (greenTimer3 > 0 && (millis() - greenTimer3 < 300000)) {
      sendNextion("bEve.bco=2016");
    } else {
      sendNextion("bEve.bco=65535");
      greenTimer3 = 0;
    }
    sendNextion("ref bEve");

    // มื้อก่อนนอน
    if (greenTimer4 > 0 && (millis() - greenTimer4 < 300000)) {
      sendNextion("bBed.bco=2016");
    } else {
      sendNextion("bBed.bco=65535");
      greenTimer4 = 0;
    }
    sendNextion("ref bBed");
  }

  // =========================================================================
// 🔘 5. คอยรับสัญญาณและตอบสนองการกดสัมผัสผ่านทางหน้าจอ Nextion
// =========================================================================
if (Serial.available() > 0) {
  uint8_t header = Serial.peek(); 

  // --- กรณีที่ 1: รับข้อมูลการกดปุ่ม (0x65) ---
  if (header == 0x65 && Serial.available() >= 7) {
    Serial.read(); 
    
    uint8_t pageID     = Serial.read();  
    uint8_t compID     = Serial.read();  
    uint8_t touchEvent = Serial.read(); 
    
    Serial.read(); Serial.read(); Serial.read(); // เคลียร์ FF FF FF

    current_page = pageID; 

    // 📌 เช็คสถานะหน้าจอว่าอยู่ในหน้า Manual หรือไม่ให้ถูกต้อง
    if (pageID == 2) { 
      is_manual_active = true;  // ถ้าเป็น Page 2 ให้ถือว่าเป็นหน้า Manual
    } else { 
      is_manual_active = false; // หน้าอื่นๆ (เช่น Page 0, Page 1) เป็นหน้าปกติ
    }

    // 📌 ปุ่มสั่งหมุน Servo จ่ายยาแบบกดเองในหน้า Manual บนจอ Nextion (Slot 1-4)
    if (is_manual_active) { 
      if      ((compID == 5 || compID == 10) && digitalRead(IR1_PIN) == LOW) { triggerServo(1); } 
      else if ((compID == 6 || compID == 20) && digitalRead(IR2_PIN) == LOW) { triggerServo(2); } 
      else if ((compID == 7 || compID == 30) && digitalRead(IR3_PIN) == LOW) { triggerServo(3); } 
      else if ((compID == 8 || compID == 40) && digitalRead(IR4_PIN) == LOW) { triggerServo(4); }
    }
  }
  
  // --- กรณีที่ 2: อัปเดตหน้าจาก sendme (0x66) ---
  else if (header == 0x66 && Serial.available() >= 5) {
    Serial.read(); 
    uint8_t pageID = Serial.read(); 
    Serial.read(); Serial.read(); Serial.read(); 
    
    current_page = pageID;
    
    if (current_page == 2) { 
      is_manual_active = true; 
    } else { 
      is_manual_active = false; 
    }
  }
  
  // ล้างขยะข้อมูลอื่น
  else if (header != 0x65 && header != 0x66) {
    Serial.read(); 
  }
}
  delay(1);
}