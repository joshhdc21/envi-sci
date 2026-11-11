#include <Arduino.h>
#include <ESP8266WiFi.h>      // Replaced WiFi.h for ESP8266
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <SoftwareSerial.h>   // Added for PMS5003 on ESP8266
#include <Firebase_ESP_Client.h>

// Provide token status and RTDB helper functions
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- WIFI CREDENTIALS ---
#define WIFI_SSID "COEfaculty"
#define WIFI_PASSWORD "UR54S9De"

// --- FIREBASE CONFIGURATION ---
#define API_KEY "AIzaSyAMidQ4KHYD5qMGCqV8JLr5020uuUb2fnk"
#define DATABASE_URL "https://envi-sci-project-default-rtdb.asia-southeast1.firebasedatabase.app"

// --- NODE CONFIGURATION ---
#define FLOOR_PATH "/sensors/floor2"

// --- HARDWARE PINS & OBJECTS ---
// BME680 I2C Pins
Adafruit_BME680 bme; 

// PMS5003 using SoftwareSerial on ESP8266
// NodeMCU Pins: D5 (GPIO14) as RX, D6 (GPIO12) as TX
SoftwareSerial pmsSerial(14, 12); 

// Firebase Objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// PMS5003 Data Structure
struct pms5003data {
  uint16_t framelen, pm10_standard, pm25_standard, pm100_standard;
  uint16_t pm10_env, pm25_env, pm100_env;
  uint16_t particles_03um, particles_05um, particles_10um;
  uint16_t particles_25um, particles_50um, particles_100um;
  uint16_t unused, checksum;
};
struct pms5003data data;

boolean readPMSdata(Stream *s);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- ENVI-SCI NODE 2 (ESP8266) STARTING ---");

  // 1. Wi-Fi Connection
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    retries++;
    if (retries > 40) {
      Serial.println("\n❌ Wi-Fi Connection Failed! Check your credentials.");
      retries = 0;
    }
  }
  Serial.println("\n✅ WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // 2. Firebase Configuration
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("✅ Firebase Signup Success");
    signupOK = true;
  } else {
    Serial.printf("❌ Firebase Signup Failed: %s\n", config.signer.signupError.message.c_str());
  }

  config.token_status_callback = tokenStatusCallback; 

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // 3. Sensors Initialization
  // ESP8266 Default I2C: SDA = D2 (GPIO4), SCL = D1 (GPIO5)
  Wire.begin(4, 5); 
  if (!bme.begin(0x76)) {
    if (!bme.begin(0x77)) {
      Serial.println("❌ BME680 ERROR: Check I2C wiring (SDA=D2, SCL=D1)");
    }
  } else {
    Serial.println("✅ BME680 Detected");
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setGasHeater(320, 150);
  }

  // Initialize SoftwareSerial for PMS5003
  pmsSerial.begin(9600);
  Serial.println("✅ PMS5003 SoftwareSerial Started on RX=D5, TX=D6");
  
  Serial.println("--- SYSTEM ONLINE ---");
}

void loop() {
  static unsigned long lastTime = 0;
  
  if (millis() - lastTime > 5000) {
    lastTime = millis();
    
    float temp = 0, hum = 0, pres = 0;
    int pm1 = 0, pm25 = 0, pm10 = 0;
    bool bmeSuccess = false;
    bool pmsSuccess = false;

    // Read BME680
    if (bme.performReading()) {
      temp = bme.temperature;
      hum = bme.humidity;
      pres = bme.pressure / 100.0;
      bmeSuccess = true;
      Serial.printf("[BME] T:%.1f C H:%.1f %% P:%.1f hPa\n", temp, hum, pres);
    } else {
      Serial.println("❌ BME Read Failed");
    }

    // Read PMS5003 via SoftwareSerial
    if (readPMSdata(&pmsSerial)) {
      pm1 = data.pm10_env;    
      pm25 = data.pm25_env;   
      pm10 = data.pm100_env;  
      pmsSuccess = true;
      Serial.printf("[PMS] 1.0:%d | 2.5:%d | 10.0:%d\n", pm1, pm25, pm10);
    } 

    // --- SEND TO FIREBASE ---
    if (Firebase.ready() && signupOK) {
      String path = FLOOR_PATH; 

      if (bmeSuccess) {
        Firebase.RTDB.setFloat(&fbdo, path + "/temperature", temp);
        Firebase.RTDB.setFloat(&fbdo, path + "/humidity", hum);
        Firebase.RTDB.setFloat(&fbdo, path + "/pressure", pres);
      }
      
      if (pmsSuccess) {
        Firebase.RTDB.setInt(&fbdo, path + "/pm1", pm1);
        Firebase.RTDB.setInt(&fbdo, path + "/pm25", pm25);
        Firebase.RTDB.setInt(&fbdo, path + "/pm10", pm10);
      }

      if (bmeSuccess || pmsSuccess) {
        Serial.println("🚀 Data Uploaded Successfully to " + path);
      }
    } else {
      Serial.println("⚠️ Firebase not ready yet.");
    }
    Serial.println("-------------------------------------");
  }
}

// Helper: PMS Read Checksum Parser
boolean readPMSdata(Stream *s) {
  if (!s->available()) return false;
  
  while (s->available() && s->peek() != 0x42) s->read();
  
  unsigned long start = millis();
  while (s->available() < 32 && (millis() - start < 150)) delay(1);
  
  if (s->available() < 32) return false;
  
  uint8_t buffer[32]; uint16_t sum = 0;
  s->readBytes(buffer, 32);
  for (uint8_t i = 0; i < 30; i++) sum += buffer[i];
  
  uint16_t buffer_u16[15];
  for (uint8_t i = 0; i < 15; i++) {
    buffer_u16[i] = buffer[2 + i * 2 + 1];
    buffer_u16[i] += (buffer[2 + i * 2] << 8);
  }
  memcpy((void *)&data, (void *)buffer_u16, 30);
  
  if (sum != data.checksum) return false;
  return true;
}