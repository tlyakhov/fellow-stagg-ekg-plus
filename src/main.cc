// For some magic reason, this include needs to come first,
// Otherwise there will be a bunch of stl-related compile errors. Why????
#include "StaggKettle.hh"

#include <Arduino.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <M5Stack.h>
#include <Preferences.h>
#include <BLEDevice.h>

#include "Free_Fonts.hh"
#include "FSRScale.hh"
#include "PIIDefines.hh"

#define ADC_PIN0 35
#define ADC_PIN1 36
#define RANGE_PIN1 16
#define RANGE_PIN2 17
#define BAT_CHK 13

const double fillThreshold = 4.0;

static StaggKettle kettle;
static FirebaseData firebaseData;
static Preferences prefs;
static FSRScale scale(&prefs, ADC_PIN0);

// State tracking for UI
static StaggKettle::State xState = StaggKettle::State::Connected;
static bool xLifted;
static bool xPower;
static byte xCurrentTemp = -1;
static byte xTargetTemp = -1;
//static unsigned int xCountdown = -1;
static double xWeight = -1;
static byte xCalMode = -1;
static bool refreshState = false;
static bool refreshTemps = false;
static bool refreshData = true;
static unsigned long lastDataRefresh = 0;

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

void setup() {
  // Setup the M5 Core
  M5.begin();
  dacWrite(25, 0);  // Disable speaker DAC

  prefs.begin("fellow-stagg", false);

  // pinMode(ADC_PIN0, INPUT)

  // Setup the TFT display
  M5.Lcd.setBrightness(100);
  M5.Lcd.fillScreen(TFT_BLACK);
  // Serial baud
  Serial.begin(115200);
  Serial.println("Starting Fellow Stagg EKG+ bridge application...");
  Serial.println("Connecting to WiFi...");
  WiFi.onEvent(onWiFiEvent);
  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASS);
  BLEDevice::init("");
  Firebase.begin(FIREBASE_PROJECT, FIREBASE_SECRET);
  Firebase.reconnectWiFi(true);
  Firebase.setMaxRetry(firebaseData, 3);
  Firebase.setMaxErrorQueue(firebaseData, 30);

  // Let's scan for a kettle!
  kettle.scan();
}

void drawState() {
  int fh = M5.Lcd.fontHeight(GFXFF);
  int ypos = 120 - fh * 2 - fh / 2;
  M5.Lcd.fillRect(0, ypos, 320, fh, TFT_BLACK);
  if (kettle.getState() == StaggKettle::State::Connected) {
    if (kettle.isLifted()) {
      M5.Lcd.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
      M5.Lcd.drawString("Lifted", 160, ypos, GFXFF);
    } else if (kettle.isOn()) {
      M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
      M5.Lcd.drawString("Heating", 160, ypos, GFXFF);
    } else {
      M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      M5.Lcd.drawString("Off", 160, ypos, GFXFF);
    }
  } else {
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.drawString(StaggKettle::StateStrings[kettle.getState()],
                      160, ypos, GFXFF);
  }
}
void drawTemps() {
  int fh = M5.Lcd.fontHeight(GFXFF);
  int ypos = 120 - fh * 1 - fh / 2;
  M5.Lcd.fillRect(0, ypos, 320, fh, TFT_BLACK);
  if (kettle.getState() != StaggKettle::State::Connected) {
    return;
  }

  if (!kettle.isOn() || kettle.isLifted()) {
    M5.Lcd.setTextColor(TFT_BLUE, TFT_BLACK);
    M5.Lcd.drawString("Current: --- -> " + String(kettle.getTargetTemp()), 160,
                      ypos, GFXFF);
  } else {
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.drawString("Current: " + String(kettle.getCurrentTemp()) + " -> " +
                          String(kettle.getTargetTemp()),
                      160, ypos, GFXFF);
  }
}
void drawScale() {
  int fh = M5.Lcd.fontHeight(GFXFF);
  int ypos = 120 - fh / 2;
  M5.Lcd.fillRect(0, ypos, 320, fh, TFT_BLACK);
  if (scale.getCalibrationMode() == 0) {
    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.drawString("Fill: " + String(scale.getWeight()) + "oz", 160, ypos,
                      GFXFF);
  } else {
    M5.Lcd.setFreeFont(FSSB12);
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.drawString(
        "Fill kettle to exactly " +
            String(Calibration::Ounces[scale.getCalibrationMode() - 1]) +
            "oz, then click button",
        160, ypos, GFXFF);
  }
}

void refreshFirebase() {
  if (WiFi.status() != WL_CONNECTED ||
      kettle.getState() != StaggKettle::State::Connected ||
      kettle.getName().size() == 0)
    return;

  std::string path = std::string("/") + kettle.getName() + "/status";
  Serial.print("Firebase update for ");
  Serial.print(path.c_str());
  Serial.println(" from " + String(WiFi.localIP().toString()));
  
  FirebaseJson json;
  json.add("isOn", kettle.isOn());
  json.add("isLifted", kettle.isLifted());
  json.add("currentTemp", (int)kettle.getCurrentTemp());
  json.add("targetTemp", (int)kettle.getTargetTemp());
  json.add("lastUpdated", String(millis()));
  if(!Firebase.setJSON(firebaseData, path.c_str(), json)) {
    Serial.println("Firebase update failed.");
    Serial.println(firebaseData.errorReason());
  }
}

void loop(void) {
  // update button state
  M5.update();
  kettle.loop();
  scale.loop();

  // State tracking for UI

  refreshState = false;
  refreshTemps = false;
  if (xState != kettle.getState()) {
    xState = kettle.getState();
    refreshState = true;
    refreshTemps = true;
    refreshData = true;
  }
  if (xPower != kettle.isOn()) {
    xPower = kettle.isOn();
    refreshState = true;
    refreshTemps = true;
    refreshData = true;
  }
  if (xLifted != kettle.isLifted()) {
    xLifted = kettle.isLifted();
    refreshState = true;
    refreshTemps = true;
    refreshData = true;
  }
  if (xCurrentTemp != kettle.getCurrentTemp()) {
    xCurrentTemp = kettle.getCurrentTemp();
    refreshTemps = true;
    refreshData = true;
  }
  if (xTargetTemp != kettle.getTargetTemp()) {
    xTargetTemp = kettle.getTargetTemp();
    refreshTemps = true;
    refreshData = true;
  }

  unsigned long timeNow = millis();
  if (timeNow < lastDataRefresh)
    lastDataRefresh = timeNow;

  if(refreshData && timeNow - lastDataRefresh > 5000) {
    refreshFirebase();
    refreshData = false;
    lastDataRefresh = timeNow;
  }

  // UI drawing

  M5.Lcd.setFreeFont(FSSB18);
  M5.Lcd.setTextDatum(TC_DATUM);
  if (refreshState) drawState();
  if (refreshTemps) drawTemps();

  if (xWeight != scale.getWeight() || xCalMode != scale.getCalibrationMode()) {
    xWeight = scale.getWeight();
    xCalMode = scale.getCalibrationMode();
    drawScale();
  }

  // Input checking

  if (M5.BtnA.wasReleased()) {
    // M5.Speaker.beep();
    if (kettle.getState() == StaggKettle::State::Connected &&
        !kettle.isOn()) {
      Serial.println("A - ON!");
      if (true || scale.getWeight() >= fillThreshold) {
        kettle.on();
      } else {
        Serial.println("FILL LEVEL TOO LOW! " + String(scale.getWeight()) +
                       "oz < " + String(fillThreshold) + "oz");
      }

    } else {
      Serial.println("A - OFF!");
      kettle.off();
    }
  } else if (M5.BtnB.wasReleased()) {
    Serial.println("B - SET");
    if(kettle.getTargetTemp() < 210)
      kettle.setTemp(kettle.getTargetTemp() + 5);
    else
      kettle.setTemp(160);
  } else if (M5.BtnC.wasReleased()) {
    Serial.println("C - CALIBRATE");
    scale.nextCalibration();
  }
}
