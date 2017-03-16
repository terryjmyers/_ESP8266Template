# _ESP8266Template
Features:
1. Serial command interface
2. Telnet command interface, (login required)
3. Serial and telnet commands share common interface
4. Stores website login credentials in salted hash on EEPROM
5. SPIFFS Storage of config.json for network settings, project name, and sensor scaling(calibration data), NTP settings, etc
6. Access Point automatically created when WiFi not configured
7. Wegpage allows viewing of data, configuring network settings, viewing system data, editing files in SPIFFS, and more
8. NTP integrated with time zone offset and DST calculations
9. SMTP email

How to install:

1. Install my other libraries or delete references:
  1.https://github.com/terryjmyers/PulseTimer.git
  2.https://github.com/terryjmyers/LoopStatistics.git (not really needed you can delete references to this mroe easily)
  3.https://github.com/terryjmyers/TimeLib.git (Updated for DST and time zone offsets)
2. Install the arduino IDE file system uploader: https://github.com/esp8266/Arduino/blob/master/doc/filesystem.md#uploading-files-to-file-system
3. Upload /data folder using file system uploader tool from Arduino IDE
