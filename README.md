# _ESP8266Template
Features:
1. Serial command interface
2. Telnet command interface, (login required)
*Serial and telnet commands share common interface
*Stores website login credentials in salted hash on EEPROM
*SPIFFS Storage of config.json for network settings, project name, and sensor scaling(calibration data), NTP settings, etc
*Access Point automatically created when WiFi not configured
*Wegpage allows viewing of data, configuring network settings, viewing system data, editing files in SPIFFS, and more
*NTP integrated with time zone offset and DST calculations
*SMTP email
