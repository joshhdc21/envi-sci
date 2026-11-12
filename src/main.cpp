
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <time.h>

// =====================================
// 1. WIFI CREDENTIALS
// =====================================

#define WIFI_SSID "COEfaculty"
#define WIFI_PASSWORD "UR54S9De"

// =====================================
// 2. FIREBASE CONFIGURATION
// =====================================

#define API_KEY "AIzaSyAMidQ4KHYD5qMGCqV8JLr5020uuUb2fnk"

#define DATABASE_URL \
"https://envi-sci-project-default-rtdb.asia-southeast1.firebasedatabase.app"

#define FLOOR_PATH "/sensors/floor2"

// Firebase Authentication
const char* SIGNUP_URL =
"https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=";

const char* REFRESH_URL =
"https://securetoken.googleapis.com/v1/token?key=";

String idToken = "";
String refreshToken = "";

unsigned long tokenExpiry = 0;

// =====================================
// 3. HARDWARE CONFIGURATION
// =====================================

#define BME_ADDRESS 0x77
#define BME_SDA D1
#define BME_SCL D2

Adafruit_BME680 bme;

// PMS5003 TX -> ESP8266 D7
SoftwareSerial pmsSerial(D7, -1);

bool bmeInitialized = false;
bool authInitialized = false;

// =====================================
// 4. PMS5003 DATA STRUCTURE
// =====================================

struct pms5003data {
    uint16_t framelen;
    uint16_t pm10_standard;
    uint16_t pm25_standard;
    uint16_t pm100_standard;
    uint16_t pm10_env;
    uint16_t pm25_env;
    uint16_t pm100_env;
    uint16_t particles_03um;
    uint16_t particles_05um;
    uint16_t particles_10um;
    uint16_t particles_25um;
    uint16_t particles_50um;
    uint16_t particles_100um;
    uint16_t unused;
    uint16_t checksum;
};

pms5003data data;

bool readPMSdata(Stream *s);

// =====================================
// 5. HELPER FUNCTIONS
// =====================================

uint16_t read16(const uint8_t *buffer, uint8_t position) {
    return ((uint16_t)buffer[position] << 8) |
           buffer[position + 1];
}

// =====================================
// 6. FIREBASE ANONYMOUS SIGNUP
// =====================================

bool firebaseSignup() {

    Serial.println("Creating Firebase anonymous session...");

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;

    String url = String(SIGNUP_URL) + API_KEY;

    if (!https.begin(client, url)) {
        Serial.println("Unable to start Firebase signup request.");
        return false;
    }

    https.addHeader("Content-Type", "application/json");

    String payload = "{\"returnSecureToken\":true}";

    int httpCode = https.POST(payload);

    if (httpCode > 0) {

        String response = https.getString();

        if (httpCode == HTTP_CODE_OK) {

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, response);

            if (!error) {

                idToken = doc["idToken"].as<String>();
                refreshToken = doc["refreshToken"].as<String>();

                unsigned long expiresIn =
                    doc["expiresIn"].as<unsigned long>();

                tokenExpiry =
                    millis() + (expiresIn > 60 ? expiresIn - 60 : 1) * 1000UL;

                Serial.println("Firebase anonymous signup successful!");

                https.end();
                return true;
            }
        }

        Serial.printf("Firebase signup HTTP error: %d\n", httpCode);
        Serial.println(response);

    } else {

        Serial.printf("Signup request failed: %s\n",
                      https.errorToString(httpCode).c_str());
    }

    https.end();
    return false;
}

// =====================================
// 7. FIREBASE TOKEN REFRESH
// =====================================

bool refreshFirebaseToken() {

    if (refreshToken.length() == 0) {
        return firebaseSignup();
    }

    Serial.println("Refreshing Firebase token...");

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;

    String url = String(REFRESH_URL) + API_KEY;

    if (!https.begin(client, url)) {
        Serial.println("Unable to start token refresh request.");
        return false;
    }

    https.addHeader(
        "Content-Type",
        "application/x-www-form-urlencoded"
    );

    String payload =
        "grant_type=refresh_token&refresh_token=" + refreshToken;

    int httpCode = https.POST(payload);

    if (httpCode > 0) {

        String response = https.getString();

        if (httpCode == HTTP_CODE_OK) {

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, response);

            if (!error) {

                idToken = doc["id_token"].as<String>();

                String newRefreshToken =
                    doc["refresh_token"].as<String>();

                if (newRefreshToken.length() > 0) {
                    refreshToken = newRefreshToken;
                }

                unsigned long expiresIn =
                    doc["expires_in"].as<unsigned long>();

                tokenExpiry =
                    millis() + (expiresIn > 60 ? expiresIn - 60 : 1) * 1000UL;

                Serial.println("Firebase token refreshed!");

                https.end();
                return true;
            }
        }

        Serial.printf("Token refresh HTTP error: %d\n", httpCode);
        Serial.println(response);

    } else {

        Serial.printf("Token refresh request failed: %s\n",
                      https.errorToString(httpCode).c_str());
    }

    https.end();
    return false;
}

// =====================================
// 8. CHECK FIREBASE TOKEN
// =====================================

bool ensureFirebaseToken() {

    if (!authInitialized) {
        authInitialized = firebaseSignup();
        return authInitialized;
    }

    if ((long)(millis() - tokenExpiry) >= 0) {
        authInitialized = refreshFirebaseToken();
    }

    return authInitialized;
}

// =====================================
// 9. GENERATE TIMESTAMP
// =====================================

String getTimestamp() {

    time_t now = time(nullptr);

    if (now < 100000) {
        return "";
    }

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    char timestamp[30];

    strftime(
        timestamp,
        sizeof(timestamp),
        "%Y-%m-%dT%H:%M:%SZ",
        &timeinfo
    );

    return String(timestamp);
}

// =====================================
// 10. HTTP POST TO FIREBASE
// =====================================

bool uploadToFirebase(
    bool bmeSuccess,
    float temp,
    float hum,
    float pres,
    bool pmsSuccess,
    int pm1,
    int pm25,
    int pm10
) {

    if (!ensureFirebaseToken()) {
        Serial.println("Firebase authentication unavailable.");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;

    String url =
        String(DATABASE_URL) +
        String(FLOOR_PATH) +
        ".json?auth=" + idToken;

    if (!https.begin(client, url)) {
        Serial.println("Unable to start Firebase POST request.");
        return false;
    }

    https.addHeader("Content-Type", "application/json");

    JsonDocument doc;

    // Include only successfully read sensor values.
    if (bmeSuccess) {
        doc["temperature"] = temp;
        doc["humidity"] = hum;
        doc["pressure"] = pres;
    }

    if (pmsSuccess) {
        doc["pm1"] = pm1;
        doc["pm25"] = pm25;
        doc["pm10"] = pm10;
    }

    String timestamp = getTimestamp();

    if (timestamp.length() > 0) {
        doc["timestamp"] = timestamp;
    } else {
        Serial.println("Warning: NTP time not synchronized.");
    }

    String payload;
    serializeJson(doc, payload);

    Serial.println("Sending HTTP POST...");
    Serial.println(payload);

    int httpCode = https.POST(payload);

    if (httpCode == HTTP_CODE_OK) {

        String response = https.getString();

        Serial.println("HTTP POST successful!");
        Serial.print("Firebase response: ");
        Serial.println(response);

        https.end();
        return true;
    }

    Serial.printf("HTTP POST failed. Code: %d\n", httpCode);

    if (httpCode > 0) {
        Serial.println(https.getString());
    } else {
        Serial.println(https.errorToString(httpCode));
    }

    https.end();
    return false;
}

// =====================================
// 11. SETUP
// =====================================

void setup() {

    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("--- ENVI-SCI NODE 2 (ESP8266) STARTING ---");

    // BME680 INITIALIZATION

    Wire.begin(BME_SDA, BME_SCL);
    Wire.setClock(100000);

    Serial.println("Initializing BME680...");

    bmeInitialized = bme.begin(BME_ADDRESS);

    if (bmeInitialized) {

        Serial.println("BME680 detected at 0x77!");

        bme.setTemperatureOversampling(BME680_OS_8X);
        bme.setHumidityOversampling(BME680_OS_2X);
        bme.setPressureOversampling(BME680_OS_4X);
        bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
        bme.setGasHeater(320, 150);

    } else {

        Serial.println("BME680 initialization FAILED!");
    }

    // PMS5003 INITIALIZATION

    pmsSerial.begin(9600);
    pmsSerial.setTimeout(100);

    Serial.println("PMS5003 SoftwareSerial started.");
    Serial.println("RX = D5, TX = D6");

    // WIFI CONNECTION

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to Wi-Fi");

    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }

    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // TIME SYNCHRONIZATION

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    Serial.println("Synchronizing time...");

    // Wait briefly for NTP, without blocking indefinitely.
    unsigned long ntpStart = millis();

    while (time(nullptr) < 100000 && millis() - ntpStart < 15000) {
        delay(250);
        yield();
    }

    if (time(nullptr) >= 100000) {
        Serial.println("Time synchronized!");
    } else {
        Serial.println("NTP synchronization pending.");
    }

    // FIREBASE AUTHENTICATION

    Serial.println("Connecting to Firebase...");

    authInitialized = firebaseSignup();

    if (authInitialized) {
        Serial.println("Firebase authentication ready!");
    } else {
        Serial.println("Firebase authentication failed.");
    }

    Serial.println("--- SYSTEM ONLINE ---");
}

// =====================================
// 12. MAIN LOOP
// =====================================

void loop() {

    static unsigned long lastTime = 0;

    if (millis() - lastTime >= 5000) {

        lastTime = millis();

        float temp = 0;
        float hum = 0;
        float pres = 0;

        int pm1 = 0;
        int pm25 = 0;
        int pm10 = 0;

        bool bmeSuccess = false;
        bool pmsSuccess = false;

        // BME680 READING

        if (bmeInitialized) {

            if (bme.performReading()) {

                temp = bme.temperature;
                hum = bme.humidity;
                pres = bme.pressure / 100.0;

                bmeSuccess = true;

                Serial.printf(
                    "[BME] T: %.1f C | H: %.1f %% | P: %.1f hPa\n",
                    temp, hum, pres
                );

            } else {

                Serial.println("BME Read Failed!");
            }

        } else {

            Serial.println("BME680 unavailable.");
        }

        // PMS5003 READING

        if (readPMSdata(&pmsSerial)) {

            pm1 = data.pm10_env;
            pm25 = data.pm25_env;
            pm10 = data.pm100_env;

            pmsSuccess = true;

            Serial.printf(
                "[PMS] PM1.0: %d | PM2.5: %d | PM10: %d\n",
                pm1, pm25, pm10
            );

        } else {

            Serial.println("PMS5003: No valid frame received.");
        }

        // HTTP POST UPLOAD

        if (WiFi.status() == WL_CONNECTED) {

            if (bmeSuccess || pmsSuccess) {

                bool uploadSuccess = uploadToFirebase(
                    bmeSuccess,
                    temp,
                    hum,
                    pres,
                    pmsSuccess,
                    pm1,
                    pm25,
                    pm10
                );

                if (uploadSuccess) {
                    Serial.println("New record added to Firebase!");
                } else {
                    Serial.println("Firebase upload failed.");
                }

            } else {

                Serial.println("No valid sensor data to upload.");
            }

        } else {

            Serial.println("Wi-Fi disconnected.");
        }

        Serial.println("-------------------------------------");
    }

    yield();
}

// =====================================
// 13. PMS5003 FRAME READER
// =====================================

boolean readPMSdata(Stream *s) {

    while (s->available() >= 2) {

        if (s->peek() != 0x42) {
            s->read();
            continue;
        }

        uint8_t first = s->read();
        uint8_t second = s->read();

        if (second != 0x4D) {
            continue;
        }

        uint8_t buffer[32];

        buffer[0] = first;
        buffer[1] = second;

        unsigned long start = millis();

        while (s->available() < 30 &&
               millis() - start < 200) {

            delay(1);
            yield();
        }

        if (s->available() < 30) {
            return false;
        }

        size_t received = s->readBytes(
            (char *)(buffer + 2), 30
        );

        if (received != 30) {
            return false;
        }

        uint16_t frameLength = read16(buffer, 2);

        if (frameLength != 28) {
            continue;
        }

        uint16_t calculatedSum = 0;

        for (uint8_t i = 0; i < 30; i++) {
            calculatedSum += buffer[i];
        }

        uint16_t receivedSum = read16(buffer, 30);

        if (calculatedSum != receivedSum) {
            continue;
        }

        data.framelen = read16(buffer, 2);

        data.pm10_standard = read16(buffer, 4);
        data.pm25_standard = read16(buffer, 6);
        data.pm100_standard = read16(buffer, 8);

        data.pm10_env = read16(buffer, 10);
        data.pm25_env = read16(buffer, 12);
        data.pm100_env = read16(buffer, 14);

        data.particles_03um = read16(buffer, 16);
        data.particles_05um = read16(buffer, 18);
        data.particles_10um = read16(buffer, 20);
        data.particles_25um = read16(buffer, 22);
        data.particles_50um = read16(buffer, 24);
        data.particles_100um = read16(buffer, 26);

        data.unused = read16(buffer, 28);
        data.checksum = receivedSum;

        return true;
    }

    return false;
}
