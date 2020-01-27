#include "StaggKettle.hh"

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

static const uint8_t ekgStates = 9;
static const uint8_t ekgStateBytes[ekgStates] = {
    3, // 0 = Power
    3, // 1 = Hold
    4, // 2 = Target temperature
    4, // 3 = Current temperature
    4, // 4 = Countdown when lifted?
    4, // 5 = Unknown, usually 0x05, 0xFF, 0xFF, 0xFF
    3, // 6 = Boiled/Holding?, usually 0x06, 0x00, 0x00
    3, // 7 = Unknown, usually 0x07, 0x00, 0x00
    3  // 8 = Kettle lifted
    };

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
  Serial.println(name.c_str());
  state = StaggKettle::State::Connected;
  timeStateChange = millis();
  sequence = 0;
}

void StaggKettle::onDisconnect(BLEClient* pclient) {
  Serial.print("<StaggKettle::onDisconnect> Device ");
  Serial.println(name.c_str());
  state = StaggKettle::State::Inactive;
  timeStateChange = millis();
  if (pRemoteService != nullptr) {
    delete pRemoteService;
    pRemoteService = nullptr;
  }
}

// Called by BLE when a device has been found during a scan.
void StaggKettle::onResult(BLEAdvertisedDevice advertiser) {
  Serial.print("<StaggKettle::onResult> BLE Advertised Device found: ");
  Serial.print(advertiser.getName().c_str());
  Serial.print(" - ");
  Serial.println(advertiser.getAddress().toString().c_str());

  // Does this device provide the service for our kettle?
  if (advertiser.haveServiceUUID() &&
      advertiser.isAdvertisingService(ekgServiceUUID)) {
    pBLEScan->stop();
    pDevice = new BLEAdvertisedDevice(advertiser);
    pBLEScan->clearResults();
    state = StaggKettle::State::Found;
    timeStateChange = millis();
  }
}

bool StaggKettle::connectToServer() {
  state = StaggKettle::State::Connecting;
  timeStateChange = millis();

  Serial.print("<StaggKettle::connectToServer> Connecting to BLE device ");
  name.assign(pDevice->getName());
  Serial.println(name.c_str());

  if (pClient != nullptr) {
    delete pClient;
  }
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
    delete pClient;
    pClient = nullptr;
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
    delete pClient;
    pClient = nullptr;    
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

void StaggKettle::parseEvent(const uint8_t* data, size_t length, bool debug) {
  if (data[0] >= ekgStates || ekgStateBytes[data[0]] != length) {
    Serial.print("<StaggKettle::parseEvent> Wrong state length or type: ");    
    for (int i = 0; i < length; i++) {
      Serial.print(String(data[i], HEX));
      Serial.print(" ");
    }
    Serial.println("END");
  }

  switch (data[0]) {
    case 0:  // Power (length 3)
      if (data[1] == 1) {
        power = true;
        if (debug)
          Serial.println("<StaggKettle::parseEvent> On");
      } else if (data[1] == 0) {
        power = false;
        if (debug)
          Serial.println("<StaggKettle::parseEvent> Off");
      } else {
        Serial.print("<StaggKettle::parseEvent> Power unknown state ");
        Serial.println(data[1]);
      }
      break;
    case 1:  // Hold (length 3)
      if (data[1] == 1) {
        hold = true;
        if (debug)
          Serial.println("<StaggKettle::parseEvent> Hold [on]");
      }
      else if(data[1] == 0) {
        hold = false;
        if (debug)
          Serial.println("<StaggKettle::parseEvent> Hold [off]");
      }
      else {
        Serial.print("<StaggKettle::parseEvent> Hold unknown state ");
        Serial.println(data[1]);
      }
      break;
    case 2:  // Target temperature (length 4)
      targetTemp = data[1];
      units = data[2] == 1 ? TempUnits::Fahrenheit : TempUnits::Celsius;
      if (debug) {
        Serial.print("<StaggKettle::parseEvent> Target ");
        Serial.print(String(targetTemp));
        Serial.println(units == TempUnits::Fahrenheit ? "F" : "C");        
      }
      break;
    case 3:  // Current temperature (length 4)
      currentTemp = data[1];
      units = data[2] == 1 ? TempUnits::Fahrenheit : TempUnits::Celsius;      
      if (debug) {
        Serial.print("<StaggKettle::parseEvent> Current ");
        Serial.print(String(currentTemp));
        Serial.println(units == TempUnits::Fahrenheit ? "F" : "C");
      }
      break;
    case 4:  // Countdown when lifted? (length 4)
      countdown = data[1];
      if(debug) {
        Serial.print("<StaggKettle::parseEvent> Countdown ");
        Serial.println(String(countdown));
      }
      break;
    case 8:  // Kettle lifted (length 3)
      if (data[1] == 0) {
        if(debug)
          Serial.println("<StaggKettle::parseEvent> Kettle lifted!");
        lifted = true;
      } else if (data[1] == 1) {
        if(debug)
          Serial.println("<StaggKettle::parseEvent> Kettle on base.");
        lifted = false;
      } else {
        Serial.print("<StaggKettle::parseEvent> Lifting unknown state ");
        Serial.println(data[1]);
      }
      break;
    case 5:  // Unknown (length 4), usually 0x05, 0xFF, 0xFF, 0xFF
    case 6:  // Unknown (length 3), usually 0x06, 0x00, 0x00
      // This may be a "kettle has boiled" signal, or "kettle holding" signal.
    case 7:  // Unknown (length 3), usually 0x07, 0x00, 0x00
    default:
      if (unknownStates.count(data[0]) != 0 &&
          memcmp(data, unknownStates[data[0]], length) == 0)
        break;
      else if (unknownStates.count(data[0]) == 0)
        unknownStates[data[0]] = new uint8_t[16];

      memcpy(unknownStates[data[0]], data, length);
      Serial.print("<StaggKettle::parseEvent> Unknown state change: ");
      for (int i = 0; i < length; i++) {
        Serial.print(String(data[i], HEX));
        Serial.print(" ");
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

  // Pattern recognizer that expects frames of the form:
  // 0xefdd followed by some bytes. ekgStateBytes holds the number of bytes for
  // each state we expect. Some complicated logic here to parse frames as soon
  // as we get them, and also skip frames in case we get fragments or bad data.
  for (int i = 0; i < length; i++) {
    // Look for the first frame separator byte.
    if (bufferState == 0 && pData[i] == 0xef) {
      bufferState = 1;
      bufferPos = 0;
      continue;
    // Look for the second frame separator byte.  
    } else if (bufferState == 1 && pData[i] == 0xdd) {
      bufferState = 2;
      bufferPos = 0;
      continue;
    // The rest are data bytes.  
    } else if (bufferState == 2) {
      buffer[bufferPos] = pData[i];
      // If we have at least one byte, we know the type of state frame that we
      // got, so check if it's in range of the states we know about, and if so,
      // if we have that number of bytes, we have a complete frame, so parse it!
      if (bufferPos > 0 && buffer[0] < ekgStates &&
          bufferPos + 1 >= ekgStateBytes[buffer[0]]) {
        this->parseEvent(buffer, bufferPos + 1, false);
        bufferPos = 0;
        bufferState = 1;
      // Some weirdly long frame, probably something wrong, skip it.
      } else if (bufferPos >= 63) {
        bufferState = 0;
        bufferPos = 0;
        continue;
      // If we see 0xef, peek forward and see if we have a frame separator. If
      // we do, then attempt to parse what we got and move on to the next frame.
      // Shouldn't hit this block unless something is wrong.
      } else if (i + 1 < length && pData[i] == 0xef && pData[i + 1] == 0xdd) {
        if (bufferPos > 1) this->parseEvent(buffer, bufferPos - 1, false);
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
  buf[0] = 0xef; buf[1] = 0xdd; // Magic, frame start
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
  if (units == TempUnits::Fahrenheit) {
    if (userTemp > 212) userTemp = 212;
    if (userTemp < 160) userTemp = 160;
  } else {
    if (userTemp > 100) userTemp = 100;
    if (userTemp < 65) userTemp = 65;
  }
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
      if (timeNow - timeStateChange < StaggKettle::RetryDelay) break;
      scan();
      break;
    case StaggKettle::State::Scanning:
      if (timeNow - timeStateChange < StaggKettle::RetryDelay) break;
      if (pBLEScan != nullptr) {
        pBLEScan->stop();
        pBLEScan->clearResults();
      }
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
