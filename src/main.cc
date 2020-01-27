// For some magic reason, this include needs to come first,
// Otherwise there will be a bunch of stl-related compile errors. Why????
#include "StaggKettle.hh"

#include <Arduino.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <Adafruit_SSD1306.h>

#include "FSRScale.hh"
#include "PIIDefinesExample.hh"



const int fillThreshold = 3.0;
const unsigned long firebaseStateInterval = 5000;
const unsigned long firebasePollInterval = 3000;

// For an SSD1306 display connected to I2C (SDA, SCL pins)
const uint8_t ScreenWidth = 128;
const uint8_t ScreenHeight = 64;
const int8_t ScreenResetPin = -1; // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(ScreenWidth, ScreenHeight, &Wire, ScreenResetPin);

static StaggKettle kettle;
static FirebaseData firebaseData;
static Preferences prefs;
static FSRScale scale(32);
static FirebaseJson json;

// State tracking for UI
static StaggKettle::State xState = StaggKettle::State::Connected;
static bool xLifted;
static bool xPower;
static byte xCurrentTemp = -1;
static byte xTargetTemp = -1;
//static unsigned int xCountdown = -1;
static int xFill = -1;
static byte xCalMode = -1;
static bool refreshState = false;
static bool refreshTemps = false;
static bool refreshFirebaseState = true;
static unsigned long lastFirebaseStateRefresh = 0;
static unsigned long lastFirebasePoll = 0;
static unsigned long lastHeapDebug = 0;

void onWiFiEvent(WiFiEvent_t event)
{
  switch (event) {
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.println("<onWiFiEvent> Got IP!");
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("<onWiFiEvent> Disconnected!");
      break;
    default:
      break;
  }
}

void setupWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_MODE_STA);
  WiFi.onEvent(onWiFiEvent);
  WiFi.config(HOME_WIFI_IP, HOME_WIFI_GATEWAY, HOME_WIFI_SUBNET, HOME_WIFI_DNS);
  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASS);
  Firebase.begin(FIREBASE_PROJECT, FIREBASE_SECRET);
  Firebase.reconnectWiFi(true);
  Firebase.setMaxRetry(firebaseData, 3);
  Firebase.setMaxErrorQueue(firebaseData, 15);
  Firebase.setwriteSizeLimit(firebaseData, "tiny");
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Fellow Stagg EKG+ bridge application...");
  // Init display
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println("SSD1306 allocation failed");
  }
  // Show initial display buffer contents on the screen --
  // the library initializes this with an Adafruit splash screen.
  display.display();
  // Init scale
  // scale.loadFromPrefs();
  // Init bluetooth
  BLEDevice::init("");
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  // Init wifi
  setupWiFi();
  // Let's scan for a kettle!
  kettle.scan();
}

void updateFirebaseState() {
  if (!WiFi.isConnected() ||
      kettle.getState() != StaggKettle::State::Connected ||
      kettle.getName().size() == 0)
    return;

  std::string path = std::string("/") + kettle.getName() + "/status";
  Serial.print("Firebase update for ");
  Serial.print(path.c_str());
  Serial.println(" from " + String(WiFi.localIP().toString()));
  
  json.clear();
  json.add("isOn", kettle.isOn());
  json.add("isLifted", kettle.isLifted());
  json.add("isHold", kettle.isHold());
  json.add("currentTemp", (int)kettle.getCurrentTemp());
  json.add("targetTemp", (int)kettle.getTargetTemp());
  json.add("units", (int)kettle.getUnits());
  json.add("fill", scale.getFill());
  json.add("lastUpdated", String(millis()));
  if(!Firebase.setJSON(firebaseData, path.c_str(), json)) {
    Serial.println("Firebase update failed.");
    Serial.println(firebaseData.errorReason());
  }
}

void pollFirebase() {
   if (!WiFi.isConnected() ||
      kettle.getState() != StaggKettle::State::Connected ||
      kettle.getName().size() == 0)
    return;

  std::string path = std::string("/") + kettle.getName() + "/command";
  Serial.print("Polling Firebase ");
  Serial.print(path.c_str());
  Serial.println(" from " + String(WiFi.localIP().toString()));
  if (!Firebase.pathExist(firebaseData, path.c_str()))
    return;

  if(!Firebase.getJSON(firebaseData, path.c_str())) {
    Serial.println("Firebase polling failed.");
    Serial.println(firebaseData.errorReason());
    return;
  }
  FirebaseJson &result = firebaseData.jsonObject();
  FirebaseJsonData data;
  if(result.get(data, "off")) {
    kettle.off();
  } else if(result.get(data, "on")) {
    if (scale.getFill() >= fillThreshold) {
      kettle.on();
    } else {
      Serial.println("FILL LEVEL TOO LOW! " + String(scale.getFill()) +
                       "oz < " + String(fillThreshold) + "oz");
    }
  } else if (result.get(data, "calibrate")) {
    Serial.println("Calibrate");
    scale.nextCalibration();
  } else if(result.get(data, "temp")) {
    if(result.get(data, "value"))
      kettle.setTemp((byte)data.intValue);
  }
  Firebase.deleteNode(firebaseData, path.c_str());
}

void drawScale() {
  int fh = 10;
  int ypos = 32 - fh / 2;
  display.setCursor(32, ypos);
  if (scale.getCalibrationMode() == 0) {
    display.setTextColor(SSD1306_WHITE);
    display.println("Fill: " + String(scale.getFill()) + "oz");
  } else {
    display.setTextColor(SSD1306_WHITE);
    display.println(
        "Fill kettle to exactly " +
            String(Calibration::Ounces[scale.getCalibrationMode() - 1]) +
            "oz, then click button");
  }
}

void loop(void) {
  kettle.loop();
  scale.loop();

  // State tracking for UI

  refreshState = false;
  refreshTemps = false;
  if (xState != kettle.getState()) {
    xState = kettle.getState();
    refreshState = true;
    refreshTemps = true;
    refreshFirebaseState = true;
  }
  if (xPower != kettle.isOn()) {
    xPower = kettle.isOn();
    refreshState = true;
    refreshTemps = true;
    refreshFirebaseState = true;
  }
  if (xLifted != kettle.isLifted()) {
    xLifted = kettle.isLifted();
    refreshState = true;
    refreshTemps = true;
    refreshFirebaseState = true;
  }
  if (xCurrentTemp != kettle.getCurrentTemp()) {
    xCurrentTemp = kettle.getCurrentTemp();
    refreshTemps = true;
    refreshFirebaseState = true;
  }
  if (xTargetTemp != kettle.getTargetTemp()) {
    xTargetTemp = kettle.getTargetTemp();
    refreshTemps = true;
    refreshFirebaseState = true;
  }

  if (xFill != scale.getFill() || xCalMode != scale.getCalibrationMode()) {
    xFill = scale.getFill();
    xCalMode = scale.getCalibrationMode();
    refreshFirebaseState = true;
    display.clearDisplay();
    drawScale();
    display.display();
  }

  unsigned long timeNow = millis();
  // Handle 64 bit wraparound
  if (timeNow < lastFirebaseStateRefresh)
    lastFirebaseStateRefresh = timeNow;
  if (timeNow < lastFirebasePoll)
    lastFirebasePoll = timeNow;
  if (timeNow < lastHeapDebug)
    lastHeapDebug = timeNow;  

  if(refreshFirebaseState &&
     timeNow - lastFirebaseStateRefresh > firebaseStateInterval) {
    updateFirebaseState();
    refreshFirebaseState = false;
    lastFirebaseStateRefresh = timeNow;
  }

  if (timeNow - lastFirebasePoll > firebasePollInterval) {
    pollFirebase();
    lastFirebasePoll = timeNow;
  }

  if (timeNow - lastHeapDebug > 10000) {
    Serial.print("Free heap: ");
    Serial.println(ESP.getFreeHeap());
    lastHeapDebug = timeNow;
  }


}
