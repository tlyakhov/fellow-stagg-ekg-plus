#ifndef __FSRSCALE_H__
#define __FSRSCALE_H__

#include <Preferences.h>
#include <math.h>

namespace Calibration {
static const int Count = 4;
static double Ounces[Count] = {0.0, 12.0, 24.0, 30.0};
static const int CurveOrder = 2;
static const int16_t BufferSize = 32;
}  // namespace Calibration

class FSRScale {
 public:
  FSRScale(byte pin);
  void loadFromPrefs();
  byte getCalibrationMode() { return calMode; }
  int getFill() { return (int)round(fill); }
  void nextCalibration();
  void loop();
  ~FSRScale();

 private:
  Preferences prefs;
  byte pin = 35;  // ADC_PIN0
  byte calMode = 0;
  uint16_t fsrBuffer[Calibration::BufferSize];
  byte fsrBufferPos = 0;
  double prevAvg;
  double fill;
  double coeffs[Calibration::CurveOrder + 1];
  double calReadings[Calibration::Count];
};

#endif