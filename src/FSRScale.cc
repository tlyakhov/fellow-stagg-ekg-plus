#include "FSRScale.hh"
#include <curveFitting.h>

FSRScale::FSRScale(Preferences *prefs, byte pin) : prefs(prefs), pin(pin) {
  for (int i = 0; i < Calibration::CurveOrder + 1; i++) {
    coeffs[i] = prefs->getDouble(("ScaleCoeffs" + String(i)).c_str(), 0.0);
    Serial.println("<Scale::Scale> Loaded coefficient " + String(i) + " = " +
                   String(coeffs[i]));
  }
}

FSRScale::~FSRScale() {}

void FSRScale::nextCalibration() {
  prevAvg = -1;
  if (calMode < Calibration::Count) {
    calMode++;
    return;
  }
  calMode = 0;

  int ret =
      fitCurve(Calibration::CurveOrder, Calibration::Count, Calibration::Ounces,
               calReadings, Calibration::CurveOrder + 1, coeffs);
  if (ret == 0) {
    Serial.println("<Scale::Loop> Calibrated scale! Coefficients:");
    for (int i = 0; i < Calibration::CurveOrder + 1; i++) {
      Serial.println(coeffs[i]);
      // Save to memory!
      prefs->putDouble(("ScaleCoeffs" + String(i)).c_str(), coeffs[i]);
    }
  } else {
    Serial.println("<Scale::Loop> Calibration error! " + String(ret));
  }
}

void FSRScale::loop() {
  uint16_t fsrValue = analogRead(pin);

  fsrBuffer[fsrBufferPos] = fsrValue;
  fsrBufferPos++;
  if (fsrBufferPos >= Calibration::BufferSize) fsrBufferPos = 0;

  double fsrAverage = 0.0;

  for (int i = 0; i < Calibration::BufferSize; i++) {
    fsrAverage += fsrBuffer[i];
  }

  fsrAverage /= (double)Calibration::BufferSize;

  weight =
      coeffs[0] * fsrAverage * fsrAverage + coeffs[1] * fsrAverage + coeffs[2];

  if (calMode >= 1 && calMode <= Calibration::Count && prevAvg != fsrAverage) {
    Serial.println("<Scale::Loop> Calibration value for " +
                   String(Calibration::Ounces[calMode - 1]) +
                   "oz = " + String(fsrAverage));
    calReadings[calMode - 1] = fsrAverage;
    prevAvg = fsrAverage;
  }
}