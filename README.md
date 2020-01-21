# fellow-stagg-ekg-plus

WiFi & cloud bridge for the [Fellow Stagg EKG+](https://fellowproducts.com/products/stagg-ekg-plus) electric kettle.

## Requirements

Designed to be deployed on an [M5Stack FIRE](https://m5stack.com/products/fire-iot-development-kit), though any ESP32-based board will probably work , assuming BLE & WiFi are available. PSRAM is highly recommended because the TLS connection for communicating with Firebase requires a lot (80KB+) of RAM to work.

Other hardware used is an FSR (Force Sensitive Resistor, [Interlink Electronics FSR-402](https://www.interlinkelectronics.com/fsr-402)) to measure fill (e.g. how many oz of water in the kettle), to avoid turning on an empty kettle remotely.

Developed using VSCode & Platform IO.

## Reverse Engineering

The kettle's BLE protocol was initially reverse engineered by using Wireshark on the official EKG+ iOS app traffic, then refined through experimentation.

Basic details:
* Enzytek BLE chip
    * SPS (Serial Port Service)
* 0xefdd as a separator for both rx/tx frames
* There's a magic init sequence to start comms with the kettle
* 8 byte commands for power/temp setting
* Arbitrary length state data, several type identifiers:
    * 0x00 - Power state (on or off)
    * 0x01 - Unknown, usually 0x01, 0x00, 0x00, 0x00
    * 0x02 - Target temperature
    * 0x03 - Actual temperature
    * 0x04 - Countdown when kettle is lifted off base
    * 0x05 - Unknown, usually 0x05, 0xFF, 0xFF, 0xFF, 0xFF
    * 0x06 - Unknown, usually 0x06, 0x00, 0x00
    * 0x07 - Unknown, usually 0x07, 0x00, 0x00, 0x00
    * 0x08 - Kettle is lifted off base?

## Tim's TODOs

* Test on M5Stack FIRE
* Figure out how to mount the FSR on the kettle in a non-janky way.

