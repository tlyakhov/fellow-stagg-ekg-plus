#ifndef __STAGGKETTLE_H__
#define __STAGGKETTLE_H__

#include <Arduino.h>
#include <BLEDevice.h>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

class StaggKettle : public BLEClientCallbacks,
                    public BLEAdvertisedDeviceCallbacks {
 public:
  // Some constants...
  enum State { Inactive, Scanning, Found, Connecting, Connected };
  static const char* StateStrings[5];
  const int RetryDelay = 5000; // 5s
  enum Command { On, Off, Set };
  enum TempUnits { Fahrenheit, Celsius };

  StaggKettle();
  ~StaggKettle();
  
  State getState() const { return state; }
  std::string getName() const { return name; }
  bool isOn() const { return power; }
  bool isLifted() const { return lifted; }
  bool isHold() const { return hold; }
  TempUnits getUnits() const { return units; }
  unsigned int getCountdown() const { return countdown; }
  byte getCurrentTemp() const { return currentTemp; }
  byte getTargetTemp() const { return targetTemp; }

  void scan();
  bool connectToServer();
  void setTemp(byte temp);
  void on() { qCommands.push(Command::On); }
  void off() { qCommands.push(Command::Off); }
  void loop();

  // BLE callbacks
  void onResult(BLEAdvertisedDevice advertisedDevice);
  void onConnect(BLEClient* pclient);
  void onDisconnect(BLEClient* pclient);
  void onNotify(BLERemoteCharacteristic* c, uint8_t* pData, size_t length,
                bool isNotify);

 private:
  // kettle states
  volatile State state;
  byte sequence = 0;
  byte currentTemp = 0;
  byte targetTemp = 0;
  byte userTemp = 0;
  bool lifted = false;
  bool power = false;
  bool hold = false;
  TempUnits units = TempUnits::Fahrenheit;
  unsigned int countdown;

  // kettle data states
  uint8_t buffer[64];
  int bufferPos = 0;
  int bufferState = 0;
  std::unordered_map<uint8_t, uint8_t*> unknownStates;

  // BLE state
  BLEScan* pBLEScan;
  BLEAdvertisedDevice* pDevice;
  BLERemoteService* pRemoteService;
  BLERemoteCharacteristic* prcKettleSerial;
  BLEClient* pClient;
  std::string name;

  // Device state
  unsigned long timeLastCommand;
  unsigned long timeStateChange;
  std::queue<Command> qCommands;
  std::mutex mtxState;

  void parseEvent(const uint8_t* data, size_t length, bool debug);
  void sendCommand(Command cmd);
};
#endif
