#include "FSRScale.hh"
#include <curveFitting.h>

FSRScale::FSRScale(byte pin) : pin(pin) {
  // Some reasonable numbers from a good calibration:
  coeffs[0] = -0.08; // ax^2 -0.08
  coeffs[1] = 13.83; // bx 16.83?
  coeffs[2] = 3019.81; // c 2819.81?
}

FSRScale::~FSRScale() {}

void FSRScale::loadFromPrefs() {
  // NVRAM settings
  char prefName[] = "ScaleCoeffs0";
  prefs.begin("fellow-stagg", false);  
  for (int i = 0; i < Calibration::CurveOrder + 1; i++) {
    prefName[strlen(prefName)-1] = '0' + i;
    coeffs[i] = prefs.getDouble(prefName, 0.0);
    Serial.println("<Scale::Scale> Loaded coefficient " + String(i) + " = " +
                   String(coeffs[i]));
  }
  prefs.end();
}

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
      char prefName[] = "ScaleCoeffs0";
      prefName[strlen(prefName)-1] = '0' + i;
      prefs.begin("fellow-stagg", false);  
      prefs.putDouble(prefName, coeffs[i]);
      prefs.end();
    }
  } else {
    Serial.println("<Scale::Loop> Calibration error! " + String(ret));
  }
}

void FSRScale::loop() {
  // Read a raw value
  uint16_t fsrValue = analogRead(pin);

  // We use this buffer to average out the highly variable measurements
  fsrBuffer[fsrBufferPos] = fsrValue;
  fsrBufferPos++;
  if (fsrBufferPos >= Calibration::BufferSize) fsrBufferPos = 0;

  // Simple averaging
  double fsrAverage = 0.0;
  for (int i = 0; i < Calibration::BufferSize; i++) {
    fsrAverage += fsrBuffer[i];
  }
  fsrAverage /= (double)Calibration::BufferSize;

  // We curve fit the measurements during calibration to a 2nd order (quadratic)
  // polynomial ax^2 + bx + c = y [where x is the ounces filled and y is the FSR
  // measurement]. Now that we have the coefficients, use the quadratic formula
  // to solve for x based on y. x = (sqrt(4ay - 4ac + b^2) - b) / 2a
  double det = 4.0*coeffs[0]*(fsrAverage - coeffs[2])+coeffs[1]*coeffs[1];
  if (det >= 0.0) {
    double num = sqrt(det)-coeffs[1];
    double denom = 2 * coeffs[0];
    fill = denom == 0 ? 0 : (num / denom);
    if (fill < 0)
      fill = 0;
  } else {
    fill = 30.43; // Max fill is 0.9L
  }
  
  // Serial.println(String(fsrAverage) + " " + String(fill));

  if (calMode >= 1 && calMode <= Calibration::Count && prevAvg != fsrAverage) {
    Serial.println("<Scale::Loop> Calibration value for " +
                   String(Calibration::Ounces[calMode - 1]) +
                   "oz = " + String(fsrAverage));
    calReadings[calMode - 1] = fsrAverage;
    prevAvg = fsrAverage;
  }
}