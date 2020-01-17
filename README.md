# fellow-stagg-ekg-plus

WiFi & Google Assistant Bridge for Fellow Stagg EKG+ electric kettle.

Designed to be deployed on an [M5Stack Core](https://m5stack.com/collections/m5-core/products/basic-core-iot-development-kit), though any ESP32-based board will probably work
, assuming BLE & WiFi are available.

The kettle's BLE protocol was initially reverse engineered by using Wireshark on
the official EKG+ iOS app traffic, then refined through experimentation.

Another added feature here is an [FSR402]-based fill measurement (e.g. how many 
oz of water in the kettle), to avoid turning an empty kettle on remotely. This 
feature is probably way over-engineered by using 2nd order curve fitting on user
-supplied calibration data, given the questionable accuracy of the sensor.

# Tim's TODOs

* Actually get the Firebase part of this working
* Get temperature setting commands working.
* Figure out how to mount the FSR on the kettle in a non-janky way.