#include "StaggKettle.hh"
#include <M5Stack.h>

// Friendly names of states.
 const char* StaggKettle::StateStrings[] = {"Inactive", "Scanning...", "Found",
                                       "Connecting...", "Connected"};

// Lots of magic numbers reverse engineered from BLE traffic capture:

// UUID of the remote service we wish to connect to.
// This is for the Fellow Stagg EKG+ SPS (serial over BLE) service.
static BLEUUID ekgServiceUUID("00001820-0000-1000-8000-00805f9b34fb");

// UUID of the characteristic for the serial channel. This shows up in wireshark
// as "Internet Protocol Support: Age"
static BLEUUID ekgCharUUID("00002A80-0000-1000-8000-00805f9b34fb");

// All Fellow Stagg EKG+ comms (both rx & tx) start with 0xefdd.
// This appears to be a command to tell the kettle the client knows how to talk
// to it, seems to be completely a magic number.
static uint8_t ekgInit[20] = {0xef, 0xdd, 0x0b, 0x30, 0x31, 0x32, 0x33,
                              0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
                              0x31, 0x32, 0x33, 0x34, 0x9a, 0x6d};

// Don't send commands more often than every X ms.
const unsigned long debounceDelay = 200;

static std::unordered_map<BLERemoteCharacteristic*, StaggKettle*> notifiers;

// static wrapper for onNotify member callback.
static void bleNotify(BLERemoteCharacteristic* c, uint8_t* pData, size_t length,
                      bool isNotify) {
  if (notifiers.count(c) == 0) {
    Serial.println("<bleNotify> No notifier for this characteristic!");
    return;
  }
  notifiers[c]->onNotify(c, pData, length, isNotify);
}

StaggKettle::StaggKettle()
    : state(StaggKettle::State::Inactive), timeLastCommand(millis()) {}

StaggKettle::~StaggKettle() {
  std::unordered_map<BLERemoteCharacteristic*, StaggKettle*>::iterator it;

  for (it = notifiers.begin(); it != notifiers.end(); ++it) {
    if (it->second == this) {
      notifiers.erase(it->first);
      break;
    }
  }
}

void StaggKettle::scan() {
  Serial.println("<StaggKettle::scan> Scanning...");

  state = StaggKettle::State::Scanning;
  timeStateChange = millis();

  // Retrieve a Scanner and set the callback we want to use to be informed when
  // we have detected a new device.  Specify that we want active scanning and
  // start the scan to run for 5 seconds.
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(this);
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
}

void StaggKettle::onConnect(BLEClient* pclient) {
  Serial.print("<StaggKettle::onConnect> Device ");
  Serial.println(pDevice->getName().c_str());
  state = StaggKettle::State::Connected;
  timeStateChange = millis();
  sequence = 0;
}

void StaggKettle::onDisconnect(BLEClient* pclient) {
  Serial.print("<StaggKettle::onDisconnect> Device ");
  Serial.println(pDevice->getName().c_str());
  state = StaggKettle::State::Inactive;
  timeStateChange = millis();
}

// Called by BLE when a device has been found during a scan.
void StaggKettle::onResult(BLEAdvertisedDevice advertiser) {
  Serial.print("<StaggKettle::onResult> BLE Advertised Device found: ");
  Serial.println(advertiser.toString().c_str());

  // Does this device provide the service for our kettle?
  if (advertiser.haveServiceUUID() &&
      advertiser.isAdvertisingService(ekgServiceUUID)) {
    BLEDevice::getScan()->stop();
    pDevice = new BLEAdvertisedDevice(advertiser);
    state = StaggKettle::State::Found;
    timeStateChange = millis();
  }
}

bool StaggKettle::connectToServer() {
  state = StaggKettle::State::Connecting;
  timeStateChange = millis();

  Serial.print("<StaggKettle::connectToServer> Connecting to BLE device ");
  Serial.println(pDevice->getName().c_str());

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(this);
  Serial.println("<StaggKettle::connectToServer> Created BLE client");

  // Connect to the remove BLE Server.
  // if we pass a BLEAdvertisedDevice instead of address,
  // it will be recognized as a peer device address (public or private)
  if (!pClient->connect(pDevice)) {
    Serial.println("<StaggKettle::connectToServer> Failed to connect.");
    return false;
  }

  // Obtain a reference to the service we are after in the remote BLE server.
  pRemoteService = pClient->getService(ekgServiceUUID);
  if (pRemoteService == nullptr) {
    Serial.print(
        "<StaggKettle::connectToServer> Failed to find EKG+ service UUID: ");
    Serial.println(ekgServiceUUID.toString().c_str());
    pClient->disconnect();
    state = StaggKettle::State::Inactive;
    timeStateChange = millis();

    return false;
  }
  Serial.println("<StaggKettle::connectToServer> Found EKG+ service UUID");

  // Obtain a reference to the characteristic in the service of the remote BLE
  // server.
  prcKettleSerial = pRemoteService->getCharacteristic(ekgCharUUID);
  if (prcKettleSerial == nullptr) {
    Serial.print(
        "<StaggKettle::connectToServer> Failed to find EKG+ SPS characteristic "
        "UUID: ");
    Serial.println(ekgCharUUID.toString().c_str());
    pClient->disconnect();
    state = StaggKettle::State::Inactive;
    timeStateChange = millis();

    return false;
  }

  Serial.println(
      "<StaggKettle::connectToServer> Found EKG+ SPS characteristic UUID");

  bufferState = 0;
  if (prcKettleSerial->canNotify()) {
    notifiers[prcKettleSerial] = this;
    prcKettleSerial->registerForNotify(bleNotify);
  }
  return true;
}

void StaggKettle::parseEvent(const uint8_t* data, size_t length) {
  switch (data[0]) {
    case 0:  // Power
      if (data[1] == 1) {
        power = true;
        //Serial.println("<StaggKettle::parseEvent> On");
      } else if (data[1] == 0) {
        power = false;
        //Serial.println("<StaggKettle::parseEvent> Off");
      } else {
        Serial.println("<StaggKettle::parseEvent> Power unknown state " +
                       String(data[1]));
      }
      break;
    case 2:  // Target temperature
      targetTemp = data[1];
      //Serial.println("<StaggKettle::parseEvent> Target " + String(targetTemp));
      break;
    case 3:  // Current temperature
      currentTemp = data[1];
      //Serial.println("<StaggKettle::parseEvent> Current " +
      //               String(currentTemp));
      break;
    case 4:  // Countdown when lifted?
      countdown = data[1];
      // Serial.println("<StaggKettle::parseEvent> Countdown " +
      // String(countdown));
      break;
    case 8:  // Kettle lifted
      if (data[1] == 0) {
        // Serial.println("<StaggKettle::parseEvent> Kettle lifted!");
        lifted = true;
      } else if (data[1] == 1) {
        // Serial.println("<StaggKettle::parseEvent> Kettle on base.");
        lifted = false;
      } else {
        Serial.println("<StaggKettle::parseEvent> Lifting unknown state " +
                       String(data[1]));
      }
      break;
    case 1:  // Unknown, usually 0x01, 0x00, 0x00, 0x00
    case 5:  // Unknown, usually 0x05, 0xFF, 0xFF, 0xFF, 0xFF
    case 7:  // Unknown, usually 0x07, 0x00, 0x00, 0x00
    case 6:  // Unknown, usually 0x06, 0x00, 0x00
    default:
      if (unknownStates.count(data[0]) != 0 &&
          memcmp(data, unknownStates[data[0]], length) == 0)
        break;
      else if (unknownStates.count(data[0]) == 0)
        unknownStates[data[0]] = new uint8_t[16];

      memcpy(unknownStates[data[0]], data, length);
      Serial.print("<StaggKettle::parseEvent> Unknown state change: ");
      for (int i = 0; i < length; i++) {
        Serial.print(String(data[i], HEX) + " ");
      }
      Serial.println("END");
      break;
  }
}

void StaggKettle::onNotify(BLERemoteCharacteristic* c, uint8_t* pData,
                           size_t length, bool isNotify) {
  mtxState.lock();
  if (state != StaggKettle::State::Connected) {
    mtxState.unlock();
    return;
  }

  for (int i = 0; i < length; i++) {
    if (bufferState == 0 && pData[i] == 0xef) {
      bufferState = 1;
      bufferPos = 0;
      continue;
    } else if (bufferState == 1 && pData[i] == 0xdd) {
      bufferState = 2;
      bufferPos = 0;
      continue;
    } else if (bufferState == 2) {
      buffer[bufferPos] = pData[i];
      if (bufferPos >= 255) {
        bufferState = 0;
        bufferPos = 0;
        continue;
      }
      if (i + 1 < length && pData[i] == 0xef && pData[i + 1] == 0xdd) {
        if (bufferPos > 1) this->parseEvent(buffer, bufferPos - 1);
        bufferPos = 0;
        bufferState = 1;
      } else {
        bufferPos++;
      }
    }
  }
  mtxState.unlock();
}

void StaggKettle::sendCommand(StaggKettle::Command cmd) {
  if (state != StaggKettle::State::Connected) {
    Serial.println("<StaggKettle::sendCommand> Not connected, returning.");
    return;
  }

  Serial.println(String("<StaggKettle::sendCommand> ") + String(cmd));
  uint8_t buf[8];
  buf[0] = 0xef; buf[1] = 0xdd; // Magic, packet start
  buf[2] = 0x0a; // Command flag?
  buf[3] = sequence;
  switch (cmd) {
    case StaggKettle::Command::On:
      buf[4] = 0x00; // Power
      buf[5] = 0x01; // On
      break;
    case StaggKettle::Command::Off:
      buf[4] = 0x00; // Power
      buf[5] = 0x00; // Off
      break;
    case StaggKettle::Command::Set:
      buf[4] = 0x01; // Temp
      buf[5] = userTemp;
      break;
    default:
      return;
  }
  buf[6] = buf[3] + buf[5]; // Checksum
  buf[7] = buf[4]; // Checksum?
  prcKettleSerial->writeValue(buf, 8);
  sequence++;
}

void StaggKettle::setTemp(byte temp) {
  userTemp = temp;
  if (userTemp > 212) userTemp = 212;
  if (userTemp < 160) userTemp = 160;
  qCommands.push(StaggKettle::Command::Set);
}

void StaggKettle::loop() {
  mtxState.lock();
  unsigned long timeNow = millis();

  // Handle 64bit wraparound
  if (timeNow < timeLastCommand) {
    timeLastCommand = timeNow;
  }
  if (timeNow < timeStateChange) {
    timeStateChange = timeNow;
  }

  StaggKettle::Command cmd;
  switch (state) {
    case StaggKettle::State::Inactive:
      scan();
      break;
    case StaggKettle::State::Scanning:
      if (timeNow - timeStateChange < 15000) break;
      if (pBLEScan != nullptr) pBLEScan->stop();
      state = StaggKettle::State::Inactive;
      timeStateChange = timeNow;
      break;
    case StaggKettle::State::Found:
      if (connectToServer()) {
        Serial.println(
            "<StaggKettle::loop> Connected to kettle, initializing...");
        timeLastCommand = timeNow;
        prcKettleSerial->writeValue(ekgInit, 20);
      }
      break;
    case StaggKettle::State::Connected:
      if (timeNow - timeLastCommand < debounceDelay || qCommands.empty()) break;
      cmd = qCommands.front();
      qCommands.pop();
      sendCommand(cmd);
      timeLastCommand = debounceDelay;
      break;
    default:
      break;
  }
  mtxState.unlock();
}
