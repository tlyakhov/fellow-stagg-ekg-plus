#ifndef __FSRSCALE_H__
#define __FSRSCALE_H__

#include <Preferences.h>

namespace Calibration {
static const int Count = 4;
static double Ounces[Count] = {0.0, 8.0, 16.0, 24.0};
static const int CurveOrder = 2;
static const int16_t BufferSize = 8;
}  // namespace Calibration

class FSRScale {
 public:
  FSRScale(Preferences *prefs, byte pin);
  byte getCalibrationMode() { return calMode; }
  double getWeight() { return weight; }
  void nextCalibration();
  void loop();
  ~FSRScale();

 private:
  Preferences *prefs;
  byte pin = 35;  // ADC_PIN0
  byte calMode = 0;
  uint16_t fsrBuffer[Calibration::BufferSize];
  byte fsrBufferPos = 0;
  double prevAvg;
  double weight;
  double coeffs[Calibration::CurveOrder + 1];
  double calReadings[Calibration::Count];
};

#endif