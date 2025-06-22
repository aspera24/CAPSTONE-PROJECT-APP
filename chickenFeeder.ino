// CHICKEN FEEDER SYSTEM
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <time.h>
#include "HX711.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// WiFi & Firebase credentials
#define WIFI_SSID "ENTER_WIFI_SSID"
#define WIFI_PASSWORD "ENTER_WIFI_PASSWORD"

#define API_KEY "ENTER_API_KEY"
#define DATABASE_URL "ENTER_REALTIME_DB_URL"

// I/O Pins
#define MOTOR_RELAY 18     // Motor relay for chicken feeder - GPIO18
#define WATERPUMP_PIN 19   // Relay pin for water pump - GPIO19
#define DOUT 21            // Connect to HX711 DOUT
#define CLK 22             // Connect to HX711 SCK
#define RELAY_FORWARD 25   // Example pin for actuator forward
#define RELAY_BACKWARD 26  // Example pin for actuator backward
#define LCD_SDA 16
#define LCD_SCL 4
#define USE_LCD false

HX711 scale;
LiquidCrystal_I2C lcd(0x27, 16, 2);  //Download Zip Library with this link: https://github.com/johnrickman/LiquidCrystal_I2C/archive/refs/heads/master.zip

float calibration_factor = -123822;

// Constants
#define CHECK_INTERVAL 500   // Interval to check Firebase control buttons
#define PUMP_DURATION 10000  // Water pump will run for 10 seconds

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// State variables
bool signupOK = false;
bool isFeeding = false;
bool isWaterPumping = false;
unsigned long feedEnd = 0;
unsigned long pumpStart = 0;
unsigned long lastCheck = 0;
unsigned long lastScheduleFeedTime = 0;
String lastMode = "";
int lastTarget = 0;

void setup() {
  Serial.begin(115200);
  pinMode(MOTOR_RELAY, OUTPUT);
  pinMode(WATERPUMP_PIN, OUTPUT);
  digitalWrite(MOTOR_RELAY, LOW);
  digitalWrite(WATERPUMP_PIN, LOW);
  scale.begin(DOUT, CLK);
  scale.set_scale(calibration_factor);
  scale.tare();  // Reset the scale to 0

  connectWiFi();         // Connect to WiFi
  initializeFirebase();  // Set up Firebase connection
  syncTimeWithNTP();     // Sync time via NTP

  if (USE_LCD) {
    Wire.begin(LCD_SDA, LCD_SCL);
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Chicken Feeder");
    lcd.setCursor(0, 1);
    lcd.print("Starting...");
  }
}

void loop() {
  unsigned long nowMillis = millis();

  if (Firebase.ready() && signupOK && (nowMillis - lastCheck >= CHECK_INTERVAL)) {
    lastCheck = nowMillis;
    handleManualFeeding();  // Check if manual feed is triggered
    handleWaterPump();      // Check if water pump button is triggered
  }

  checkScheduleFeeding();  // Check if scheduled feeding matches current time
}

// --- Connect to WiFi ---
void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
}

// --- Initialize Firebase Connection ---
void initializeFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.time_zone = 8.0;

  if (Firebase.signUp(&config, &auth, "", "")) {
    signupOK = true;
    Serial.println("Firebase sign up success.");
  } else {
    Serial.printf("Sign up failed: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}


// --- Sync time with NTP server ---
void syncTimeWithNTP() {
  configTime(28800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nNTP time acquired.");
}

// --- Handle manual feeding trigger ---
void handleManualFeeding() {
  if (Firebase.RTDB.getBool(&fbdo, "control/push_button") && fbdo.boolData()) {
    Serial.println("Manual feed button pressed.");
    if (Firebase.RTDB.getFloat(&fbdo, "control/target_weight")) {
      float targetWeight = fbdo.floatData();
      float currentWeight = scale.get_units(5);

      Serial.println("Manual feeding triggered...");
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Manual Feed");
      lcd.setCursor(0, 1);
      lcd.print("Target: " + String(targetWeight, 1) + "kg");
      delay(1000);  // short pause for user to see the display

      if (currentWeight < targetWeight) {
        startFeedingToWeight(targetWeight, "manual");
      } else {
        Serial.println("Target weight already reached.");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Target Already");
        lcd.setCursor(0, 1);
        lcd.print("Reached");
        Firebase.RTDB.setBool(&fbdo, "control/push_button", false);
        delay(1000);  // display message before clearing again
        lcd.clear();
      }
    }
  }
}

void extendActuator() {
  Serial.println("Extending actuator...");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Extending Tray");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...");

  digitalWrite(RELAY_BACKWARD, LOW);  // Ensure backward is off
  digitalWrite(RELAY_FORWARD, HIGH);
  delay(5000);  // Adjust this timing based on your actuator
  digitalWrite(RELAY_FORWARD, LOW);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Tray Extended");
  delay(500);
  lcd.clear();
}

void retractActuator() {
  Serial.println("Retracting actuator...");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Retracting Tray");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...");

  digitalWrite(RELAY_FORWARD, LOW);  // Ensure forward is off
  digitalWrite(RELAY_BACKWARD, HIGH);
  delay(5000);  // Adjust this timing based on your actuator
  digitalWrite(RELAY_BACKWARD, LOW);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Tray Retracted");
  delay(500);
  lcd.clear();
}

// --- Start Feeding based on weight ---
void startFeedingToWeight(float targetWeight, String mode) {
  float initialWeight = scale.get_units(10);
  isFeeding = true;
  lastMode = mode;  // <- i-save ang mode (manual or scheduled)
  digitalWrite(MOTOR_RELAY, HIGH);

  float currentWeight = 0;

  Serial.printf("Starting %s feeding to %.2f kg...\n", mode.c_str(), targetWeight);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Feeding: " + mode);
  lcd.setCursor(0, 1);
  lcd.print("Target: " + String(targetWeight, 1) + "kg");

  while (true) {
    currentWeight = scale.get_units(3) - initialWeight;
    Serial.printf("Current Feed: %.2f kg / Target: %.2f kg\n", currentWeight, targetWeight);

    lcd.setCursor(0, 0);
    lcd.print("Feeding...");
    lcd.setCursor(0, 1);
    lcd.print("Now: " + String(currentWeight, 2) + "kg");

    if (currentWeight >= targetWeight - 0.05) {  // with small tolerance
      Serial.println("Target weight reached. Stopping motor.");
      break;
    }

    delay(200);
  }

  digitalWrite(MOTOR_RELAY, LOW);
  isFeeding = false;
  lastTarget = targetWeight;

  Serial.printf("Final weight gain: %.2f kg\n", currentWeight);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Done Feeding");
  lcd.setCursor(0, 1);
  lcd.print("Total: " + String(currentWeight, 2) + "kg");
  delay(500);

  extendActuator();
  retractActuator();

  Firebase.RTDB.setBool(&fbdo, "control/push_button", false);
  recordFeedingEvent();
}

// --- Record Feeding Event to Firebase ---
void recordFeedingEvent() {
  Serial.println("Recording feeding to Firebase...");

  time_t now = time(nullptr);
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  char timeString[30];
  strftime(timeString, sizeof(timeString), "%d %b %Y %H:%M:%S", &timeinfo);
  String timeStr(timeString);

  FirebaseJson record;
  record.set("time_epoch", now);
  record.set("time_string", timeStr);
  record.set("mode", lastMode);
  record.set("target_weight", lastTarget);
  record.set("status", "Success");

  if (Firebase.RTDB.pushJSON(&fbdo, "feeding_records", &record)) {
    String key = fbdo.pushName();
    Firebase.RTDB.setString(&fbdo, "feeding_records/" + key + "/key", key);
    Serial.println("Feeding record pushed to Firebase.");
  } else {
    Serial.printf("Error pushing record: %s\n", fbdo.errorReason().c_str());
  }
}


// --- Handle water pump logic ---
void handleWaterPump() {
  if (!isWaterPumping) {
    if (Firebase.RTDB.getBool(&fbdo, "waterpump/push_button") && fbdo.boolData()) {
      Serial.println("Water pump button pressed.");
      digitalWrite(WATERPUMP_PIN, HIGH);
      isWaterPumping = true;
      pumpStart = millis();
      Firebase.RTDB.setBool(&fbdo, "waterpump/push_button", false);

      if (USE_LCD) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Water Pump ON");
      }
    }
  } else if (millis() - pumpStart >= PUMP_DURATION) {
    digitalWrite(WATERPUMP_PIN, LOW);
    isWaterPumping = false;
    Serial.println("Water pump stopped.");

    if (USE_LCD) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Water Pump OFF");
    }
  }
}


// --- Check for scheduled feeding ---
void checkScheduleFeeding() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int hour = timeinfo.tm_hour;
  int minute = timeinfo.tm_min;
  int weekday = timeinfo.tm_wday;
  unsigned long nowMinute = hour * 60 + minute;

  if (nowMinute == lastScheduleFeedTime) return;

  if (Firebase.RTDB.getJSON(&fbdo, "schedule")) {
    FirebaseJson& schedules = fbdo.jsonObject();
    FirebaseJsonData data;

    size_t count = schedules.iteratorBegin();

    for (size_t i = 0; i < count; i++) {
      yield();  // avoid WDT reset
      int type;
      String key, value;
      schedules.iteratorGet(i, type, key, value);

      FirebaseJsonData schedData;
      schedules.get(schedData, key);
      if (!schedData.success || schedData.type != "object") continue;

      FirebaseJson schedJson;
      schedJson.setJsonData(schedData.stringValue);

      // Extract values
      bool enabled = false;
      int schedHour = -1, schedMinute = -1;
      float target = -1;
      schedJson.get(data, "enabled");
      if (data.success) enabled = data.boolValue;

      schedJson.get(data, "hour");
      if (data.success) schedHour = data.intValue;

      schedJson.get(data, "minute");
      if (data.success) schedMinute = data.intValue;

      if (schedJson.get(data, "target_weight") && data.success) {
        target = data.intValue;
      }

      schedJson.get(data, "days");

      if (data.success && data.type == "string") {
        String days = data.stringValue;
        const char* weekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        String today = weekdays[weekday];

        if (enabled && schedHour == hour && schedMinute == minute) {
          if (days.indexOf(today) >= 0) {
            Serial.printf("Schedule matched. Feeding for %2fkg\n", target);
            lastScheduleFeedTime = nowMinute;
            startFeedingToWeight(target, "scheduled");
          }
        }
      }
    }
    schedules.iteratorEnd();
  }
}
