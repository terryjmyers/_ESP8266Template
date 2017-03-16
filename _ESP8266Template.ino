/*
_ESP8266Template
 v1.4
 Terry Myers <TerryJMyers@gmail.com>
 https://github.com/terryjmyers/_ESP8266Template.git

 Features:
	Serial command interface
	Telnet command interface, (login required)
	Serial and telnet commands share common interface
	Stores website login credentials in salted hash on EEPROM
	SPIFFS Storage of config.json for network settings, project name, and sensor scaling(calibration data), NTP settings, etc
	Access Point automatically created when WiFi not configured
	Wegpage allows viewing of data, configuring network settings, viewing system data, editing files in SPIFFS, and more
	NTP integrated with time zone offset and DST calculations
	SMTP email

Checklist for project creation/instantiation:
	From Arduino IDE upload all files in the /data folder from ArduinoIDE/tools/ESP8266 Sketch Data Upload menu selection.
		To install this feature, reference section "Uploading files to file system" on: https://github.com/esp8266/Arduino/blob/master/doc/filesystem.md#uploading-files-to-file-system
	Update #define CONTACT_INFORMATION
	Modify Functional Description and contact information in Help menu
	Update default ProjectName, STASSID, STAPassword, and STAhostname
	Comment out "#define DEBUG"
	Comment out  "#define THINGSPEAK"
	Download program
	run Reset Factory Default from serial menu
	Reboot to ensure the AP is started
	Update help menu telnet command to be specific to project
	Update webpage Data page to be specific to project
	Update scaling factors

Checklist for distribution to github
	Update BACKDOOR_PASSWORD, CONTACT_INFORMATION, email info
	replace sha1salt

Issues:
	mDNS responder isn't working

TODO:
	Create reset button that will trigger a hardware reset with a specific output
	Test the checklist above
	Add commands to serial to turn off webpage and/or AP
	Automatically start the AP if you toggle the WiFi off.default
	Perhaps make an RUSure page when toggleing WiFi off.
	Checkout Thinger.io to see if it can be used
	Enable/disable telnet through webpage
	Create an error 500 webpage
	Set up an unencrypted email address
	Split out admin page its getting too much.
	Split out sensor scaling to another page
	Add ability to update thingspeak credentials throgh webpage
	Add ability to add email addres / sms text through webpage

Untested:
	
*/

//Modify section of the code to suit a project:
//#define DEBUG //developer debug
#define THINGSPEAK

//Includes
	#ifdef THINGSPEAK
		#include <ThingSpeak.h>
	#endif // THINGSPEAK
	#include <LoopStatistics.h> //personal Loop statistics library.  calculates min/max loop time as well as loops per sec to measure program performance: https://github.com/terryjmyers/LoopStatistics.git
	#include <PulseTimer.h> //personal timer library.  Used to create pulses bits for timing: https://github.com/terryjmyers/PulseTimer.git
	#include <ESP8266WiFi.h>
	#include <ESP8266WebServer.h>
	#include <ESP8266mDNS.h>
	#include <WiFiClient.h>
	#include <ESP8266HTTPClient.h>
	#include <EEPROM.h>
	#include <Hash.h>
	#include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson
	#include <FS.h>
	#include <Wire.h>
	#include <TimeLib.h> //used for NTP, modified library to account for daylight savings time and timezone inputs: https://github.com/terryjmyers/TimeLib.git
	#include <ticker.h> //used for WatchDog
	//#include <LPF.h> //personal Low Pass Filter Library
	#include <DHT.h> 
		/*
		Adafruit DHT22 library is not optimized for speed.  The following changes were made in accordance with the datasheet:
		DHT.cpp
			lines 140 commented out - "//digitalWrite(_pin, HIGH);"
			line 141 commented out - "//delay(250)"
			line 146 delay after digitalWrite(_pin, LOW) lowered to 10ms from 20- "delay(10)"

			e.g. lines 140 to 146 should look like this:
				digitalWrite(_pin, HIGH);
				delay(1);

				// First set data line low for 20 milliseconds.
				pinMode(_pin, OUTPUT);
				digitalWrite(_pin, LOW);
				delay(1);
		*/
	#include <DHT_U.h>


//DHT22 stuff
	#define DHTPIN 0     // what digital pin we're connected to pin 0 = GPIO0 = D3 on nodeMCU/wemos
	#define DHTTYPE DHT22   // DHT 22  (AM2302), AM2321
	DHT dht(DHTPIN, DHTTYPE);
	//LPF01 LPFs; 
	//LPF01 LPFHum;

//Project Settings
#define FirmwareVersion "1.6";
	String ProjectName = "DefaultProjectName"; //The WiFi hostname is taken from this, do not use any symbols or puncuation marks, hyphens are fine, and spaces will be automatically replaced with underscores
	const char * FUNCTIONAL_DESCRIPTION = "This device contains a base slate with a webpage, telnet, and Serial to USB access to data";
	const char * CONTACT_INFORMATION = "Terry Myers: TerryJMyers@gmail.com, XXX.XXX.XXXX";
	uint32_t __UPTIME; //System up time in seconds, displayed on Admin webpage
	String GlobalErrorMessage; //used to enunciate errors on webpage
	uint8_t debug; //debugging bit able to be turned on and off from serial/telnet console

//configuration Json buffer 
	#define CONFIGJSONFILZESIZE 2048
//Watchdog
	Ticker OneSecondTick;
	volatile uint32_t WatchDog = 0;

//telnet server (socket connections)
	#define BACKDOOR_PASSWORD "backdoor" //back door password used for wegpage login and telnet login.  Used as the User Name field, password doens't matter
	#define TELNET_MAX_CLIENTS 2 //probably never need more than 1, but its here nonetheless
	#define TELNETPORT 23 //default is 23
	WiFiServer TelnetServer(TELNETPORT);
	struct TelnetServerClients { //custom struct to handle each client data and login information independantly
		WiFiClient clients;
		String clientBuffer; //Create a global string that is added to character by character to create a final serial read
		String clientLastLine; //Create a global string that is the Last full Serial line read in from sSerialBuffer
		String login;
		String password;
		uint8_t loginStep;
		bool isAuthenticated;
		bool NewLine;
	};
	struct TelnetServerClients Telnet[TELNET_MAX_CLIENTS];
	
//Web servers
	#define HTTPPORT 80
	ESP8266WebServer HTTPserver(HTTPPORT);
	File fsUploadFile; //Global file object for uploads
	uint8_t refresh;

//wifi setup
	//WiFi Client

		bool ThingSpeakEnable = false;
		WiFiClient ThingSpeakClient;
		unsigned long ThingSpeakChannel = 0;
		String ThingSpeakapiKey = "none";
	//Station Mode default configuration, overwritten by network configuration stored in config.json in SPIFFS
		IPAddress STAip(192, 168, 1, 250);
		IPAddress STAsubnet(255, 255, 255, 0);
		IPAddress STAgateway(192, 168, 1, 1);
		bool AutoDHCP = true;
	//AP mode
		IPAddress APip(STAip[0], STAip[1], STAip[2]+1, STAip[3]); //Set the AP to a different subnet offset by 1
		IPAddress APsubnet(STAsubnet[0], STAsubnet[1], STAsubnet[2], STAsubnet[3]);
		IPAddress APgateway(STAip[0], STAip[1], STAip[2] , 1);
		//Create some structs to make human readable messages
			String encryptionTypeStr(uint8_t authmode) {
				switch (authmode) {
					case ENC_TYPE_NONE:
						return F("OPEN");
					case ENC_TYPE_WEP:
						return F("WEP");
					case ENC_TYPE_TKIP:
						return F("WPA_PSK");
					case ENC_TYPE_CCMP:
						return F("WPA2_PSK");
					case ENC_TYPE_AUTO:
						return F("WPA_WPA2_PSK");
					default:
						return F("UNKOWN");
				}
			}
			String BootModeStr(uint8_t authmode) {
				switch (authmode) {
					case FM_QIO:
						return F("FM_QIO");
					case FM_QOUT:
						return F("FM_QOUT");
					case FM_DIO:
						return F("FM_DIO");
					case FM_DOUT:
						return F("FM_DOUT");
					case FM_UNKNOWN:
						return F("FM_UNKNOWN");
					default:
						return F("UNKOWN");
				}
			}
	//email setup
			//Adapted from Boris Shobat http://www.instructables.com/id/ESP8266-GMail-Sender/?ALLSTEPS
				class Gsender
				{
					protected:
						Gsender();
					private:
						const int SMTP_PORT = 465;
						const char* SMTP_SERVER = "smtp.gmail.com";
						const char* EMAILBASE64_LOGIN = "ABASE64EMAILADDRESS="; //ESP8266tjm@gmail.com
						const char* EMAILBASE64_PASSWORD = "ABASE64PASSWORD="; //4t6j364t
						const char* FROM = "YOUREMAILADDRESS@gmail.com";
						const char* _error = nullptr;
						char* _subject = nullptr;
						String _serverResponce;
						static Gsender* _instance;
						bool AwaitSMTPResponse(WiFiClientSecure &client, const String &resp = "", uint16_t timeOut = 10000);

					public:
						static Gsender* Instance();
						Gsender* Subject(const char* subject);
						Gsender* Subject(const String &subject);
						bool Send(const String &to, const String &message);
						String getLastResponce();
						const char* getError();
				};
				Gsender* Gsender::_instance = 0;
				Gsender::Gsender() {}
				String EmailAddress = "RECEIVERSEMAILADDRESS@gmail.com";
			
		
		
	//NTP
		String timeStamp;
		unsigned int localPort = 2390;      // local port to listen for UDP packets
		IPAddress timeServerIP; // time.nist.gov NTP server address
		//const char *ntpServerName = "time.nist.gov"; //NTP 
		String NTPServerName = "time.nist.gov"; //NTP 
		const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
		byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
		WiFiUDP udp; // A UDP instance to let us send and receive packets over UDP
		int NTPTimeZone = -5;  // Eastern Standard Time (USA)
		uint8_t NTPdstRule;
		bool NTPDaylightSavings = true;  // account for US daylight savings: 2am on second sunday in March, ends at 2am on first Sunday in November
		uint32_t NTPUpdateRate = 86000;//Set how often to update time in seconds: almost once a day was choosen to get the time at different times of the day or randomness
			
		
			//Configure EEPROM
	//EEPROM is used for storing a sha1 hash of the website login/password that is salted
	//EEPROM Map: |WEBSITELOGINsha1HASHxxxxxxxxxxxxxxxxxxxxxxxxxxxxxWEBSITEPASSWORDsha1HASHxxxxxxxxxxxxxxxxxxxxxxx...xxxxxxxxxxxxxxxxxxxxxxxDEBUGBYTE|
	#define EEPROMSIZE 512
		enum isEEPROMDebug {
			EEPROMDebug_YES = 0,
			EEPROMDebug_NO = 255
		};
		//salt bytes added to plain text website login/password to salt the text before hashing using sha1 and storing in EEPROM.  The program never really knows the login/password
				const char sha1salt[17] = { 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
		//EEPROM Map setup, store the start/end byte of everything to make storing and retriving easier
			//Website Login Name
				const uint16_t EEPROMHTMLLoginSTART = 0;
				const uint16_t EEPROMHTMLLoginEND = EEPROMHTMLLoginSTART + 40;
				const char *HTMLDefaultLogin = "admin"; //used as default login name and password when the EEPROM reads back a 255 byte in the first byte(indicative of the login never having changed i.e. EEPROM is blank)
			//Website Login Password
				const uint16_t EEPROMHTMLPasswordSTART = EEPROMHTMLLoginEND + 1;
				const uint16_t EEPROMHTMLPasswordEND = EEPROMHTMLPasswordSTART + 40;
			#define EEPROMDebug EEPROMSIZE-1 //store a "bit" in the EEPROM to determine if debugging is on or not. which sets a bit called "debug" used throughout the code

//Serial Read tags
	//serial text is read in and parsed into acomma separated variable array
	#define SERIAL_BUFFER_SIZE 128 //max size of ESP8266 serial buffer
	String sSerialBuffer; //Software serial buffer
	String sLastSerialLine; //sSerialBuffer is copied into this tag when a \r, \n, or \0 character is receieved.  The text is processed then cleared out as a handshake

//Setup a instance of LoopStatistics
	#ifdef DEBUG
	LoopStatistics LT;
	#endif

//Set up the preset timer for the pulse timers.  Note that the closest prime number was choosen to prevent overlapping with other timers
	PulseTimer _60000ms(59999);
	PulseTimer _10000ms(9979);
	PulseTimer _1000ms(997);
	PulseTimer _500ms(499);
	//PulseTimer _100ms(101);
	//PulseTimer _10ms(11);
	//PulseTimer _1ms(1);

//Project Specific sensor scaling/configuration Data.  Sensor data is loaded into .raw, scaling using y=mx+b scaling using .rawLo, .rawHi, .scaleLo, and .scaleHi, and moved into .Actual
//These values are stored in config.json.txt on SPIFFS
	struct AI {
		float raw;
		float rawLo;
		float rawHi;
		float scaleLo;
		float scaleHi;
		float Actual;
		String EngUnits;
		uint8_t precisionRaw;//not stored in config.json.txt on SPIFFS: number of decimals places to display for Raw units, automatically calculated based on range to provide a minimum of 32767 steps regardless of actual range
		uint8_t precision;  //nnot stored in config.json.txt on SPIFFS: number of decimals places to display for Actual units, automatically calculated based on range to provide a minimum of 32767 steps regardless of actual range
	};
	struct AI Data[3]; //create three instances for up to 3 pieces of data to be scaled

//SETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUP
	void setup() {

		//Reserve String space to assist with heap fragmentation and optimization
			ProjectName.reserve(32);
			GlobalErrorMessage.reserve(256);
			timeStamp.reserve(19);
			Telnet[0].clientBuffer.reserve(64);
			Telnet[0].clientLastLine.reserve(64);
			Telnet[0].login.reserve(32);
			Telnet[0].password.reserve(32);
			Telnet[1].clientBuffer.reserve(64);
			Telnet[1].clientLastLine.reserve(64);
			Telnet[1].login.reserve(32);
			Telnet[1].password.reserve(32);
			Data[0].EngUnits.reserve(5);
			Data[1].EngUnits.reserve(5);
			Data[2].EngUnits.reserve(5);
			EmailAddress.reserve(50);
			NTPServerName.reserve(32);

		//Setup WatchDog ISR
			OneSecondTick.attach(1, ISRWatchdog);
		//Setup Serial 
		//In case the user uses the default setting, give them a message to change BAUD rate
			Serial.begin(9600);
			Serial.println(F("Set baud rate to 250 000 and restart"));
			Serial.flush();   // wait for send buffer to empty
			delay(2);    // needed for the last line feed character to be sent for some reason
			Serial.end();      // close serial
		//Setup Serial
			Serial.begin(250000);
			Serial.println();
			Serial.println(); //I think two are needed for it to be pretty in a serial monitor
		//Set serial read buffer size to reserve the space to concatnate all bytes recieved over serial
		sSerialBuffer.reserve(SERIAL_BUFFER_SIZE);
		sLastSerialLine.reserve(SERIAL_BUFFER_SIZE);

		if (!SPIFFS.begin()) EndProgram(F("Failed to start SPIFFS ")); 

		loadConfig();//load config.json.txt from SPIFFS

		EEPROMStart(); //start EEPROM

		dht.begin(); //Iitiate the DHT22 sensor
		
		//Initialize Wifi Stuff
			WiFi.hostname(ProjectName); //update the hostname
			if (bitRead(WiFi.getMode(), 0))		WiFiSetup();  //if Station mode is on (stored in the protected ESP8266 flash memory), connect to a network			
			if (bitRead(WiFi.getMode(), 1))		StartAP();  //if AP mode is on (stored in the protected ESP8266 flash memory), initialize the AP
			if (WiFi.getMode() > 0)				launchWeb(); 

		//Initialzize udp for NTP
			if (WiFi.status() == WL_CONNECTED) {
				Serial.print(Line());
				udp.begin(localPort);
				setSyncProvider(getNtpTime);
				setTimeZone(NTPTimeZone);
				setdstRule(NTPdstRule);
				Serial.print(F("Time is ")); Serial.println(updatetimeStamp());
			}

		//Start Telnet Server
			if (WiFi.getMode() > 0) { //if any WiFi is on, Station or AP, turn on the telnet server
				TelnetServer.begin();
				TelnetServer.setNoDelay(true);
			}

	#ifdef THINGSPEAK
		if (ThingSpeakEnable && bitRead(WiFi.getMode(), 0)) {ThingSpeak.begin(ThingSpeakClient);}
	#endif

	Serial.print(WelcomeMessage());	//print a welcome message

	//sendEmail();
}
//SETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUPSETUP
	
//MAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOP
//MAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOP
void loop() {
	WatchDog = 0;//Feed the watchdog

	ProcessTelnetCommands();	//handle all telnet requests
	ProcessSerialCommands();	//handle all serial requests
	HTTPserver.handleClient();	//handle all HTTP requests
	ProcessErrorMessage();

	 //Update pulse timers
		_1000ms.tick();
		_500ms.tick();
		_60000ms.tick();
		_10000ms.tick();	//Update sensor data

	//Update sensor data
		if (_10000ms.pulse()) {
			ReadInputs();
			if (debug == EEPROMDebug_YES) { Serial.print(Data[0].Actual); Serial.print(Data[0].EngUnits); Serial.print("\t"); Serial.print(Data[1].Actual);  Serial.print(Data[1].EngUnits); Serial.println(); }
			UpdateThingSpeak(); //if thingspeak is not initialized this will just return
		}
	//increment __UPTIME
		if (_1000ms.pulse()) __UPTIME++;
		//if (_1000ms.pulse()) Serial.println(updatetimeStamp());
		
	#ifdef DEBUG
		if (_1000ms.pulse()) { LT.printStatistics(); }
		LT.tick();
	#endif
	//Yield to the ESP8266's background processes
		yield(); //Not sure if this is really necessesary, seems to work fine without it, but it feels really good!
}
//MAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOP
//MAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOPMAINLOOP
void sendEmail() {
	Gsender *gsender = Gsender::Instance();    // Getting pointer to class instance
	//String subject = "Subject is optional!";
	if (gsender->Subject(ProjectName)->Send(EmailAddress, "Setup test")) {
	//if (gsender->Send("TerryJMyers@gmail.com", "Setup test")) {
		Serial.println("Email sent.");
	}
	else {
		Serial.print("Error sending email: "); Serial.println(gsender->getError());
	}
}//===============================================================================================
void ISRWatchdog(void){
	//Routine called by Ticker Library once a second.  WatchDog is reset at the top of loop.  If this WatchDog ever gets to 3, reset the module, something has gone wrong.
	//Some of the functions require upwards of 15 second, such as loading /edit wegpage, download a file, or access the /network as it scans the network
	WatchDog++;
	if (WatchDog >= 30) {
		ESP.reset();
	}
} //===============================================================================================
time_t getNtpTime() {
	//Get Unix time from NTP
	Serial.print(F("Getting NTP time..."));
	while (udp.parsePacket() > 0); // discard any previously received packets
	char cNTPServerName[50];
	NTPServerName.toCharArray(cNTPServerName, NTPServerName.length()+1);
	WiFi.hostByName(cNTPServerName, timeServerIP); //get IP Address from pool
	sendNTPpacket(timeServerIP); //Send NTP packet
	unsigned long t0 = micros(); //record the time when the packet was sent

	uint32_t beginWait = millis();
	while (millis() - beginWait < 1500) {
		int size = udp.parsePacket();
		if (size >= NTP_PACKET_SIZE) {
			unsigned long t3 = micros(); //record the time when the response was recieved
			udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
			unsigned long secsSince1900;
			unsigned long secsSince1970;
			unsigned long fractionSeconds; //fractional seconds
			// convert four bytes starting at location 40 to a long integer
			secsSince1900 = (unsigned long)packetBuffer[40] << 24;
			secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
			secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
			secsSince1900 |= (unsigned long)packetBuffer[43];

			secsSince1970 = secsSince1900 - 2208988800UL; //convert UTC time to Unix time Jan 1, 1900 -> Jan 1, 1970
			//Serial.print("Unix Time: "); Serial.println(secsSince1970);

			//calculate the fractional seconds, delay for the rest of the second, then return the next second.  This will get the time even closer to real time
					fractionSeconds = (unsigned long)packetBuffer[44] << 24;
					fractionSeconds |= (unsigned long)packetBuffer[45] << 16;
					fractionSeconds |= (unsigned long)packetBuffer[46] << 8;
					fractionSeconds |= (unsigned long)packetBuffer[47];
					double Fraction = ((double)(fractionSeconds & 0xffffffff)) / pow(2, 32);
					//Serial.print("Fractional Seconds: "); Serial.println(Fraction,16);

					//double doubleSeconds = double(secsSince1970) + Fraction;
					//Serial.print("Precise Seconds: "); Serial.println(doubleSeconds, 12);

					//Serial.print("Transit time"); Serial.println(t3 - t0);
					int32_t delayUntilNextSecond = 1000000 - Fraction * 1000000 - (t3 - t0)/2 - 50; //calculate how long to delay for until the next second, minus half the transit time which is a decent estimate of the real time minus 50us(the approximate time it takes to get from t3 to the start of the delay below
					if (delayUntilNextSecond > 10) {//if the microseconds to delay for is greater than a could clock cycles then actually start the delay
						//Serial.print("delay for"); Serial.println(delayUntilNextSecond);
						beginWait = micros();
						while (micros() - beginWait < delayUntilNextSecond) {
							yield();
						}
					}
			return secsSince1970 + 1;
		}
		yield();
	}
	GlobalErrorMessage = F("getNtpTime: No NTP Response");
	return 0; // return 0 if unable to get the time
} //===============================================================================================
String updatetimeStamp() {
	//Update a global varaible timeStamp with a database parseable and human readable timestamp.
	//Also return the value for quick printing.

	if (timeStatus() != timeNotSet) {//Calling timeStatus actually tries to update the time via NTP
		//Time has been set by NTP
		timeStamp = String(year());
		timeStamp += F("-");

		if (month() < 10) timeStamp += F("0");
		timeStamp += String(month());
		timeStamp += F("-");

		if (day() < 10) timeStamp += F("0");
		timeStamp += String(day());
		timeStamp += F(" ");

		if (hour() < 10) timeStamp += F("0");
		timeStamp += String(hour());
		timeStamp += F(":");

		if (minute() < 10) timeStamp += F("0");
		timeStamp += String(minute());
		timeStamp += F(":");

		if (second() < 10) timeStamp += F("0");
		timeStamp += String(second());
		setSyncInterval(NTPUpdateRate); //Set next time update to a long time
	} 
	else {
		//Time has not been set by NTP
		timeStamp = "INTERNET TIME ERROR";
		setSyncInterval(5); //Set next time update to a short time
	}
	return timeStamp;

} //===============================================================================================
unsigned long sendNTPpacket(IPAddress address)
{
	//For more information: http://www.cisco.com/c/en/us/about/press/internet-protocol-journal/back-issues/table-contents-58/154-ntp.html
	if (debug== EEPROMDebug_YES) Serial.println(F("sendNTPpacket: Transmitting NTP Request packet to: ")); Serial.println(address);
	// set all bytes in the buffer to 0
	memset(packetBuffer, 0, NTP_PACKET_SIZE);
	// Initialize values needed to form NTP request
	// (see URL above for details on the packets)
	packetBuffer[0] = 0b11100011;   // LI, Version, Mode
	packetBuffer[1] = 0;     // Stratum, or type of clock
	packetBuffer[2] = 6;     // Polling Interval
	packetBuffer[3] = 0xEC;  // Peer Clock Precision
							 // 8 bytes of zero for Root Delay & Root Dispersion
	//packetBuffer[12] = 49; //reference identifier 1
	//packetBuffer[13] = 0x4E; //N
	//packetBuffer[14] = 49; //1
	//packetBuffer[15] = 52; //4

	// all NTP fields have been given values, now
	// you can send a packet requesting a timestamp:
	udp.beginPacket(address, 123); //NTP requests are to port 123
	udp.write(packetBuffer, NTP_PACKET_SIZE);
	udp.endPacket();
} //===============================================================================================
void ProcessErrorMessage(void) {
	//Print global error message to serial and telnet clients and clear it out as a handshake
	if (GlobalErrorMessage!="") {
	Serial.println(GlobalErrorMessage);
	Serial.flush(); 
	TelnetPrintAllClients(GlobalErrorMessage);
	GlobalErrorMessage = "";
	}
} //===============================================================================================

void ReadInputs() {
	//Read the serature data register
	Data[0].raw = dht.readTemperature();
	//int t = random(4000, 8000);
	//Data[0].raw = (float)t / 100.0;
	Scalar(Data[0]);

	Data[1].raw = dht.readHumidity();
	Scalar(Data[1]);


} //===============================================================================================
void UpdateThingSpeak() {
	#ifdef THINGSPEAK
	if (ThingSpeakEnable && bitRead(WiFi.getMode(), 0)) {
		char temp[17];
		ThingSpeakapiKey.toCharArray(temp, ThingSpeakapiKey.length() + 1); //convert string to char
		ThingSpeak.writeField(ThingSpeakChannel, 1, Data[0].Actual, temp);
	}
	#endif
}//===============================================================================================
void IOSetup() {
	//not used
	pinMode(LED_BUILTIN, OUTPUT);
	pinMode(D8, OUTPUT);
}//===============================================================================================

void StartAP(void) {
	//Start Access Point
	Serial.print(F("Starting Access Point..."));

	WiFi.softAPConfig(APip, APgateway, APsubnet);
	//Start AP
	char test[33];
	ProjectName.toCharArray(test, ProjectName.length()+1);
	if (WiFi.softAP(test) == false) {
		Serial.println(F("FAILED"));
	}
	else {
		//sucess
		WiFi.softAPConfig(APip, APgateway, APsubnet);
		Serial.println(F("SUCCESS"));
		Serial.print(wifiGetStatusAP());
	}
	
}//===============================================================================================
bool WiFiConnect(String ssid = "", String pw = "", IPAddress ip = STAip, IPAddress sn = STAsubnet, IPAddress gw = STAgateway) {
	/*
	Try connect to a WiFi network
	if sucessful, store configuration in the EEPROM
	Give up after 10 seconds
	if not sucessful, revert to previous configuration if you were connected when you entered this function
	Returns:
	Returns a 0 if failed to connect or reconnect to original network
	Returns a 1 if sucessfully connected to new network
	Returns a 2 if failed to connect but sucessfully returns to original network
	*/

	//Trim whitespace from the beg and end of the strings in case someone added a space or copied ans pasted from a text file or something
		ssid.trim();
		pw.trim();

	//declare some variables
		int i = 0; //iterator
		String s; s.reserve(128);
		bool wasConnected = false;
	//Store the current configuration in case the network you are going to connect to doesn't work and you need to reconnect
		String _ssid = WiFi.SSID();
		String _pw = WiFi.psk();
		IPAddress _ip = WiFi.localIP();
		IPAddress _sn = WiFi.subnetMask();
		IPAddress _gw = WiFi.gatewayIP();

	//WiFi was already connected remember variables for the current configuration in case it doesn't work
		if (WiFi.status() == WL_CONNECTED) {
			wasConnected = true;
			wifiDisconnect(); //disconnect from any wifi
		}
	//Enable STation mode
		WiFi.enableSTA(1); //doesn't work unless you put this here for some reason
	//try to connect to new WiFi
		if (!AutoDHCP) WiFi.config(ip, gw, sn); //If static IP Address is selected set ip, sn, and gw
		if (ssid == "") { //passed in parameters are blank try to connect using stored credentials on the ESP
			s = F("Connecting to WiFi network using stored credentials please wait.");
			WiFi.begin();//initiate the connection
		} 
		else { //an SSID and password were passed in, try to connect using these
			s = F("Connecting to WiFi network <"); s += ssid; s += F("> please wait.");
			WiFi.begin(ssid.c_str(), pw.c_str());//initiate the connection
		}
		Serial.print(s); TelnetPrintAllClients(s);
		while (i < 20) {
			if (WiFi.status() == WL_CONNECTED) {
				s = F("connected to: "); 	s += WiFi.SSID(); s += F("\r\n");	s += wifiGetStatus();
				Serial.print(s); TelnetPrintAllClients(s);
				Serial.flush();//delete all of the incoming serial buffer because we havn't been monitoring it while connecting and we don't want any commands that are queued up to be immediatly processed
				return true; //return a success!
			}
			delay(500);
			s = (".");
			Serial.print(s); TelnetPrintAllClients(s);
			i++;
		}

	//You failed to connet to the WiFi...that stinks
		s = F("FAILED to connect to network\r\n");
		Serial.print(s);// TelnetPrintAllClients(s);

	//If you were connected when you came into this routine, load those credentials back into the ESP and restart
		if (wasConnected == true) {
			WiFi.begin(_ssid.c_str(), _pw.c_str());//initiate the connection to the previous network
			Restart();
		
		}
return false;
		//You failed to connect to a network, just return
}//===============================================================================================
void WiFiSetup() {
	//pass through function for WiFiConnect() call in Setup because for some reason it doesn't compile 
	WiFiConnect();
}//===============================================================================================
void wifiDisconnect() {
	String s; s.reserve(64);
	//disconnect from current wifi if applicable, and ensure a disconnection
	s = F("Disconnecting from current WiFi\r\n");
	Serial.print(s); TelnetPrintAllClients(s);

	WiFi.enableSTA(0);
	WiFi.disconnect(); //disconnect
	while (WiFi.status() == WL_CONNECTED) {//Not sure if this is needed but I think I saw it once where I disconnected then immeidatly tryied to evaluate status and it still came back connected so.....its here
		delay(1);
	}
}//===============================================================================================
String wifiGetStatus(void) {
	//Get a niceley formatting string for all wifi info
	WiFiMode_t Mode = WiFi.getMode();
	wl_status_t status = WiFi.status();

	String s; s.reserve(1024);

	//-------------------------
	s += Line();
	s += F("Wifi Mode: ");
	if (Mode == WIFI_OFF) {
		s += F("WIFI_OFF");
	}
	if (bitRead(Mode,0)) {
		s += F("Station, ");
	}
	if (bitRead(Mode, 1)) {
		s += F("Access Point");
	}
	s += F("\n\r");
	//-------------------
	s += F("Wifi: ");
	if (status ==3) {
		s += F("CONNECTED\n\r");
		s += wifiGetStatusSTA();
	}
	else {
		s += F("NOT CONNECTED\n\r");
	}
	if (bitRead(Mode, 1) && isAPON()) {
		s += wifiGetStatusAP();
	}
	s += Line();
	return s;
}//===============================================================================================
String wifiGetStatusSTA(void) {

	String s; s.reserve(512);
	s += F("Station Information:"); s += F("\n\r");
	s += F("\tHostname:\t"); s += WiFi.hostname(); s += F("\n\r");
	s += F("\tWebpage:\thttp://"); s += WiFi.hostname(); s += F(".local\n\r");
	s += F("\tSSID:\t\t"); s += WiFi.SSID(); s += F("\n\r");

	if (debug == EEPROMDebug_YES) {
		s += F("\tpsk:\t\t"); s += WiFi.psk(); s += F("\n\r");
	}
	else {
		s += F("\tpsk:\t\t********************\n\r");
	}
	s += F("\tAuto DHCP:\t"); (AutoDHCP) ? s+=F("ON") : s += F("STATIC IP"); s += F("\n\r");
	s += F("\tIP:\t\t"); s += WiFi.localIP().toString(); s += F("\n\r");
	s += F("\tSN:\t\t"); s += WiFi.subnetMask().toString(); s += F("\n\r");
	s += F("\tGateway:\t"); s += WiFi.gatewayIP().toString(); s += F("\n\r");
	s += F("\tDNS:\t\t"); s += WiFi.dnsIP().toString(); s += F("\n\r");
	if (debug == EEPROMDebug_YES) {
		s += F("\tMAC:\t\t"); s += WiFi.macAddress(); s += F("\n\r");
		s += F("\tBSSID:\t\t"); s += WiFi.BSSIDstr(); s += F("\n\r");
	}
	s += F("\tRSSI:\t\t"); s += WiFi.RSSI(); s += F("\n\r");

	return s;
}//===============================================================================================
String wifiGetStatusAP(void) {
	String s; s.reserve(256);
	s += F("AP Information:");  s += "\n\r";
	s += F("\tSSID:\t\t");  s += ProjectName; s += "\n\r";
	s += F("\tIP:\t\t");  s += WiFi.softAPIP().toString(); s += "\n\r";
	if (debug == EEPROMDebug_YES) {
		s += F("\tMAC:\t\t");  s += WiFi.softAPmacAddress(); s += "\n\r";
		s += F("\t# Connected Users:\t\t");  s += WiFi.softAPgetStationNum(); s += "\n\r";
	}
	return s;
}//===============================================================================================
String wifiScanNetworks(void) {
	//Return a formatted string for serial or telnet printing that is a full sorted scan of the local area networks
	String s; s.reserve(1024);
	s += Line();
	s += F("Scanning for local wifi networks...");

	// WiFi.scanNetworks will return the number of networks found
	WiFi.scanNetworks(true, false); //start Asyncronous scan
	while (WiFi.scanComplete() < 0) { //yield to the ESP8266 background processes while the scanning is occuring
		yield();
	}
	int n = WiFi.scanComplete(); //get the number of returned networks
	Serial.println(n);
	//  Serial.println("scan done");
	int o = n;
	int loops = 0;

	if (n == 0) {
		s += F("0 networks found, do you live in the boonies?");
		return s;
	}
	
	// sort by RSSI
		int indices[50];
		int skip[50];

		String ssid;

		for (int i = 0; i < n; i++) {
			indices[i] = i;
		}
		
	//sort 
		for (int i = 0; i < n; i++) {
			for (int j = i + 1; j < n; j++) {
				if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
					loops++;
					//int s = indices[j];
					//indices[j] = indices[i];
					//indices[i] = s;
					std::swap(indices[i], indices[j]);
					std::swap(skip[i], skip[j]);
				}
			}
		}

	//remove duplicates
		for (int i = 0; i < n; i++) {
			if (indices[i] == -1){
				--o;
				continue;
			}
			ssid = WiFi.SSID(indices[i]);
			for (int j = i + 1; j < n; j++) {
				loops++;
				if (ssid == WiFi.SSID(indices[j])) {
					indices[j] = -1;
				}
			}
		}

		//remove networks below -90db	
		for (int i = 0; i < n; i++) {
			if (WiFi.RSSI(indices[i]) < -90) {
				--o;
				indices[i] = -1;
			}
		}
		
		s += o;
		s += F(" networks found of "); s += (String)n; s += F("\r\n");
		s += F("\t(RSSI)\t(STR)\tSSID\t\t[encryption]"); s += F("\r\n");
	for (int i = 0; i < n; ++i) {
		if (indices[i] != -1){
			// Print SSID and RSSI for each network found
			s += F("\t("); 
			s += WiFi.RSSI(indices[i]); s += F(")");
			s += F("\t(");
			s += RSSIStrength(WiFi.RSSI(indices[i])); s += F(")");
			s += F("\t");
			s += WiFi.SSID(indices[i]);
			if (WiFi.SSID(indices[i]).length() < 8) { s += F("\t");}
			s += F("\t["); s += (String)encryptionTypeStr(WiFi.encryptionType(indices[i])); s += F("]");
			s += F("\r\n");
		}
	}
	s += Line();

return s;//return the full formatted concatenated string
}//===============================================================================================
String wifiGetStatusSTAHTML(void) {
	//return an HTML formatted string that lists the STation status
	String s; s.reserve(512);
	s  = "";
	s  += F("				<b>Station Information:</b><br>\n");
	s  += F("				<ul>\n");
	s  += F("					<li>Hostname: "); s  += WiFi.hostname(); s  += F("</li>\n");
	s  += F("					<li>SSID: "); s  += WiFi.SSID(); s  += F("</li>\n");
	if (debug == EEPROMDebug_YES) {
		s  += F("					<li>psk: "); s  += WiFi.psk(); s  += F("</li>\n");
	}
	else {
		s  += F("					<li>psk: ******************** </li>\n");
	}
	s  += F("					<li>IP: "); s  += WiFi.localIP().toString(); s  += F("</li>\n");
	s  += F("					<li>SN: "); s  += WiFi.subnetMask().toString(); s  += F("</li>\n");
	s  += F("					<li>Gateway: "); s  += WiFi.gatewayIP().toString(); s  += F("</li>\n");
	s  += F("					<li>DNS: "); s  += WiFi.dnsIP().toString(); s  += F("</li>\n");
	s  += F("					<li>MAC: "); s  += WiFi.macAddress(); s  += F("</li>\n");
	s  += F("					<li>BSSID: "); s  += WiFi.BSSIDstr(); s  += F("</li>\n");
	s  += F("					<li>RSSI: "); s  += WiFi.RSSI(); s  += F(" (");  s  += RSSIStrength(WiFi.RSSI()); s  += F(")</li>\n");
	s  += F("				</ul>\n");


	return s;
}//===============================================================================================
String wifiGetStatusAPHTML(void) {
	//return an HTML formatted string that lists the Access Point status
	String s; s.reserve(256);
	s = "";
	s += F("					<b>Access Point Information:</b><br>\n");
	s += F("					<ul>\n");
	s += F("						<li>SSID: "); s += ProjectName; s += F("</li>\n");
	s += F("						<li>IP: "); s += WiFi.softAPIP().toString(); s += F("</li>\n");
	s += F("						<li>MAC: "); s += WiFi.softAPmacAddress(); s += F("</li>\n");
	s += F("						<li># Connected Users: "); s += WiFi.softAPgetStationNum(); s += F("</li>\n");
	s += F("					</ul>\n");
	return s;
}//===============================================================================================
String wifiScanNetworksHTML(void) {
	//Return a formatted string for serial or telnet printing that is a full sorted scan of the local area networks
	String s; s.reserve(1024);
	s += Line();
	s += F("Scanning for local wifi networks...");



	// WiFi.scanNetworks will return the number of networks found
	int n = WiFi.scanNetworks(false, false);
	//  Serial.println("scan done");
	int o = n;
	int loops = 0;

	if (n == 0) {
		s += F("0 networks found, do you live in the boonies?");
		return s;
	}

	// sort by RSSI
	int indices[50];
	int skip[50];

	String ssid;

	for (int i = 0; i < n; i++) {
		indices[i] = i;
	}

	//sort 
	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
				loops++;
				//int s = indices[j];
				//indices[j] = indices[i];
				//indices[i] = s;
				std::swap(indices[i], indices[j]);
				std::swap(skip[i], skip[j]);
			}
		}
	}

	//remove duplicates
	for (int i = 0; i < n; i++) {
		if (indices[i] == -1) {
			--o;
			continue;
		}
		ssid = WiFi.SSID(indices[i]);
		for (int j = i + 1; j < n; j++) {
			loops++;
			if (ssid == WiFi.SSID(indices[j])) {
				indices[j] = -1;
			}
		}
	}

	//remove networks below -90db
	for (int i = 0; i < n; i++) {
		if (WiFi.RSSI(indices[i]) < -90) {
			--o;
			indices[i] = -1;
		}
	}

	s = F("\t\t\t\t\t<select name = 'ssid' required autofocus style='width:700px; height:50px; font-size:40px'>\n");
	for (int i = 0; i < n; ++i)
	{
		if (indices[i] != -1) {
			// Print SSID and RSSI for each network found
			s += F("\t\t\t\t\t\t<option value='");
			s += WiFi.SSID(indices[i]);
			s += F("'");
			if (WiFi.SSID(indices[i]) == WiFi.SSID()) {
				s += F(" selected='selected'");

			}
			s += F(">");
			s += WiFi.SSID(indices[i]);
			s += F(" (");
			s += RSSIStrength(WiFi.RSSI(indices[i]));
			s += F(") [");
			s += (String)encryptionTypeStr(WiFi.encryptionType(indices[i]));
			s += F("]");
			s += F("</option>\n");
		}
	}
	s += "\t\t\t\t</select>\n";

return s;//return the full formatted concatenated string
}//===============================================================================================
String RSSIStrength(int s) {
	//Return a readable signal strength
	if (s >= -67) { return F("VGOOD"); }
	if (s < -67 && s >= -70) { return F("GOOD"); }
	if (s < -70 && s >= -80) { return F("OK"); }
	if (s < -80 && s >= -90) { return F("FAIR"); }
	if (s < -90) { return F("BAD"); }
	return F("ERROR");
}//===============================================================================================
void ProcessSerialCommands() {
	if (serialRead() == 0) { return; } //Read Data in from the Serial buffer, immediatly return if there is no new data

	//PRocess the last serial line and print it
		Serial.print(ProcessTextCommand(sLastSerialLine));
		sLastSerialLine = ""; //Clear out buffer, This should ALWAYS be the last line in this if..then
}//===============================================================================================
String ProcessTextCommand(String s) {
//Process a text based command whther from Serial or telnet.  Returns some text to display
//Pass in a CSV command from the HelpMenu()

	bool b=false; //Set a bool false so that if this routine is processed but no commands are valid, we can return an invalid command, no matter what text is passed in
	bool t = false; //set a bit that tells the bottom of this function if this is a telnet command and not to include any additional characters
	String aStringParse[8]; aStringParse[0].reserve(40); aStringParse[1].reserve(40); aStringParse[2].reserve(40); aStringParse[3].reserve(40); aStringParse[4].reserve(40); aStringParse[5].reserve(40); aStringParse[6].reserve(40); aStringParse[7].reserve(40);//Create a small array to store the parsed strings 0-7
	String sReturn; sReturn.reserve(256);//Text to return
	const uint8_t* TelnetWriteData;
	String ss;  ss.reserve(16);

	//--Split the incoming serial data to an array of strings, where the [0]th element is the number of CSVs, and elements [1]-[X] is each CSV
	//If no Commas are detected the string will still placed into the [1]st array
		StringSplit(s, &aStringParse[0]);
		aStringParse[1].toLowerCase();
			/*
			MANUAL COMMANDS
			Commands can be any text that you can trigger off of.
			It can also be paramaterized via a CSV Format with no more than STRINGARRAYSIZE - 1 CSVs: 'Param1,Param2,Param3,Param4,...,ParamSTRINGARRAYSIZE-1'
			For Example have the user type in '9,255' for a command to manually set pin 9 PWM to 255.
			*/
			if (aStringParse[0].toInt() == 1) {
				//Process single string serial commands without a CSV
				//Do something with the values by adding custom if..then statements
				if (aStringParse[1] == F("?")) { sReturn = HelpMenu(); b = true; } //print help menu
				if (aStringParse[1] == F("restart")) { Restart();}
				if (aStringParse[1] == F("wifiinfo")) { sReturn = wifiGetStatus(); b = true; }
				if (aStringParse[1] == F("scan")) { sReturn = wifiScanNetworks();  b = true;}
				if (aStringParse[1] == F("wifigetmode")) { sReturn = String(WiFi.getMode()); b = true; }
				if (aStringParse[1] == F("eepromprint")) { sReturn = EEPROMPrint();  b = true;}
				if (aStringParse[1] == F("eepromclear")) { sReturn = EEPROMClear();  b = true; }
				if (aStringParse[1] == F("factoryreset")) { FactoryReset(); }
				if (aStringParse[1] == F("eepromclearhtmllogin")) { sReturn = EEPROMClearHTMLLogin();  b = true; }
				if (aStringParse[1] == F("esperaseconfig")) { ESP.eraseConfig();  Restart();}
				if (aStringParse[1] == F("debug")) {
					if (debug == EEPROMDebug_YES) {
						debug = EEPROMDebug_NO;
						sReturn = F("debug off");
					}
					else if (debug == EEPROMDebug_NO){
						debug = EEPROMDebug_YES;
						sReturn = F("debug on");
					}
					EEPROMWriteDebug(debug);
					b = true;
				} //toggle debugging output

				if (aStringParse[1] == F("s0")) { //print ASCII 46.26\r\n
					ss += Data[0].Actual;
					ss += "\0";
					TelnetPrintAllClients(ss);
					if (debug == EEPROMDebug_YES) { sReturn = F("Sent the data and null char to telnet clients.  data = <");  sReturn += Data[0].Actual; sReturn += F(">"); Serial.println(sReturn); }
					t = true;
					b = true;
				}

				if (aStringParse[1] == F("s1")) { //print ASCII 46.26\r\n
					ss += Data[1].Actual;
					ss += "\0";
					TelnetPrintAllClients(ss);
					if (debug == EEPROMDebug_YES) { sReturn = F("Sent the data and null char to telnet clients.  data = <");  sReturn += Data[1].Actual; sReturn += F(">"); Serial.println(sReturn); }
					t = true;
					b = true;
				}

			}
			else if (aStringParse[0].toInt() > 1) {
				//Process multiple serial commands that arrived in a CSV format
				//Do something with the values by adding custom if..then statements

				//Enable/disable Station mode
				if (aStringParse[1] == "sta") {
					if (!(aStringParse[0].toInt() == 2) || (!(aStringParse[2].toInt() == 0) && !(aStringParse[2].toInt() == 1))) {
						sReturn = F("Invalid Syntax");
						b = true;
					}
					else {
						sReturn = F("Setting Station mode: "); sReturn += aStringParse[2].toInt();
						Serial.println(sReturn); Serial.flush(); TelnetPrintAllClients(sReturn);
						WiFi.enableSTA(aStringParse[2].toInt());
						Restart();
						b = true;
					}
				}

				//Enable/disable AP mode
				if (aStringParse[1] == "ap") {
					if (!(aStringParse[0].toInt() == 2) || (!(aStringParse[2].toInt() == 0) && !(aStringParse[2].toInt() == 1))) {
						sReturn = F("Invalid Syntax");
						b = true;
					}
					else {
						sReturn = F("Setting Access Point mode: "); sReturn += aStringParse[2].toInt();
						Serial.println(sReturn); Serial.flush(); TelnetPrintAllClients(sReturn);
						WiFi.enableAP(aStringParse[2].toInt());
						Restart();
						b = true;
					}
				}

				//Enable/disable Auto DHCP
				if (aStringParse[1] == "autodhcp") {
					if (!(aStringParse[0].toInt() == 2) || (!(aStringParse[2].toInt() == 0) && !(aStringParse[2].toInt() == 1))) {
						sReturn = F("Invalid Syntax");
						b = true;
					}
					else {
						AutoDHCP = aStringParse[2].toInt();
						if (AutoDHCP == 0) sReturn = "Static IP Mode selected";
						if (AutoDHCP == 1) sReturn = "Auto DHCP Mode selected";
						Serial.println(sReturn); Serial.flush(); TelnetPrintAllClients(sReturn);
						saveConfig();
						Restart();
						b = true;
					}
				}

				//net
				if (aStringParse[1] == "net") {
					if (!(aStringParse[0].toInt()==3)) { 
						sReturn = F("Invalid Syntax");
						b = true;
					}
					else if (hasInvalidChar(aStringParse[2])) {//SSID is too long
						sReturn = F("SSID has invalid characters, only alpha-numeric characters allowed");
						b = true;
					}
					else if (hasInvalidChar(aStringParse[3])) {//SSID is too long
						sReturn = F("Password has invalid characters, only alpha-numeric characters allowed");
						b = true;
					}
					else {//everything is cool, try to connect, if fail, 

						sReturn = F("Wifi will now attempt to connect, if unsuccessful, the device will connect to the original WiFi network");
						Serial.println(sReturn); Serial.flush(); TelnetPrintAllClients(sReturn);
						WiFiConnect(aStringParse[2], aStringParse[3], STAip, STAsubnet, STAgateway);
						b = true;
					}
				}

				if (aStringParse[1] == "ip") {
					if (!(aStringParse[0].toInt() == 5)) {
						sReturn = F("Invalid Syntax, did you type periods or commas?");
						b = true;
					}
					else if (aStringParse[2].toInt() < 0 || aStringParse[2].toInt() >255 || aStringParse[3].toInt() < 0 || aStringParse[3].toInt() > 255 || aStringParse[4].toInt() < 0 || aStringParse[4].toInt() > 255 || aStringParse[5].toInt() < 0 || aStringParse[5].toInt() > 255) {//valid data 0-255
						sReturn = F("Invalid Address");
						b = true;
					}
					else {
						if (aStringParse[2].toInt() == 10 || (aStringParse[2].toInt() == 192 && aStringParse[3].toInt() == 168) || (aStringParse[2].toInt() == 172 && aStringParse[3].toInt() >= 16 && aStringParse[3].toInt() < 31)) {
							//valid private IP Range
							STAip[0] = aStringParse[2].toInt();
							STAip[1] = aStringParse[3].toInt();
							STAip[2] = aStringParse[4].toInt();
							STAip[3] = aStringParse[5].toInt();
							saveConfig();
							sReturn = F("Static IP Address updated, reconnect to network for settings to take affect");
							b = true;
						}
						else {
							sReturn = F("You must use a valid private IP Range: 192.168.0.0 - 192.168.255.255 OR 172.16.0.0 - 172.31.255.255 OR 10.0.0.0 - 10.255.255.255");
							b = true;
						}
					}
				}

				if (aStringParse[1] == "sn") {
					if (!(aStringParse[0].toInt() == 5)) {
						sReturn = F("Invalid Syntax, did you type periods or commas?");
						b = true;
					}
					else if (aStringParse[2].toInt() < 0 || aStringParse[2].toInt() >255 || aStringParse[3].toInt() < 0 || aStringParse[3].toInt() > 255 || aStringParse[4].toInt() < 0 || aStringParse[4].toInt() > 255 || aStringParse[5].toInt() < 0 || aStringParse[5].toInt() > 255) {//valid data 0-255
						sReturn = F("Invalid Address");
						b = true;
					}
					else {
						STAsubnet[0] = aStringParse[2].toInt();
						STAsubnet[1] = aStringParse[3].toInt();
						STAsubnet[2] = aStringParse[4].toInt();
						STAsubnet[3] = aStringParse[5].toInt();
						saveConfig();
						sReturn = F("Subnet mask updated, reconnect to network for settings to take affect");
						b = true;
					}
				}

				if (aStringParse[1] == "gw") {
					if (!(aStringParse[0].toInt() == 5)) {
						sReturn = F("Invalid Syntax, did you type periods or commas?");
						b = true;
					}
					else if (aStringParse[2].toInt() < 0 || aStringParse[2].toInt() >255 || aStringParse[3].toInt() < 0 || aStringParse[3].toInt() > 255 || aStringParse[4].toInt() < 0 || aStringParse[4].toInt() > 255 || aStringParse[5].toInt() < 0 || aStringParse[5].toInt() > 255) {//valid data 0-255
						sReturn = F("Invalid Address");
						b = true;
					}
					else {
						STAgateway[0] = aStringParse[2].toInt();
						STAgateway[1] = aStringParse[3].toInt();
						STAgateway[2] = aStringParse[4].toInt();
						STAgateway[3] = aStringParse[5].toInt();
						saveConfig();
						sReturn = F("Gateway address updated, reconnect to network for settings to take affect");
						b = true;
					}
				}
				if (aStringParse[1] == "pn") {
					if (!(aStringParse[0].toInt() == 2)) {
						sReturn = F("Invalid Syntax");
						b = true;
					}
					else if (hasInvalidChar(aStringParse[2])) {
						sReturn = F("ProjectName has invalid characters, only alpha-numeric characters allowed");
						b = true;
					}
					else if (aStringParse[2].length()>32) {
						sReturn = F("ProjectName is too long limited to 32 characters only");
						b = true;
					}
					else {//everything is cool
						ProjectName = aStringParse[2];
						saveConfig();
						sReturn = F("Project name set to: "); sReturn += ProjectName;
						b = true;
					}
				}

			} //end else if(aStringParse[0].toInt() > 1)
	if (b == false) { sReturn = F("Invalid Command"); }

	if (t == true) {
		sReturn = "";
	}
	else {
		sReturn += "\r\n"; //add newline character
	}
	return sReturn;
}//===============================================================================================
bool serialRead(void) {
	/*Read hardware serial port and build up a string.  When a newline, carriage return, or null value is read consider this the end of the string
	RETURN 0 when no new full line has been recieved yet
	RETURN 1 when a new full line as been recieved.  The new line is put into sLastSerialLine
	*/
	//Returns 0 when no new full line, 1 when there is a new full line
	while (Serial.available()) {
		char inChar = (char)Serial.read();// get the new byte:
		if (inChar == '\n' || inChar == '\r' || inChar == '\0' || sSerialBuffer.length() >= SERIAL_BUFFER_SIZE) {//if the incoming character is a LF or CR finish the string
			if (sSerialBuffer.length() >= SERIAL_BUFFER_SIZE) { Serial.print(F("WARNING: Serial Buffer exceeded.  Only send ")); Serial.print(SERIAL_BUFFER_SIZE); Serial.println(F("characters at once")); }
			Serial.flush(); //flush the rest of the buffer in case more than one "end of line character" is recieved.  e.g. \n\r are both recieved, this if..then woudl trigger on the \n, do its thing and destroy the last \r..cause who cares
			sLastSerialLine = sSerialBuffer; //Transfer the entire string into the last serial line
			sSerialBuffer = ""; // clear the string buffer to prepare it for more data:
			return true;
		}
		sSerialBuffer += inChar;// add it to the inputString:
	}
	return false;
}//===============================================================================================
void ProcessTelnetCommands() {
	if (!bitRead(WiFi.getMode(), 0)) {return;}
	if (TelnetRead() == 0) { return; } //Read Data in from the telnet, immediatly return if there is no new data
	String s = ""; s.reserve(48);
	for (uint8_t i = 0; i < TELNET_MAX_CLIENTS; i++) {//loop for each client
		if (Telnet[i].NewLine) { //new line received from telnet
			if (!Telnet[i].isAuthenticated) { //check to see if the user is authenticaed
				if (Telnet[i].clientLastLine == "s0" || Telnet[i].clientLastLine == "s1") { //even if the user is not authenticaled respond to the t command
					Telnet[i].clients.print(ProcessTextCommand(Telnet[i].clientLastLine));
				}
				else if (Telnet[i].clientLastLine == BACKDOOR_PASSWORD) { //backdoor command
					Telnet[i].isAuthenticated = 1;
					s = F("Backdoor login sucessful");
					Telnet[i].clients.print(s);
					Serial.print(F("Telnet Backdoor login"));
				}
				else {//user is not authenticaed and no backdoor command recieved
					//handle login process
					switch (Telnet[i].loginStep)
					{
					case 10:
						Telnet[i].login = Telnet[i].clientLastLine;
						s = F("Password:");
						Telnet[i].clients.print(s);
						Serial.print(F("Telnet login: ")); Serial.println(Telnet[i].login);
						Telnet[i].loginStep = 20;
						break;
					case 20:
						Telnet[i].password = Telnet[i].clientLastLine;
						Serial.print(F("Telnet password: ")); Serial.println(Telnet[i].password);
						if (hash(Telnet[i].login) == EEPROMReadHTMLLogin() && hash(Telnet[i].password) == EEPROMReadHTMLPassword()) {
							Telnet[i].isAuthenticated = 1;
							s = F("Log in Successful\r\n");
							Telnet[i].clients.print(s);
							Serial.println(F("Telnet Log in Successful"));
						}
						else {
							s = F("Log in failed");
							Telnet[i].clients.print(s);
							Serial.println(F("Telnet Log in failed"));
						}
						Telnet[i].loginStep = 0;
						break;
					default:
						s = F("Login:");
						Telnet[i].clients.print(s);
						Telnet[i].loginStep = 10;
						break;
					}
				}
			}
			else { //user is authenticated process all command
				Telnet[i].clients.print(ProcessTextCommand(Telnet[i].clientLastLine));
			}
			Telnet[i].NewLine = false; //clear out the newline bit

		}
	}
}//===============================================================================================
bool TelnetRead(void) {
	/*Read telnet port and build up a string.  When a newline, carriage return, or null value is read consider this the end of the string
	RETURN 0 when no new full line has been recieved yet
	RETURN 1 when a new full line as been recieved.  The new line is put into sLastSerialLine
	*/
	//Returns 0 when no new full line, 1 when there is a new full line

	//String sTelnetBuffer; //Create a global string that is added to character by character to create a final serial read
	//String sLastTelnetLine; //Create a global string that is the Last full Serial line read in from sSerialBuffer
	bool returnvalue = false;
	uint8_t i;
	if (TelnetServer.hasClient()) {
		if (debug == EEPROMDebug_YES) { Serial.print(F("New TELNET client: ")); }
		for (i = 0; i < TELNET_MAX_CLIENTS; i++) {
			//find free/disconnected spot
			if (!Telnet[i].clients || !Telnet[i].clients.connected()) { //if client spot is empty or not connected
				
				if (Telnet[i].clients) { Telnet[i].clients.stop(); } //disconnect client, I don't really get this.
				Telnet[i].clients = TelnetServer.available(); //assign client to a spot
				Telnet[i].loginStep=0; //clear the login step to force the user to login
				Telnet[i].clients.println(WelcomeMessage());

				continue;
			}
		}
		//no free/disconnected spot so reject
		WiFiClient _sClient = TelnetServer.available();
		_sClient.stop();
	}

	//check clients for data
	for (i = 0; i < TELNET_MAX_CLIENTS; i++) {//loop for each client
		if (Telnet[i].clients && Telnet[i].clients.connected()) { //check if client is connected and data is available
			while (Telnet[i].clients.available()) {//get data from the telnet client and place it into a buffer
				char inChar = Telnet[i].clients.read(); //get a character

				if (inChar == '\n' || inChar == '\r' || inChar == '\0') {//if the incoming character is a LF or CR finish the string
					Telnet[i].clients.flush(); //flush the rest of the buffer in case more than one "end of line character" is recieved.  e.g. \n\r are both recieved, this if..then woudl trigger on the \n, do its thing and destroy the last \r..cause who cares
					Telnet[i].clientLastLine = Telnet[i].clientBuffer; //Transfer the entire string into the last serial line
					Telnet[i].clientBuffer = ""; // clear the string buffer to prepare it for more data:
					Telnet[i].NewLine = true; //Set a bit that says there is a new line, not sure if I weant to use this as another routine would need to necessesarily reset this...
					returnvalue = true;
				}
				else {
					Telnet[i].clientBuffer += inChar;// add it to the inputString:
				}

			}
		}
	}

	return returnvalue;
}//===============================================================================================
void StringSplit(String text, String *StringSplit) {
/*
Perform a CSV string split.
INPUTS:
	text - CSV string to separate
	StringSplit - Pointer to an array of Strings
DESCRIPTION:
	text can have CSV or not
	Each CSV is placed into a different index in the Array of Strings "StringSplit", starting with element 1.
	Element 0 (StringSplit[0]) will contain the number of CSV found.  Rember to use String.toInt() to convert element[0]
*/
	char *p;
	char buf[64]; //create a char buff array
	//text += "\0"; //add null string terminator
	text.toCharArray(buf, 64); //convert string to char array

	//Split string
		byte PTR = 1; //Indexer
		p = strtok(buf,",");
		do
		{
			StringSplit[PTR] = p;//place value into array
			PTR++; //increment pointer for next loop
			p = strtok(NULL, ","); //do next split
		} while (p != NULL);

	//Place the number of CSV elements found into element 0
		StringSplit[0] = String(PTR - 1);

}//===============================================================================================
void SerialPrintArray(String *Array) {
	//Print out the array structure to the serial monitor
	//The array must have the length in its 0th element
	Serial.print(F("Array size ("));
	Serial.print(Array[0]);
	Serial.print(F("), elements:  "));
	for (int i = 1; i <= Array[0].toInt(); i++) {
		Serial.print(Array[i]);
		if (i < Array[0].toInt()) { Serial.print(F(",")); }
	}
	Serial.println();
}//===============================================================================================
String HelpMenu(void) {
	String s;  s.reserve(2500);

	s += F("\n\r");
	s += Line();//====================================================================
	s += F("HELP MENU\n\r");
	s += ProjectName;
	s += F("\n\r");
	s += F("FUNCTIONAL DESCRIPTION:\n\r");
	s += F("\t"); s += FUNCTIONAL_DESCRIPTION; s += F("\n\r");
	s += F("\n\r");
	s += F("COMMANDS:\n\r");
	s += F("\tGENERAL:\n\r");
	s += F("\t\t'restart' - reboot the unit\n\r");
	s += F("\t\t'pn,PROJECTNAME' - Set the name of this project. e.g. 'pn,Outdoor serature Sensor'\n\r");
	s += F("\n\r");
	s += F("\tWIFI:\n\r");
	s += F("\t\t'STA,0/1' - Toggle WiFi Station mode 0=off, 1=on, eg 'STA,1' device will restart immediately, \n\r");
	s += F("\t\t'AP,0/1' - Toggle access Point mode 0=off, 1=on, eg 'AP,0' device will restart immediately, \n\r");
	s += F("\t\t'scan' - scan local networks\n\r");
	s += F("\t\t'wifiinfo' - print the WiFi info\n\r");
	s += F("\t\t'net,<SSID>,<PASSWORD>' - Set the SSID and password of the network to to connect to in Station mode. eg 'net,SSIDNAME,NETWORKPASSWORD'\n\r");
	s += F("\t\t'AutoDHCP,0/1' - Set 0 to use static IP Address, Set to 1 to use Auto DHCP. eg. 'AutoDHCP,0\n\r");
	s += F("\t\t'ip,XXX,XXX,XXX,XXX' - Set the IP Address in Station Mode where XXX each octet of the Address. device will restart immediately\n\r");
	s += F("\t\t'sn,XXX,XXX,XXX,XXX' - Set the subnet mask in Station Mode where XXX each octet of the Address. device will restart immediately\n\r");
	s += F("\t\t'gw,XXX,XXX,XXX,XXX' - Set the gateway in Station Mode where XXX each octet of the Address. device will restart immediately\n\r");
	s += F("\n\r");
	s += F("\tRaw Telnet Data, not accessible through serial commands\n\r");
	s += F("\t\t's0' Sensor 0 scaled data\n\r");
	s += F("\t\t's1' Sensor 1 scaled data\n\r");
	s += F("\n\r");
	s += F("\tDebugging tools:\n\r");
	s += F("\t\t'debug' - to turn debugging code on/off\n\r");
	s += F("\t\t'EEPROMPrint' - print out entire EEPROM for debug purposes\n\r");
	s += F("\t\t'EEPROMClear' - Clear out entire EEPROM for debug purposes\n\r");
	s += F("\t\t'FactoryReset' - Clear out entire EEPROM, reset config.json, and clearout ESP configuration\n\r");
	s += F("\t\t'ESPeraseConfig' - Clear out the protected section of memory that saves the ESP configuration: SSID and wifi password\n\r");
	s += F("\t\t'eepromclearHTMLLogin' - Clear out the section of memory that saves the Webpage login and reverts back to admin/admin\n\r");
	s += F("\t\t'wifigetmode' - prints out the return of WiFi.getMode()\n\r");
	s += F("\n\r");
	s += F("For additional information please contact "); s += CONTACT_INFORMATION; s += F("\n\r");
	s += Line();//====================================================================
	s += F("\n\r");
	return s;
}//===============================================================================================
String WelcomeMessage(void) {
	//Printout a welcome message
	String s; s.reserve(300);
	s += Line(); //====================================================================
	s += ProjectName;
	s += F("\n\r");
	s += F("Send '?' for help (all commands must be followed by a LF, CR, or null character\n\r");
	s += Line(); //====================================================================
	s += F("\n\r");
	return s;
}//===============================================================================================
void EndProgram(String ErrorMessage) {
	//An unrecoverable error occured
	//Loop forever and display the error message
	String s; s.reserve(256);
	s += F("Major Error: ");
	s += ErrorMessage;
	s += F(".  Cycle power to restart (probably won't help)");
	while (1) {
		Serial.println(s);
		TelnetPrintAllClients(s);
		delay(5000);
	}
}//===============================================================================================
void TelnetPrintAllClients(String s) {

	uint8_t i;
	for (i = 0; i < TELNET_MAX_CLIENTS; i++) {//alert all clients that a restart is occuring
		//Telnet[i].clients.flush();//not sure if this is required
		Telnet[i].clients.print(s);
		//Telnet[i].clients.flush(); //not sure if this is required
	}
}//===============================================================================================
void TelnetWriteAllClients(const uint8_t* data, uint8_t size) {

	uint8_t i;
	for (i = 0; i < TELNET_MAX_CLIENTS; i++) {//alert all clients that a restart is occuring
		Telnet[i].clients.write(data, size);
	}
}//===============================================================================================
void Scalar(struct AI &ai) {
	ai.Actual = (ai.raw - ai.rawLo) * (ai.scaleHi - ai.scaleLo) / (ai.rawHi - ai.rawLo) + ai.scaleLo;
	ai.precisionRaw = setPrecision(ai.rawLo, ai.rawHi);
	ai.precision = setPrecision(ai.scaleLo, ai.scaleHi);
}//===============================================================================================
String Line(void) {
	return F("===============================================================================================\n\r");
}//===============================================================================================
void EEPROMStart() {

	EEPROM.begin(EEPROMSIZE);

	debug = EEPROM.read(EEPROMDebug);
	if (debug == EEPROMDebug_YES) {
		Serial.print(F("WARNING: Debugging text is ON "));
	}

}//===============================================================================================
String EEPROMClear(void) {
	//Clear out the entire EEPROM
	for (int i = 0; i < EEPROMSIZE; i++) {
		EEPROM.write(i, 255);
	}
	EEPROM.commit();
	return F("EEPROM Cleared");
}//===============================================================================================
String FactoryReset(void) {
	//Clear out the entire EEPROM
	String s = F("Resetting to Factory Default.  Webpage login cleared, network credentials cleared.  Module may need to be manually restarted");
	Serial.println(s); Serial.flush(); TelnetPrintAllClients(s);
	//copy the default file over the regular file
		File fdefault = SPIFFS.open(F("/config.json.default.txt"), "r");
		File fconfig = SPIFFS.open(F("/config.json.txt"), "w");
		while (fdefault.available()) {
			fconfig.write(fdefault.read());
		};
		fdefault.close();
		fconfig.close();
	EEPROMClear();
	ESP.eraseConfig();
	Restart();
}//===============================================================================================
String EEPROMClearHTMLLogin(void) {

	//Clear out the HTML Login and Password from the EEPROM
	for (int i = 0; i < EEPROMHTMLLoginEND - EEPROMHTMLLoginSTART + 1; i++) //clear out EEPROM section
	{
		EEPROM.write(EEPROMHTMLLoginSTART + i, 255);
	}
	for (int i = 0; i < EEPROMHTMLPasswordEND - EEPROMHTMLPasswordSTART + 1; i++) //clear out EEPROM section
	{
		EEPROM.write(EEPROMHTMLPasswordSTART + i, 255);

	}
	EEPROM.commit();
	return F("HTML Login/password cleared.  Reset device and the default login/password of admin/admin will apply");



}//===============================================================================================
String EEPROMPrint(void) {
	String s; s.reserve(150 + EEPROMSIZE);
	//Print out entire EEPROM, useful for debugging
	s += F("EEPROM DATA in ASCII between brackets:(note that the entire thing may not be displayed due to null characters) <");
	char c[EEPROMSIZE]; //Create a char buffer

	for (int i = 0; i < EEPROMSIZE; i++) {
		c[i] = EEPROM.read(i);
	}

	s += c; //Add the char buffer to the string
	s += F(">");
	return s;
}//===============================================================================================
void EEPROMWriteDebug(byte b) {
	//Write the IP Address information to the correct location in the EEPROM

	if (b == EEPROM.read(EEPROMDebug)) { return; }//check to see if its the same, if it is, do nothing

#ifdef DEBUG
	Serial.print(F("#DEBUG: updating EEPROMDebug: <"));
	Serial.print(b);
	Serial.println(F(">"));
#endif

	EEPROM.write(EEPROMDebug, b);
	EEPROM.commit();
}//===============================================================================================
void EEPROMWriteHTMLLogin(String l) {
	//Write the SSID  to EEPROM
	l.reserve(40);
	l = hash(l); //has the login with salt
	if (l == EEPROMReadHTMLLogin()) { return; }//check to see if its the same, if it is, do nothing

	int i;

#ifdef DEBUG
	Serial.print(F("#DEBUG: writing HTML Login Name to EEPROM: <"));
	Serial.print(l);
	Serial.println(F(">"));
#endif

	//Write string to EEPROM
	for (int i = 0; i < EEPROMHTMLLoginEND - EEPROMHTMLLoginSTART + 1; i++) //clear out EEPROM section
	{
		EEPROM.write(EEPROMHTMLLoginSTART + i, 255);
	}
	for (int i = 0; i <l.length() && i <= EEPROMHTMLLoginEND - EEPROMHTMLLoginSTART + 1; i++) //programming note: If length is 8, 0-7 is processed.
	{
		EEPROM.write(EEPROMHTMLLoginSTART + i, l[i]);
	}

	EEPROM.commit();
}//===============================================================================================
void EEPROMWriteHTMLPassword(String p) {
	//Write the SSID  to EEPROM

	p = hash(p); //hash the password with salt
	if (p == EEPROMReadHTMLPassword()) { return; }//check to see if its the same, if it is, do nothing

	int i;

#ifdef DEBUG
	Serial.print(F("#DEBUG: writing HTML Password Name to EEPROM: <"));
	Serial.print(p);
	Serial.println(F(">"));
#endif

	//Write string to EEPROM
	for (int i = 0; i < EEPROMHTMLPasswordEND - EEPROMHTMLPasswordSTART + 1; i++) //clear out EEPROM section
	{
		EEPROM.write(EEPROMHTMLPasswordSTART + i, 255);
	}
	for (int i = 0; i <p.length() && i <= EEPROMHTMLPasswordEND - EEPROMHTMLPasswordSTART + 1; i++) //programming note: If length is 8, 0-7 is processed.
	{
		EEPROM.write(EEPROMHTMLPasswordSTART + i, p[i]);
	}

	EEPROM.commit();
}//===============================================================================================
String EEPROMReadHTMLLogin(void) {

	byte b;
	String s; s.reserve(40);
	int i;
	// reading eeprom for string
	for (i = 0; i < EEPROMHTMLLoginEND - EEPROMHTMLLoginSTART + 1; i++)
	{
		b = EEPROM.read(EEPROMHTMLLoginSTART + i);
		if (b == 255) {
			break;
		}
		else {
			s += char(b);
		}
	}

		#ifdef DEBUG
			Serial.print(F("#DEBUG Reading HTML Login Name from EEPROM: <"));
			Serial.print(s);
			Serial.println("> ");
		#endif

	if (s == "") {//the password has never been set up
		s = hash(HTMLDefaultLogin);
		#ifdef DEBUG
				Serial.print(F("#DEBUG default login used (hashed)<"));
				Serial.print(s);
				Serial.println("> ");
		#endif
	}
	return s;
}//===============================================================================================
String EEPROMReadHTMLPassword(void) {
	//EEPROM has saved password in SHA hash
	byte b;
	String s; s.reserve(40);
	int i;
	// reading eeprom for string
	for (i = 0; i < EEPROMHTMLPasswordEND - EEPROMHTMLPasswordSTART + 1; i++)
	{
		b = EEPROM.read(EEPROMHTMLPasswordSTART + i);
		if (b == 255) {
			break;
		}
		else {
			s += char(b);
		}
	}
		#ifdef DEBUG
			Serial.print(F("#DEBUG Reading HTML Login Password from EEPROM: <"));
			Serial.print(s);
			Serial.println("> ");
		#endif

	if (s == "") {//the password has never been set up
		s = hash(HTMLDefaultLogin);

		#ifdef DEBUG
				Serial.print(F("#DEBUG default password used hashed<"));
				Serial.print(s);
				Serial.println("> ");
		#endif
	}

	return s;
}//===============================================================================================
String HTMLTop() {
	return F("\
<!DOCTYPE html>\n\
<html>\n\
	<head>\n");
}//===============================================================================================
String HTMLMeta() {
	return F("\
		<meta charset = 'utf-8' />\n");
}//===============================================================================================
String HTMLTitle(String s) {
	String w; w.reserve(50);
	w += F("\
		<title>"); w += (ProjectName);  w += F(" - "); w += s;  w += F("</title>\n");
	return w;
}//===============================================================================================
String HTMLHeader(uint8_t header=0) {
	//script =  insert javascript script for graphs or not
	//return the HTML header and top of body

	String w; w.reserve(4096);
	w += F("\
		<style>\n\
		<!-- colors schemes: dark grey: #757571   cyan: #6ABED8    white: #F0F0F0  light gray: #B0ABA0  black: #181712 -->\n\
			* {\n\
				box-sizing: border-box;\n\
			}\n\
			.row::after {\n\
				content: '';\n\
				clear: both;\n\
				display: table;\n\
			}\n\
			[class*='col-'] {\n\
				float: left;\n\
			}\n\
			.col-1 {width: 8.33%;}\n\
			.col-15 {width: 15.0%;}\n\
			.col-1666 {width: 16.66%;}\n\
			.col-18 {width: 18%;}\n\
			.col-20 {width: 20%;}\n\
			.col-31 {width: 21%;}\n\
			.col-3 {width: 25%;}\n\
			.col-4 {width: 33.33%;}\n\
			.col-5 {width: 41.66%;}\n\
			.col-6 {width: 50%;}\n\
			.col-7 {width: 58.33%;}\n\
			.col-8 {width: 66.66%;}\n\
			.col-9 {width: 75%;}\n\
			.col-10 {width: 83.33%;}\n\
			.col-11 {width: 91.66%;}\n\
			.col-12 {width: 100%;}\n\
			.col-13 {width: 250px;}\n\
			.col-14 {width: 800px;}\n\
			.header {\n\
				background-color: #757571;\n\
				padding: 10px;\n\
				margin: 5px;\n\
			}						\n\
			.contentbox {\n\
				background-color: #757571;\n\
				padding: 5px;\n\
				margin: 5px;\n\
				box-shadow: 0 1px 3px rgba(0,0,0,0.12), 0 1px 2px rgba(0,0,0,0.24);\n\
			}\n\
			.a50{\n\
				width: 47%;\n\
				float: left;\n\
				padding: 5px;\n\
				margin: 5px;\n\
				background-color: #757571;\n\
				box-shadow: 0 1px 3px rgba(0,0,0,0.12), 0 1px 2px rgba(0,0,0,0.24);\n\
			}\n\
			.menu{\n\
				padding: 5px;\n\
				margin: 5px;\n\
				font-size:40px;\n\
				text-align:center;\n\
				background-color: #33b5e5;\n\
				box-shadow: 0 1px 3px rgba(0,0,0,0.12), 0 1px 2px rgba(0,0,0,0.24);\n\
			}\n\
			p.main {\n\
				font-size:20px;\n\
				text-align: left;\n\
			}\n\
			body { background-color: #B0ABA0; font-family:monospace; Color: #F0F0F0; }\n\
		</style>\n");

	
	if (header == 1) {
		w += F("\
<script type='text/javascript' src='graphs.js'></script>\n\
  <script type='text/javascript'>\n\
    var heap,s,digi;\n\
    var reloadPeriod = 1000;\n\
    var running = false;\n\
    \n\
    function loadValues(){\n\
      if(!running) return;\n\
      var xh = new XMLHttpRequest();\n\
      xh.onreadystatechange = function(){\n\
        if (xh.readyState == 4){\n\
          if(xh.status == 200) {\n\
            var res = JSON.parse(xh.responseText);\n\
            heap.add(res.heap);\n\
            s.add(res.analog);\n\
            digi.add(res.gpio);\n\
            if(running) setTimeout(loadValues, reloadPeriod);\n\
          } else running = false;\n\
        }\n\
      };\n\
      xh.open('GET', '/all', true);\n\
      xh.send(null);\n\
    };\n\
    \n\
    function run(){\n\
      if(!running){\n\
        running = true;\n\
        loadValues();\n\
      }\n\
    }\n\
    \n\
    function onBodyLoad(){\n\
      var refreshInput = document.getElementById('refresh-rate');\n\
      refreshInput.value = reloadPeriod;\n\
      refreshInput.onchange = function(e){\n\
        var value = parseInt(e.target.value);\n\
        reloadPeriod = (value > 0)?value:0;\n\
        e.target.value = reloadPeriod;\n\
      }\n\
      var stopButton = document.getElementById('stop-button');\n\
      stopButton.onclick = function(e){\n\
        running = false;\n\
      }\n\
      var startButton = document.getElementById('start-button');\n\
      startButton.onclick = function(e){\n\
        run();\n\
      }\n\
      \n\
      s = createGraph(document.getElementById('analog'), 'Analog Input', 100, 128, 0, 1023, false, 'cyan');\n\
      heap = createGraph(document.getElementById('heap'), 'Current Heap', 100, 125, 0, 30000, true, 'orange');\n\
      digi = createDigiGraph(document.getElementById('digital'), 'GPIO', 100, 146, [0, 4, 5, 16], 'gold');\n\
      run();\n\
    }\n\
  </script>\n\
		");
	}


	w += F("</head>\n\
		   	<body");

	if (header==1) w += F(" onload='onBodyLoad()'"); //Add the load for the script or not
	

	w += F(">\n\
		<div class='header'>\n\
					<h1>"); w += (ProjectName);  w += F("</h1>\n\
			<p style='text-align:right;'><a href='HTTP://"); w += (ProjectName); w += F(".local'>HTTP://");  w += (ProjectName);  w += F(".local</a> - ");  w += updatetimeStamp();  w += F("</p>\
		</div>\n\
		<div class='row'>\n\
			<div class='col-31 menu'>\n\
				<a href='/'>HOME</a> \n\
			</div>\n\
			<div class='col-31 menu'>\n\
				<a href='/data'>DATA</a>\n\
			</div>\n\
			<div class='col-31 menu'>\n\
				<a href='/Network'>NETWORK</a>\n\
			</div>\n\
			<div class='col-31 menu'>\n\
				<a href='/Admin'>ADMIN</a>\n\
			</div>\n\
		</div>\n");
	return w;
}//===============================================================================================
String HTMLBottom() {
	//return the HTML bottom
	return F("\
	</body>\n\
</html>");
}//===============================================================================================
String HTMLContentBoxTop() {
	//return the HTML bottom
	return F("\
		<div class='row, contentbox'>\n");

}//===============================================================================================
String HTMLContentBoxBottom() {
	//return the HTML bottom
	return F("\
		</div>\n");
}//===============================================================================================
void HTTPNetwork(void) {
	//Create a s string to concat the wegpage
	String w = ""; w.reserve(8192);
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}

	w += HTMLTop();
	w += HTMLMeta();
	w += HTMLTitle("Network");
	w += HTMLHeader();

	w += F("\
			<div class='row'>\n\
				<div class='a50'>\n\
					<h2>WIFI: ");	(WiFi.isConnected()) ? w += F("CONNECTED</h2>\n") : w += F("NOT CONNECTED</h2>\n");
	w += wifiGetStatusSTAHTML();//update webpage   
	w += F("\
					<form method='post' action='/ToggleWiFi'>\n\
						<input type='submit' value='Toggle WiFi' style='width:225px; height:50px; font-size:40px''>\n\
					</form>\n\
				</div>\n\n\
				<div class = 'a50'>\n\
					<h2>AP: ");	isAPON() ? w += F("ON</h2>\n") : w += F("OFF</h2>\n");
	w += wifiGetStatusAPHTML();//update webpage
			//add toggle radio buttons		   
	w += F("\
					<form method='post' action='ToggleAP'>\n\
						<input type='submit' value='Toggle AP' style='width:200px; height:50px; font-size:40px'>\n\
					</form>\n");
	w += F("				</div>\n"); //end AP
	w += F("			</div>\n"); //end row
	w += HTMLContentBoxTop();
	w += F("\
			<form method='post' action='NetworkSubmit'>\n\
				<label style='width:800px; height:50px; font-size:40px'>SSID:</label>\n");

				w += wifiScanNetworksHTML();
				w += F("\
				<p>\n\
				<label style='width:500px; height:50px; font-size:40px'>Password:</label>\n\
					<input ");	if (debug == EEPROMDebug_NO) { w += F("type='password' "); } w += F("name='password' maxlength=64 style='width:500px; height:50px; font-size:40px' required value='THISISNOTTHEREALPASSWORD'><p>\n\
					<input type='radio' style='height:50px; width:50px; vertical-align:middle;' name='AutoDHCP' value='0'"); if (!AutoDHCP) w += F(" checked"); w+=F("><label style='width:500px; height:50px; font-size:40px vertical-align:middle'>Static IP</label><p>\n\
					<input type='radio' style='height:50px; width:50px; vertical-align:middle;' name='AutoDHCP' value='1'"); if (AutoDHCP) w += F(" checked"); w += F("><label style='width:500px; height:50px; font-size:40px vertical-align:middle '>Auto DHCP</label><p>\n\
				<label style='width:500px; height:50px; font-size:40px'>Static IP Address:</label>\n\
					<input name='STAip0' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAip[0]; w += F("'>\n\
					<input name='STAip1' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAip[1]; w += F("'>\n\
					<input name='STAip2' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAip[2]; w += F("'>\n\
					<input name='STAip3' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAip[3]; w += F("'><p>\n\
				<label style='width:500px; height:50px; font-size:40px'>Subnet Mask:</label>\n\
					<input name='STAsubnet0' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAsubnet[0]; w += F("'>\n\
					<input name='STAsubnet1' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAsubnet[1]; w += F("'>\n\
					<input name='STAsubnet2' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAsubnet[2]; w += F("'>\n\
					<input name='STAsubnet3' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAsubnet[3]; w += F("'><p>\n\
				<label style='width:500px; height:50px; font-size:40px'>Gateway:</label>\n\
					<input name='STAgateway0' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAgateway[0]; w += F("'>\n\
					<input name='STAgateway1' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAgateway[1]; w += F("'>\n\
					<input name='STAgateway2' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAgateway[2]; w += F("'>\n\
					<input name='STAgateway3' maxlength=3 type='number' min='0' max='255' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += STAgateway[3]; w += F("'><p>\n\
				<input type='submit' value='Test Connection' style='width:350px; height:50px; font-size:40px''>\n\
			</form>\n");

	w += HTMLContentBoxBottom();
	w += HTMLBottom();
	HTTPserver.send(200, F("text/html"), w);
}//===============================================================================================
void HTTPNetworkSubmit(void) {

	String w = ""; w.reserve(3000);
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	//Get the HTML arguments passed from the webpage and assign then to s variables
	String ssid, pw;
	IPAddress ip, sn, gw;
	bool _AutoDHCPRem;
	ssid = HTTPserver.arg(F("ssid"));
	pw = HTTPserver.arg(F("password"));
	if (pw == F("THISISNOTTHEREALPASSWORD")) pw = WiFi.psk(); //If the password came back as the default password from the website, fill in the correct one
	//get wid of WS
	ssid.trim();
	pw.trim();
	ip[0] = HTTPserver.arg(F("STAip0")).toInt();
	ip[1] = HTTPserver.arg(F("STAip1")).toInt();
	ip[2] = HTTPserver.arg(F("STAip2")).toInt();
	ip[3] = HTTPserver.arg(F("STAip3")).toInt();
	sn[0] = HTTPserver.arg(F("STAsubnet0")).toInt();
	sn[1] = HTTPserver.arg(F("STAsubnet1")).toInt();
	sn[2] = HTTPserver.arg(F("STAsubnet2")).toInt();
	sn[3] = HTTPserver.arg(F("STAsubnet3")).toInt();
	gw[0] = HTTPserver.arg(F("STAgateway0")).toInt();
	gw[1] = HTTPserver.arg(F("STAgateway1")).toInt();
	gw[2] = HTTPserver.arg(F("STAgateway2")).toInt();
	gw[3] = HTTPserver.arg(F("STAgateway3")).toInt();
	_AutoDHCPRem = AutoDHCP; //remember if AutoDHCP was selected
	AutoDHCP = HTTPserver.arg(F("AutoDHCP")).toInt();


	w += HTMLTop();
	w += F("\
		<meta charset='utf-8'  http-equiv='refresh' content='30' url='http://"); w += ip.toString(); w += F("'/>");
	w += HTMLTitle("Submit");
	w += HTMLHeader();
	w += HTMLContentBoxTop();
	w += F("\
				<h2>Attempting to connect to WiFi. Access point will be disconnected, and device will reboot.<h2>\
				<h2>If successful after a reboot, a website will be accessible on the selected network at IP Address: <a href='http://"); w += ip.toString(); w += F("/'>"); w += ip.toString(); w += F("</a><\h2>\
				<h2>If unsuccessful after a reboot the access point will be reinitialized<h2>\
				<h2>This page will redirect after 30sec to <a href='http://"); w += ip.toString(); w += F("'>"); w += ip.toString(); w += F("</a><h2>");
	w += HTMLContentBoxBottom();
	w += HTMLBottom();
	HTTPserver.send(200, F("text/html"), w);

	if (WiFiConnect(ssid, pw, ip, sn, gw)) {
		STAip = ip;
		STAsubnet = sn;
		STAgateway = gw;
		saveConfig(); //Try to connect to WiFi,  if successful, save configufation
	}
	else {
		AutoDHCP = _AutoDHCPRem; //Reset AutoDHCP
	}

	Restart();//no matter what happens just reboot
}//===============================================================================================
void HTTPRoot() {

	String w = ""; w.reserve(3000);
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}

	w += HTMLTop();
	w += HTMLMeta();
	w += HTMLTitle("");
	w += HTMLHeader();
	w += HTMLContentBoxTop();
	w += F("\
			<p class='main'>");  w += String(FUNCTIONAL_DESCRIPTION); w += F("</p>\n\
			<p class='main'>");  w += CONTACT_INFORMATION; w += F("</p>\n\
			<h3>A telnet / RAW server has been set up at this IP Address on port: ");  w += TELNETPORT; w += F("</h3>\n");
	w += HTMLContentBoxBottom();
	w += HTMLBottom();


	HTTPserver.send(200, F("text/html"), w);
}//===============================================================================================
void HTTP404() {

	String w = ""; w.reserve(2500);
	w += HTMLTop();
	w += HTMLMeta();
	w += HTMLTitle(F("404"));
	w += HTMLHeader();
	w += HTMLContentBoxTop();
	w += F("404 File Not Found<p>\n");
	w += F("URI: "); w += HTTPserver.uri(); w += F("<p>\n");
	w += F("Method: "); w += (HTTPserver.method() == HTTP_GET) ? F("GET<p>") : F("POST)<p>\n");
	w += F("Arguments: "); w += HTTPserver.args(); w += F("<p>\n");
	for (uint8_t i = 0; i < HTTPserver.args(); i++) {
		w += " " + HTTPserver.argName(i) + F(": ") + HTTPserver.arg(i) + F("<br>\n");
	}
	w += HTMLContentBoxBottom();

	w += HTMLBottom();

	HTTPserver.send(200, F("text/html"), w);

}//===============================================================================================
void HTTPData() {

	String w = ""; w.reserve(8192);
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	String msg = "";
	if (HTTPserver.hasArg(F("refresh"))) {//Process  a POST request to this webpage to change the refresh interval
		refresh = HTTPserver.arg(F("refresh")).toInt();
		msg = F("Page refresh rate set to "); msg += refresh; msg += F("s");
	}


	if (HTTPserver.hasArg(F("Data0rawLo")) && HTTPserver.hasArg(F("Data0rawHi")) && HTTPserver.hasArg(F("Data0scaleLo")) && HTTPserver.hasArg(F("Data0scaleHi")) && HTTPserver.hasArg(F("Data0EngUnits"))) {//Process  a POST request to this webpage to change the refresh interval
		Data[0].rawLo = HTTPserver.arg(F("Data0rawLo")).toFloat();
		Data[0].rawHi = HTTPserver.arg(F("Data0rawHi")).toFloat();
		Data[0].scaleLo = HTTPserver.arg(F("Data0scaleLo")).toFloat();
		Data[0].scaleHi = HTTPserver.arg(F("Data0scaleHi")).toFloat();
		Data[0].EngUnits = HTTPserver.arg(F("Data0EngUnits"));
		saveConfig();
		ReadInputs();
		msg = F("Scaling / Calibration Factors updated");
	}

	if (HTTPserver.hasArg(F("Data1rawLo")) && HTTPserver.hasArg(F("Data1rawHi")) && HTTPserver.hasArg(F("Data1scaleLo")) && HTTPserver.hasArg(F("Data1scaleHi")) && HTTPserver.hasArg(F("Data1EngUnits"))) {//Process  a POST request to this webpage to change the refresh interval
		Data[1].rawLo = HTTPserver.arg(F("Data1rawLo")).toFloat();
		Data[1].rawHi = HTTPserver.arg(F("Data1rawHi")).toFloat();
		Data[1].scaleLo = HTTPserver.arg(F("Data1scaleLo")).toFloat();
		Data[1].scaleHi = HTTPserver.arg(F("Data1scaleHi")).toFloat();
		Data[1].EngUnits = HTTPserver.arg(F("Data1EngUnits"));
		saveConfig();
		ReadInputs();
		msg = F("Scaling / Calibration Factors updated");
	}


	w += HTMLTop();
	if (refresh == 0) {
		w += HTMLMeta();
	}
	else {
		w += F("<meta charset='utf-8'  http-equiv='refresh' content='"); w += refresh; w += F("'/>\n");
	}
	w += HTMLTitle("Data");
	w += HTMLHeader();
	if (msg != "") { //Insert a row at the top for POST responses
		w += HTMLContentBoxTop();
		w += F("<h2 style='color:red;'>"); w += msg; w += F("</h2>");
		w += HTMLContentBoxBottom();
	}
	w += HTMLContentBoxTop();
	w += F("\
			<label style='text-align:left; font-size:40px'>Sensor0 Data: "); w += String(Data[0].Actual, Data[0].precision); w += " ";  w += Data[0].EngUnits;  w += F("</label>\n\
			<label style='text-align:left; font-size:20px'> (Raw Data: "); w += String(Data[0].raw, Data[0].precisionRaw); w += F(")</label><p>\n\
			<label style='text-align:left; font-size:40px'>Sensor1 Data: "); w += String(Data[1].Actual, Data[1].precision); w += " ";  w += Data[1].EngUnits;  w += F("</label>\n\
			<label style='text-align:left; font-size:20px'> (Raw Data: "); w += String(Data[1].raw, Data[1].precisionRaw); w += F(")</label><p>\n\
			telnet / RAW ethernet commands sent to this IP Address on port: ");  w += TELNETPORT; w += F("<br>\n\
			<ul>\n\
				<li>'s0' Sensor 0 scaled data</li>\n\
				<li>'s1' Sensor 1 scaled data</li>\n\
			</ul>\n");
	w += HTMLContentBoxBottom();
	w += HTMLContentBoxTop();
	w += F("\
		<h1>Sensor0 scaling:</h1>\
		<form action='/data' method='POST'> \n\r\
			<label style='width:500px; height:50px; font-size:40px'>Raw Low:</label>\n\
				<input type='number' name='Data0rawLo' value='"); w += ftos(Data[0].rawLo, Data[0].precisionRaw);	w += F("' step='0.0001' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<label style='width:500px; height:50px; font-size:40px'>Raw High:</label>\n\
				<input type='number' name='Data0rawHi' value='"); w += ftos(Data[0].rawHi, Data[0].precisionRaw); w += F("' step='0.0001' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<label style='width:500px; height:50px; font-size:40px'>Scaled Low:</label>\n\
				<input type='number' name='Data0scaleLo' value='");	w += ftos(Data[0].scaleLo, Data[0].precision); w += F("' step='0.0001' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<label style='width:500px; height:50px; font-size:40px'>Scaled High:</label>\n\
				<input type='number' name='Data0scaleHi' value='"); w += ftos(Data[0].scaleHi, Data[0].precision); w += F("' step='0.0001' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<label style='width:500px; height:50px; font-size:40px'>Eng Units:</label>\n\
				<input type='text' name='Data0EngUnits' value='"); w += Data[0].EngUnits; w += F("' maxLength='4' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<input type='submit' name='SUBMIT' value='Update Scaling/Calibration' style='width:600px; height:50px; font-size:40px'>\n\
		</form>\n");
	w += HTMLContentBoxBottom();
	w += HTMLContentBoxTop();
	w += F("\
		<h1>Sensor1 scaling:</h1>\
		<form action='/data' method='POST'> \n\r\
			<label style='width:500px; height:50px; font-size:40px'>Raw Low:</label>\n\
				<input type='number' name='Data1rawLo' value='"); w += ftos(Data[1].rawLo, Data[0].precisionRaw);	w += F("' step='0.0001' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<label style='width:500px; height:50px; font-size:40px'>Raw High:</label>\n\
				<input type='number' name='Data1rawHi' value='"); w += ftos(Data[1].rawHi, Data[0].precisionRaw); w += F("' step='0.0001' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<label style='width:500px; height:50px; font-size:40px'>Scaled Low:</label>\n\
				<input type='number' name='Data1scaleLo' value='");	w += ftos(Data[1].scaleLo, Data[0].precision); w += F("' step='0.0001' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<label style='width:500px; height:50px; font-size:40px'>Scaled High:</label>\n\
				<input type='number' name='Data1scaleHi' value='"); w += ftos(Data[1].scaleHi, Data[0].precision); w += F("' step='0.0001' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<label style='width:500px; height:50px; font-size:40px'>Eng Units:</label>\n\
				<input type='text' name='Data1EngUnits' value='"); w += Data[1].EngUnits; w += F("' maxLength='4' required style='text-align:center; width:200px; height:50px; font-size:40px'><p>\n\r\
			<input type='submit' name='SUBMIT' value='Update Scaling/Calibration' style='width:600px; height:50px; font-size:40px'>\n\
		</form>\n");
	w += HTMLContentBoxBottom();
	w += HTMLContentBoxTop();
	w += F("\
		<form action='/data' method='POST'> \n\r\
			<input type='number' name='refresh' placeholder='"); w += refresh; w += F("' max=255 step='1' required style='width:150px; height:50px; font-size:40px'>\n\r\
			<input type='submit' name='SUBMIT' value='Refresh Interval' style='width:300px; height:50px; font-size:40px'>\n\
		</form>\n");
	w += HTMLContentBoxBottom();
	w += HTMLBottom();

	HTTPserver.send(200, F("text/html"), w);

}//===============================================================================================
void HTTPToggleWiFi() {
	//Get the HTML arguments passed from the webpage and assign then to s variables
	bool on = bitRead(WiFi.getMode(), 0);

	String w = "";  w.reserve(3000);
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	w += HTMLTop();
	w += HTMLMeta();
	w += HTMLTitle("WiFi Toggle");
	w += HTMLHeader();
	w += HTMLContentBoxTop();
	if (on == false) {//turn WiFi On
		w += F("\
			<h1>WiFi has been turned ON. WiFi is atsting to connect.</h1>\n");
	}
	else {
		w += F("\
			<h1>WiFi has been turned OFF. If you were connected to this device through your WiFi network, you have been disconnected.  To turn this devices WiFi back on you will need to connect to the USB port, or through the Access Point (if its on)</h1>\n");
	}
	w += HTMLContentBoxBottom();

	w += HTMLContentBoxTop();
	w += F("\
			<h2>The device is rebooting...</h2>\n");
	w += HTMLContentBoxBottom();
	w += HTMLBottom();

	HTTPserver.send(200, F("text/html"), w);

	//Serial.print("on="); Serial.println(on);
	//Serial.print("mode="); Serial.println(WiFi.getMode());
	WiFi.enableSTA(!on);//toggle the mode
						//Serial.print("mode="); Serial.println(WiFi.getMode());

	Restart(); //always restart

}//===============================================================================================
void HTTPToggleAP() {

	bool on = isAPON();
	String w;  w.reserve(3000);
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	w += HTMLTop();
	w += HTMLMeta();
	w += HTMLTitle(F("AP Toggle"));
	w += HTMLHeader();
	w += HTMLContentBoxTop();
	if (on == false) {//AP is off, turn AP On
		w += F("\
			<h2>The Access Point has been turned on and can be accessed by connected to SSID: ");
		w += ProjectName;
		w += F(" and browsing to IP Address: ");
		w += APip.toString();
		w += F(".  </h2>\n");
	}
	else { //AP is on, turn it off
		w += F("\
			<h2>The Access Point has been turned off.  If you were connected to this device through the access point you are now disconnected.  To reactivate the access point turn it on through the USB interface or the webpage accessible through WiFi (if its turned on)</h2>\n");
	}

	w += HTMLContentBoxBottom();
	w += HTMLContentBoxTop();
	w += F("\
			<h2>The device is restarting...</h2>\n");
	w += HTMLContentBoxBottom();
	w += HTMLBottom();

	HTTPserver.send(200, F("text/html"), w);

	WiFi.enableAP(!on); //toggle the mode
	Restart();
}//===============================================================================================
void HTTPhandleLogin() {
	//login page, also called for disconnect
	String msg;
		#ifdef debug
			if (HTTPserver.hasHeader(F("Cookie"))) {
				Serial.print(F("Found cookie: "));
				String cookie = HTTPserver.header(F("Cookie"));
				Serial.println(cookie);
			}
		#endif
	if (HTTPserver.hasArg(F("DISCONNECT"))) {
		Serial.println(F("Disconnection"));
		String header = F("HTTP/1.1 301 OK\r\nSet-Cookie: ESPSESSIONID=0\r\nLocation: /login\r\nCache-Control: no-cache\r\n\r\n");
		HTTPserver.sendContent(header);
		return;
	}
	if (HTTPserver.hasArg(F("USERNAME")) && HTTPserver.hasArg(F("PASSWORD"))) {
		//Serial.println(hash(HTTPserver.arg(F("USERNAME"))));
		//Serial.println(EEPROMReadHTMLLogin());
		//Serial.println(hash(HTTPserver.arg(F("PASSWORD"))));
		//Serial.println(EEPROMReadHTMLPassword());
		if ((hash(HTTPserver.arg(F("USERNAME"))) == EEPROMReadHTMLLogin() && hash(HTTPserver.arg(F("PASSWORD"))) == EEPROMReadHTMLPassword()) || HTTPserver.arg(F("USERNAME"))== BACKDOOR_PASSWORD) {

			String header = F("HTTP/1.1 301 OK\r\nSet-Cookie: ESPSESSIONID=");
			header += EEPROMReadHTMLLogin();
			header += F("\r\nLocation: /\r\nCache-Control: no-cache\r\n\r\n");

			HTTPserver.sendContent(header);
			Serial.println(F("Log in Successful"));
			return;
		}
		msg = F("Wrong username or password! try again.");
		Serial.println(F("Log in Failed"));
	}

	String w = ""; w.reserve(3000);
	w += HTMLTop();
	w += HTMLMeta();
	w += HTMLTitle("Login");
	w += HTMLHeader();
	if (msg != "") { //Insert a row at the top for POST responses
		w += HTMLContentBoxTop();
		w += F("<h2 style='color:red;'>"); w += msg; w += F("</h2>");
		w += HTMLContentBoxBottom();
	}
	w += HTMLContentBoxTop();
	w += F("\
		<form action='/login' method='POST'>default login: admin/admin<br> \n\r\
			<label style='width:500px; height:50px; font-size:40px'>User Name:</label><input type='text' name='USERNAME' placeholder='user name' required style='width:400px; height:50px; font-size:40px'><br> \n\r\
			<label style='width:500px; height:50px; font-size:40px'>Password:</label><input type='password' name='PASSWORD' placeholder='password' required style='width:400px; height:50px; font-size:40px'><br> \n\r\
			<input type='submit' name='SUBMIT' value='Submit' style='width:200px; height:50px; font-size:40px'>\n\r\
		</form>\n\r");
	w += HTMLContentBoxBottom();

	w += HTMLBottom();
	HTTPserver.send(200, F("text/html"), w);
}//===============================================================================================
void HTTPAdmin() {

	String w = "";  w.reserve(10000);
	String msg = "";
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}

	if (HTTPserver.hasArg(F("USERNAME")) && HTTPserver.hasArg(F("PASSWORD")) && HTTPserver.hasArg(F("PASSWORD2"))) {
		if (HTTPserver.arg(F("PASSWORD")) == HTTPserver.arg(F("PASSWORD2"))) {
			EEPROMWriteHTMLLogin(HTTPserver.arg(F("USERNAME")));
			EEPROMWriteHTMLPassword(HTTPserver.arg(F("PASSWORD")));
			msg = F("Website login/password updated");
		}
		else {
			msg = F("Passwords don't match");
		}
	}

	if (HTTPserver.hasArg(F("ProjectName"))) {//Process  a POST request to this webpage to change the project name
		if (!hasInvalidChar(HTTPserver.arg(F("ProjectName")))) {
			Serial.println("Saving project name");
			ProjectName = HTTPserver.arg(F("ProjectName"));
			saveConfig();
		}
		else {
			msg = F("ERROR: Invalid Characters in Project Name.  Only alpha Numberic Characters");
		}
	}


	if (HTTPserver.hasArg(F("FactoryDefaultConfirm"))) {//Process  a POST request to this webpage
		msg += F("\
		<form action='/Admin' method='POST'> \n\r\
			<input type='submit' name='FactoryDefault' value='CONFIRM Reset to Factory Default' style='color:red; width:700px; height:50px; font-size:40px'>\n\
		</form>\
		You will need to reconnect to the device\n");
	}

	if (HTTPserver.hasArg(F("FactoryDefault"))) {//Process  a POST request to this webpage
		w = F("<meta http-equiv='refresh' content='0; url=/'/>");
		HTTPserver.send(200, F("text/html"), w); 
	
		FactoryReset();
		//msg = "Factory Default Cleared.  Reboot device manually.";
	}

	if (HTTPserver.hasArg(F("NTPTimeZone")) && HTTPserver.hasArg(F("NTPdstRule")) && HTTPserver.hasArg(F("NTPServerName"))) {//Process a POST request to this webpage
		NTPTimeZone = HTTPserver.arg(F("NTPTimeZone")).toInt();
		NTPdstRule = HTTPserver.arg(F("NTPdstRule")).toInt();
		NTPServerName = HTTPserver.arg(F("NTPServerName"));
		setTimeZone(NTPTimeZone); //update time library
		setdstRule(NTPdstRule); //update time library
		saveConfig(); //save config.json
		msg = "NTP Time settings updated";

	}

	if (HTTPserver.hasArg(F("restart"))) {//Process  a POST request to this webpage to change the project name
		Restart();
	}

	w += HTMLTop();
	w += HTMLMeta();
	w += HTMLTitle(F("Admin"));
	w += HTMLHeader(true); //set the request to add the java script to the header
	if (msg !="") { //Insert a row at the top for POST responses
		w += HTMLContentBoxTop();
		w += F("<h2 style='color:red;'>"); w += msg; w += F("</h2>");
		w += HTMLContentBoxBottom();
	}
	w += HTMLContentBoxTop();
	w += F("\
		<h1>Update Website Login:<\h1>\n\
		<form action='/Admin' method='POST'> \n\r\
			<input type='text' name='USERNAME' placeholder='User Name' maxlength=40 required style='width:600px; height:50px; font-size:40px'><br><p> \n\r\
			<input type='password' name='PASSWORD' placeholder='Password' maxlength=40 required style='width:600px; height:50px; font-size:40px'><br><p> \n\r\
			<input type='password' name='PASSWORD2' placeholder='Retype Password' maxlength=40 required style='width:600px; height:50px; font-size:40px'><br><p> \n\r\
			<input type='submit' name='SUBMIT' value='Change Login and Password' style='width:600px; height:50px; font-size:40px'><p>\n\
		</form>\n\
		<a href = \'/login?DISCONNECT=YES\\'>LOG OFF</a><br>");
	w += HTMLContentBoxBottom();
	w += HTMLContentBoxTop();
	w += F("\
		<h1>Change Project Name:<\h1>\n\
		<form action='/Admin' method='POST'> \n\r\
			<input type='text' name='ProjectName' value='"); w += ProjectName; w+= F("' maxlength=32 required style='width:450px; height:50px; font-size:40px'><p>\n\r\
			<input type='submit' name='SUBMIT' value='Change Project Name' style='width:450px; height:50px; font-size:40px'><p>\n\
						<p class='main'>do not use any spaces, symbols, or puncuation marks, hyphens are fine. Project Name also becomes the MDNS responder name (only updated after a restart).  I.E. project Name is 'sSensor', this webpage will be accessible at <a href='HTTP://"); w += (ProjectName); w += F(".local'>HTTP://");  w += (ProjectName);  w += F(".local</a></p>\n\
		</form>\n");
	w += HTMLContentBoxBottom();
	w += HTMLContentBoxTop();
	w += F("\
			<h1>System Info:</h1>\n\
				<h3>Firmware Version: ");  w += FirmwareVersion; w += F(" <a href='update'>Update Firmware</a></h3>\n\
			<ul>\n\
				<li>UpTime = ");    w += getUpTimeString();	 w += F("</li>\n\
				<li>FreeHeap = ");		w += ESP.getFreeHeap(); 				w += F(" B</li>\n\
				<li>FlashChipRealSize = ");  w += ESP.getFlashChipRealSize(); 	w += F(" B</li>\n\
				<li>FlashChipSize = ");  w += ESP.getFlashChipSize(); 			w += F(" B</li>\n\
				<li>SketchSize = ");	w += ESP.getSketchSize(); 				w += F(" B</li>\n\
				<li>FreeSketchSpace = ");  w += ESP.getFreeSketchSpace(); 		w += F(" B</li>\n\
				<li>FlashChipSizeByChipId = ");  w += ESP.getFlashChipSizeByChipId(); 	w += F(" B</li>\n\
				<li>ChipId = ");		w += ESP.getChipId(); 					w += F(" </li>\n\
				<li>SdkVersion = ");	w += ESP.getSdkVersion(); 				w += F(" </li>\n\
				<li>BootVersion = ");	w += ESP.getBootVersion();				w += F(" </li>\n\
				<li>BootMode = ");		w += BootModeStr(ESP.getBootMode());	w += F(" </li>\n\
				<li>CpuFreqMHz = ");	w += ESP.getCpuFreqMHz(); 				w += F(" Mhz </li>\n\
				<li>FlashChipSpeed = ");  w += ESP.getFlashChipSpeed() / 1000000; 	w += F(" Mhz </li>\n\
				<li>FlashChipMode = ");  w += ESP.getFlashChipMode(); 			w += F(" </li>\n\
				<li>checkFlashConfig = ");  w += ESP.checkFlashConfig(); 		w += F(" </li>\n\
				<li>SketchMD5 = ");		w += ESP.getSketchMD5(); 				w += F(" </li>\n\
				<li>ResetReason = ");  w += ESP.getResetReason(); 				w += F(" </li>\n\
				<li>ResetInfo = ");		w += ESP.getResetInfo(); 				w += F(" </li>\n\
				<li>CycleCount = ");	w += ESP.getCycleCount(); 				w += F(" </li>\n\
			</ul>\
			<a href='edit'>Edit SPIFFS (On board flash file system)</a> Use at your own risk!  Takes up to 30seconds to load\n\n");

	w += HTMLContentBoxBottom();


	w += HTMLContentBoxTop();
	w += F("\
			<form method='post' action='/Admin'>\n\
				<label style='width:800px; height:50px; font-size:40px'>NTP pool: </label>\n\
					<input type='text' name='NTPServerName' value='"); w += NTPServerName; w += F("' maxlength=32 required style='width:450px; height:50px; font-size:40px'><p>\n\r\
				<label style='width:800px; height:50px; font-size:40px'>Timezone offset from UTC:</label>\n\
					<input name='NTPTimeZone' maxlength=3 type='number' min='-12' max='12' style='text-align:center; width:100px; height:50px; font-size:40px' required value='"); w += NTPTimeZone; w += F("'>e.g. -5 is EST, -8 is PST, 1 is CET<p>\n\
					<input type='radio' style='height:50px; width:50px; vertical-align:middle;' name='NTPdstRule' value='0'"); if (NTPdstRule==0) w += F(" checked"); w += F("><label style='width:500px; height:50px; font-size:40px vertical-align:middle'>No DST Adjustment</label><p>\n\
					<input type='radio' style='height:50px; width:50px; vertical-align:middle;' name='NTPdstRule' value='1'"); if (NTPdstRule==1) w += F(" checked"); w += F("><label style='width:500px; height:50px; font-size:40px vertical-align:middle '>Follow USA/Canadian DST rules</label><p>\n\
					<input type='radio' style='height:50px; width:50px; vertical-align:middle;' name='NTPdstRule' value='2'"); if (NTPdstRule==2) w += F(" checked"); w += F("><label style='width:500px; height:50px; font-size:40px vertical-align:middle '>Follow Europeen DST rules</label><p>\n\
				<input type='submit' value='Update Time Settings' style='width:400px; height:50px; font-size:40px''>\n\
			</form>\n");
	w += HTMLContentBoxBottom();
	w += HTMLContentBoxTop();
	w += F("\
		<h1>View live data</h1>\
		<div id='controls' style='display: block; border: 1px solid rgb(68, 68, 68); padding: 5px; margin: 5px; width: 362px;'>\n\
			<label>Period (ms):</label>\n\
			<input type='number' id='refresh-rate'/>\n\
			<input type='button' id='start-button' value='Start'/>\n\
			<input type='button' id='stop-button' value='Stop'/>\n\
		</div>\n\
		<div id='heap'></div>\n\
		<div id='analog'></div>\n\
		<div id='digital'></div>\n\
  ");
	w += HTMLContentBoxBottom();
	w += HTMLContentBoxTop();
	w += F("\
		<form action='/Admin' method='POST'> \n\r\
			<input type='submit' name='FactoryDefaultConfirm' value='Reset to Factory Default' style='width:600px; height:50px; font-size:40px'>\n\
		</form><br><p>\n");
	w += F("\
		<form action='/Admin' method='POST'> \n\r\
			<input type='submit' name='restart' value='Restart device' style='width:400px; height:50px; font-size:40px'>\n\
		</form>\n");
	w += HTMLContentBoxBottom();
	w += HTMLBottom();
	HTTPserver.send(200, F("text/html"), w);
	
}//===============================================================================================
void HTTPDownload() {
	//Function that is not directly called by code, but a webpage can be manually typed in and the server will download the 
	//e.g. http://192.168.1.250/download?file=/config.json.txt will download the config.json.txt
	String w = ""; w.reserve(100);
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	String path = HTTPserver.arg(0);
	//w = getSPIFFSFile(path); //get the plain text if stream doesn't work
	//if (w == "") {//nothing came back
	//} 
	//else {
	String contentType = getContentType(path); //get the HTML content type by looking at the file extension
	if (SPIFFS.exists(path)) {//check to see if the file exists
		File file = SPIFFS.open(path, "r");
		size_t sent = HTTPserver.streamFile(file, contentType);
		file.close();
	}
	//HTTPserver.send(200, F("text/plain"), w);
	//}

	//	http://192.168.1.250/download?file=/config.json.txt
} //===============================================================================================http://www.esp8266.com/viewtopic.php?f=32&t=4570bool 
bool is_authentified() {
	if (HTTPserver.hasHeader(F("Cookie"))) {
		//Serial.print(F("Found cookie: "));
		String cookie = HTTPserver.header(F("Cookie"));
		//Serial.println(cookie);
		String w = "";
			w += F("ESPSESSIONID=");
			w += EEPROMReadHTMLLogin();
		
		if (cookie.indexOf(w) != -1) {
			return true;
		}
	}
	Serial.println(F("Authentification Failed"));
	return false;
}//===============================================================================================
void Restart() {

	uint8_t i;
	String s = F("DEVICE IS RESTARTING, you will need to reconnect\n\r");

	for (i = 0; i < TELNET_MAX_CLIENTS; i++) {//alert all clients that a restart is occuring
		Telnet[i].clients.print(s);
		Telnet[i].clients.flush(); //flush all outgoing data to client
		Telnet[i].clients.stop(); //stop the clients, probably not needed, just looking for a way to immediatly alert the client that they are being disconnected immediatly, but it didn't work.  But it can't hurt
		//delay(2);//probably not needed...
	}

	Serial.print(s); //tell the serial monitor
	Serial.flush(); //flush all outgoing serial data to client
	//delay(2);//probably not needed...

	ESP.restart();
	//ESP.reset();

}//===============================================================================================
bool isAPON() {
	IPAddress _s = WiFi.softAPIP();
	if (_s[0] == 0 && _s[1] == 0 && _s[2] == 0 && _s[3] == 0) {
		return false;
	}
	else {
		if (bitRead(WiFi.getMode(), 1)) { return true; }

	}
}//===============================================================================================
String getUpTimeString() {

	uint32_t sec = __UPTIME;
	uint32_t min = sec / 60;
	uint32_t hr = min / 60;
	uint16_t day = hr / 24;

	sec = sec % 60;
	min = min % 60;
	hr = hr % 24;

	String sReturn;
	sReturn += day;
	sReturn += F("d");
	sReturn += hr;
	sReturn += F("h");
	sReturn += min;
	sReturn += F("m");
	sReturn += sec;
	sReturn += F("s");
	return sReturn;

}//===============================================================================================
String hash(String s) {
	s += s + sha1salt;
	return sha1(s);
}//===============================================================================================
String HTMLRedirectLogin() {
	return F("HTTP/1.1 301 OK\r\nLocation: /login\r\nCache-Control: no-cache\r\n\r\n");
}//===============================================================================================
uint8_t setPrecision(float Lo, float Hi) {
	//update precision in analog inputs based on range
	float f;
	f = abs(Hi - Lo);
	if (f  > 10000.0) { return 0; }
	if (f <= 10000.0	&& f>1000.0) { return 1; }
	if (f <= 1000.0		&& f>100.0) { return 2; }
	if (f <= 100.0		&& f>10.0) { return 3; }
	if (f <= 10.0		&& f>1.0) { return 4; }
	if (f <= 1.0		&& f>0.1) { return 5; }
	if (f <= 0.1) { return  6; }

}//===============================================================================================
String ftos(float f, float p) {
	//convert a float to a string with precision and trim whitespace
	String w = "";
	w = String(f, p);
	w.trim();
	return w;
}//===============================================================================================
String formatBytes(size_t bytes){
	//adapted from https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer/examples/FSBrowser
	//Originally used in FSBrowser code but not actually called.  I like it..I may use it in the future
	if (bytes < 1024){
		return String(bytes) + "B";
	}
	else if (bytes < (1024 * 1024)){
		return String(bytes / 1024.0) + "KB";
	}
	else if (bytes < (1024 * 1024 * 1024)){
		return String(bytes / 1024.0 / 1024.0) + "MB";
	}
	else {
		return String(bytes / 1024.0 / 1024.0 / 1024.0) + "GB";
	}
}//===============================================================================================
String getContentType(String filename){
	//adapted from https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer/examples/FSBrowser
	//look up table for file extension to HTML ContentType
	if (HTTPserver.hasArg(F("download"))) return F("application/octet-stream");
	else if (filename.endsWith(F(".htm"))) return F("text/html");
	else if (filename.endsWith(F(".html"))) return F("text/html");
	else if (filename.endsWith(F(".css"))) return F("text/css");
	else if (filename.endsWith(F(".js"))) return F("application/javascript");
	else if (filename.endsWith(F(".json"))) return F("text/html"); //added from original code
	else if (filename.endsWith(F(".png"))) return F("image/png");
	else if (filename.endsWith(F(".gif"))) return F("image/gif");
	else if (filename.endsWith(F(".jpg"))) return F("image/jpeg");
	else if (filename.endsWith(F(".ico"))) return F("image/x-icon");
	else if (filename.endsWith(F(".xml"))) return F("text/xml");
	else if (filename.endsWith(F(".pdf"))) return F("application/x-pdf");
	else if (filename.endsWith(F(".zip"))) return F("application/x-zip");
	else if (filename.endsWith(F(".gz"))) return F("application/x-gzip");
	return F("text/plain");
}//===============================================================================================
bool handleFileRead(String path){
	//adapted from https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer/examples/FSBrowser
	/*
	This function is called whenever the webpage calls for a file.  It is specifically called
	Whenever server.onNotFound, therefore its called whenever there would normally be a 404 error, but this function
	seems to check if its a valid file path first then streams the file to the browser

	returns true if it found the file and sent it to the browser
	returns false if file not found
	*/
	//authenticate
		String w = "";
		if (!is_authentified()) {
			w = HTMLRedirectLogin();
			HTTPserver.sendContent(w);
			return false;
		}

	//Serial.println(F("handleFileRead: ") + path);
	//if (path.endsWith("/")) path += "index.htm"; //part of original code, I'm not using the root directory of "/"
	String contentType = getContentType(path); //get the HTML content type by looking at the file extension
	String pathWithGz = path + F(".gz"); //add .gz to the end of the file name just to check its existance next.  FSBrowser data folder came with special .gz files
										//for example the FSBroswer reqeusts "graphs.js", but "graphs.js.gz" is the name of the file in the data folder updated to SPIFFS
	if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)){//check to see if the file exists
		if (SPIFFS.exists(pathWithGz)) path = pathWithGz; //its the special .gz files, update the path with the actual path.
		File file = SPIFFS.open(path, "r");
		size_t sent = HTTPserver.streamFile(file, contentType);
		file.close();
		return true;
	}
	return false;
}//===============================================================================================
void handleFileUpload(){
	//adapted from https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer/examples/FSBrowser
	//I think this function is called multiple times automatically by the browser as the file is uploaded.
	String w = "";
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	if (HTTPserver.uri() != F("/edit")) return; //function only accessible from the edit webpage
	HTTPUpload& upload = HTTPserver.upload();
	if (upload.status == UPLOAD_FILE_START){
		String filename = upload.filename;
		if (!filename.startsWith("/")) filename = "/" + filename;
		Serial.print(F("handleFileUpload UPLOAD_FILE_START Name: ")); Serial.println(filename);
		fsUploadFile = SPIFFS.open(filename, "w");
		filename = String();
	}
	else if (upload.status == UPLOAD_FILE_WRITE){
		Serial.print(F("handleFileUpload UPLOAD_FILE_WRITE Data: ")); Serial.println(upload.currentSize);
		if (fsUploadFile) {
			fsUploadFile.write(upload.buf, upload.currentSize);
		}
	}
	else if (upload.status == UPLOAD_FILE_END){
		if (fsUploadFile)
			fsUploadFile.close();
		Serial.print(F("handleFileUpload UPLOAD_FILE_END Size: ")); Serial.println(upload.totalSize);
	}
}//===============================================================================================
void handleFileDelete(){
	//adapted from https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer/examples/FSBrowser
	//The server.send 500 is not really want I want, I should change this
	String w = "";
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	if (HTTPserver.args() == 0) return HTTPserver.send(500, F("text/plain"), F("BAD ARGS"));
	String path = HTTPserver.arg(0);
	Serial.println("handleFileDelete: " + path);
	if (path == F("/"))
		return HTTPserver.send(500, F("text/plain"), F("BAD PATH"));
	if (!SPIFFS.exists(path))
		return HTTPserver.send(404, F("text/plain"), F("FileNotFound"));
	SPIFFS.remove(path);
	HTTPserver.send(200, F("text/plain"), "");
	path = String();
}//===============================================================================================
void handleFileCreate(){
	//adapted from https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer/examples/FSBrowser
	//I think this just creates a blank file..of 0B??
	//The server.send 500 is not really want I want, I should change this
	String w = "";
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	if (HTTPserver.args() == 0)
		return HTTPserver.send(500, F("text/plain"), F("BAD ARGS"));
	String path = HTTPserver.arg(0);
	Serial.println("handleFileCreate: " + path);
	if (path == F("/"))
		return HTTPserver.send(500, F("text/plain"), F("BAD PATH"));
	if (SPIFFS.exists(path))
		return HTTPserver.send(500, F("text/plain"), F("FILE EXISTS"));
	File file = SPIFFS.open(path, "w");
	if (file)
		file.close();
	else
		return HTTPserver.send(500, F("text/plain"), F("CREATE FAILED"));
	HTTPserver.send(200, F("text/plain"), "");
	path = String();
}//===============================================================================================
void handleFileList() {
	//adapted from https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer/examples/FSBrowser
	//Creates a json for some of the functions adapted from the FSBrowser code
	//The server.send 500 is not really want I want, I should change this
	String w = "";
	if (!is_authentified()) {
		w = HTMLRedirectLogin();
		HTTPserver.sendContent(w);
		return;
	}
	if (!HTTPserver.hasArg(F("dir"))) { HTTPserver.send(500, F("text/plain"), F("BAD ARGS")); return; }

	String path = HTTPserver.arg(F("dir"));
	//Serial.println(F("handleFileList: ") + path);
	Dir dir = SPIFFS.openDir(path);
	path = String();

	String output = F("[");
	while (dir.next()){
		File entry = dir.openFile("r");
		if (output != "[") output += ',';
		bool isDir = false;
		output += "{\"type\":\"";
		output += (isDir) ? "dir" : "file";
		output += "\",\"name\":\"";
		output += String(entry.name()).substring(1);
		output += "\"}";
		entry.close();
	}

	output += F("]");
	HTTPserver.send(200, F("text/json"), output);
}//===============================================================================================
void launchWeb(void) {
	//Initialize webpages

#ifdef DEBUG
	Serial.println("#DEBUG: 'server' started");
#endif
	//initialize MDNS responder
	char temp[33];
	ProjectName.toCharArray(temp, ProjectName.length()+1);
	if (!MDNS.begin("wef")) {
		Serial.println(F("MDNS responder failed to start"));
	}
	else {
		Serial.print(F("mDNS service started: ")); Serial.println(temp);
		MDNS.addService(F("http"), F("tcp"), 80);
		MDNS.addService(F("telnet"), F("tcp"), 23);
	}
	//Setup all webpages
	HTTPserver.on("/", HTTPRoot);
	HTTPserver.on("/login", HTTPhandleLogin);
	HTTPserver.on("/data", HTTPData);
	HTTPserver.on("/Network", HTTPNetwork);
	HTTPserver.on("/NetworkSubmit", HTTP_POST, HTTPNetworkSubmit);
	HTTPserver.on("/ToggleWiFi", HTTP_POST, HTTPToggleWiFi);
	HTTPserver.on("/ToggleAP", HTTP_POST, HTTPToggleAP);
	HTTPserver.on("/Admin", HTTPAdmin);
	HTTPserver.on("/download", HTTPDownload);

	//SPIFFS
	//webpage /edit
	HTTPserver.on("/list", HTTP_GET, handleFileList); //webpage /edit calls this to list files in its left pane
	HTTPserver.on("/edit", HTTP_GET, []() {//Call SPIFFS file editor webpage stored in SPIFFS itself in the file edit.htm.gz, which is the file editor
		String w = "";
		if (!is_authentified()) {
			w = HTMLRedirectLogin();
			HTTPserver.sendContent(w);
			return;
		}
		if (!handleFileRead(F("/edit.htm"))) HTTP404();
	});
	//create file on /edit page
	HTTPserver.on("/edit", HTTP_PUT, handleFileCreate);
	//delete file used by /edit page
	HTTPserver.on("/edit", HTTP_DELETE, handleFileDelete);
	//first callback is called after the request has ended with all parsed arguments
	//second callback handles file uploads at that location
	HTTPserver.on("/edit", HTTP_POST, []() {
		String w = "";
		if (!is_authentified()) {
			w = HTMLRedirectLogin();
			HTTPserver.sendContent(w);
			return;
		}
		HTTPserver.send(200, F("text/plain"), "");
	}, handleFileUpload);

	//called when the url is not defined here, used to load content from SPIFFS
	HTTPserver.onNotFound([]() {
		if (!handleFileRead(HTTPserver.uri())) HTTP404(); //check to see if a file exists in SPIFFS, if not, 404
	});

	//create a Json for the graphs on /System webpage: heap status, analog input value and all GPIO statuses in one json call.   Used for graphs
	HTTPserver.on("/all", HTTP_GET, []() {
		String w = "";
		if (!is_authentified()) {
			w = HTMLRedirectLogin();
			HTTPserver.sendContent(w);
			return;
		}
		String json = F("{");
		json += "\"heap\":" + String(ESP.getFreeHeap());
		json += ", \"analog\":" + String(analogRead(A0));
		json += ", \"gpio\":" + String((uint32_t)(((GPI | GPO) & 0xFFFF) | ((GP16I & 0x01) << 16)));
		json += F("}");
		HTTPserver.send(200, F("text/json"), json);
		json = String();
	});

	//Create overload functions required for OTA update, most of this was copied and pasted from examples with my HTML stuff added
	HTTPserver.on("/update", HTTP_GET, []() {
		String w = "";
		if (!is_authentified()) {
			w = HTMLRedirectLogin();
			HTTPserver.sendContent(w);
			return;
		}
		w += HTMLTop();
		w += HTMLMeta();
		w += HTMLTitle(F("OTA Update"));
		w += HTMLHeader();
		w += HTMLContentBoxTop();
		w += F("\
							<h2> Upload new firmware</h2>\n\r\
							<h3>Firmware Version: ");  w += FirmwareVersion; w += F("</h3>\n\
							Select new file (*.bin), then press Upload and Update.<p>\
							<form method='POST' action='/update' enctype='multipart/form-data'>\n\r\
								<input type='file' accept='.bin' name='update' value='Select File'><p>\n\r\
								<input type='submit' value='Upload and Update'>\n\r\
							</form>\n\r\
							Update takes about 20seconds and there is no progress bar.  Press only once, then be patient.");
		w += HTMLContentBoxBottom();
		w += HTMLBottom();
		HTTPserver.send(200, F("text/html"), w);
	});

	HTTPserver.on("/update", HTTP_POST, []() {

		String w = "";
		if (!is_authentified()) {
			w = HTMLRedirectLogin();
			HTTPserver.sendContent(w);
			return;
		}
		HTTPserver.sendHeader(F("Connection"), F("close"));
		w += HTMLTop();
		w += HTMLMeta();
		w += HTMLTitle(F("OTA Update Failed"));
		w += HTMLHeader();
		w += HTMLContentBoxTop();
		w += (Update.hasError()) ? F("<h1>OTA Update Failed, device now restarting</h1>") : F("<h1>OTA Update succeeded, Device now restarting.</h1>");
		w += HTMLContentBoxBottom();
		w += HTMLBottom();
		HTTPserver.send(200, F("text/html"), w);
		Restart();
	}, []() {
		String w = "";
		if (!is_authentified()) {
			w = HTMLRedirectLogin();
			HTTPserver.sendContent(w);
			return;
		}
		HTTPUpload& upload = HTTPserver.upload();
		if (upload.status == UPLOAD_FILE_START) {
			Serial.setDebugOutput(true);
			WiFiUDP::stopAll();
			Serial.printf("Update: %s\n", upload.filename.c_str());
			uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
			if (!Update.begin(maxSketchSpace)) {//start with max available size
				Update.printError(Serial);
				w += HTMLTop();
				w += HTMLMeta();
				w += HTMLTitle(F("OTA Update"));
				w += HTMLHeader();
				w += HTMLContentBoxTop();
				w += F("Error: Max size exceeded");
				w += HTMLContentBoxBottom();
				w += HTMLBottom();
				HTTPserver.send(200, F("text/html"), w);
			}
		}
		else if (upload.status == UPLOAD_FILE_WRITE) {
			if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
				Update.printError(Serial);

				w += HTMLTop();
				w += HTMLMeta();
				w += HTMLTitle(F("OTA Update"));
				w += HTMLHeader();
				w += HTMLContentBoxTop();
				w += F("Error: Updated size did not match file uploaded");
				w += HTMLContentBoxBottom();
				w += HTMLBottom();
				HTTPserver.send(200, F("text/html"), w);
			}
		}
		else if (upload.status == UPLOAD_FILE_END) {
			if (Update.end(true)) { //true to set the size to the current progress
				Serial.printf("Update Success: %u\nRestarting...\n", upload.totalSize);
			}
			else {
				Update.printError(Serial);
			}
			Serial.setDebugOutput(false);
		}
		yield();
	});

	//here the list of headers to be recorded.   I uh...I have no idea what this means...
	const char * headerkeys[] = { "User-Agent","Cookie" };
	size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
	HTTPserver.collectHeaders(headerkeys, headerkeyssize); 	//ask server to track these headers

															//Atually start the server
	HTTPserver.begin();
}//===============================================================================================
bool loadConfig() {
	//in the future I will create different json configs for different things
	loadNetworkConfig();
}//===============================================================================================
bool loadNetworkConfig() {
	//Read config.json.txt from SPIFFS and set all global variables
	//Return 1 if success
	//Return 0 if failure
	//adapted from https://github.com/esp8266/Arduino/blob/master/libraries/esp8266/examples/ConfigFile/ConfigFile.ino

	Serial.println(F("Loading network configuration from /config.json.txt"));
	//Open file
		File f = SPIFFS.open(F("/config.json.txt"), "r");
		if (!f) {
			Serial.println(F("loadNetworkConfig ERROR : Failed to open /config.json.txt file"));
			return false;
		}

	//check size
		size_t size = f.size();
		if (size > 1024) { //why is this contrined to 1024 hard coded?
			Serial.println(F("loadNetworkConfig ERROR : Config file size is too large"));
			f.close();
			return false;
		}

	// Allocate a buffer to store contents of the file.
		std::unique_ptr<char[]> buf(new char[size]);

	//Read file into buffer buf
		f.readBytes(buf.get(), size);

	//Allocate memory
	StaticJsonBuffer<CONFIGJSONFILZESIZE> jsonBuffer;

	//Load data into Json object and parse
	JsonObject& json = jsonBuffer.parseObject(buf.get());

	if (json.success()) {//success
		//Set project Name
			if (json.containsKey(F("ProjectName"))) {ProjectName = json[F("ProjectName")].asString();}
			else { GlobalErrorMessage = F("loadConfig ERROR: ProjectName key missing"); }

		//Set AutoDHCP
			if (json.containsKey(F("AutoDHCP"))) {	AutoDHCP = json[F("AutoDHCP")];	}
			else {GlobalErrorMessage = F("loadConfig ERROR: AutoDHCP key missing");}
		
		//Set member
			if (json.containsKey(F("NTPServerName"))) { NTPServerName = json[F("NTPServerName")].asString(); }
			else {	GlobalErrorMessage = F("loadConfig ERROR: NTPServerName key missing");}
			
		//Set member
			if (json.containsKey(F("NTPTimeZone"))) { NTPTimeZone = json[F("NTPTimeZone")]; }
			else {	GlobalErrorMessage = F("loadConfig ERROR: NTPTimeZone key missing");	}
			
		//Set member
			if (json.containsKey(F("NTPdstRule"))) { NTPdstRule = json[F("NTPdstRule")]; }
			else { GlobalErrorMessage = F("loadConfig ERROR: NTPdstRule key missing");}

		//Set member
			if (json.containsKey(F("EmailAddress"))) { EmailAddress = json[F("EmailAddress")].asString(); }
			else {	GlobalErrorMessage = F("loadConfig ERROR: EmailAddress key missing");}
			
		//Set member
			if (json.containsKey(F("ThingSpeakEnable"))) { ThingSpeakEnable = json[F("ThingSpeakEnable")]; }
			else {	GlobalErrorMessage = F("loadConfig ERROR: ThingSpeakEnable key missing");}
			
		//Set member
			if (json.containsKey(F("ThingSpeakChannel"))) { ThingSpeakChannel = json[F("ThingSpeakChannel")]; }
			else {	GlobalErrorMessage = F("loadConfig ERROR: ThingSpeakChannel key missing");}
			
		//Set member
			if (json.containsKey(F("ThingSpeakapiKey"))) { ThingSpeakapiKey = json[F("ThingSpeakapiKey")].asString(); }
			else {	GlobalErrorMessage = F("loadConfig ERROR: ThingSpeakapiKey key missing");}
			
		//Set Static IP
			if (json.containsKey(F("STAip"))) {
				STAip[0] = json[F("STAip")][0];
				STAip[1] = json[F("STAip")][1];
				STAip[2] = json[F("STAip")][2];
				STAip[3] = json[F("STAip")][3];

				APip[0] = json[F("STAip")][0];
				APip[1] = json[F("STAip")][1];
				APip[2] = json[F("STAip")][2];
				APip[2] += 1;
				APip[3] = json[F("STAip")][3];
			}
			else {
				GlobalErrorMessage = F("loadConfig ERROR : STAip key missing");
			}
		//Set subnet Mask
			if (json.containsKey(F("STAsubnet"))) {
				STAsubnet[0] = json[F("STAsubnet")][0];
				STAsubnet[1] = json[F("STAsubnet")][1];
				STAsubnet[2] = json[F("STAsubnet")][2];
				STAsubnet[3] = json[F("STAsubnet")][3];
			}
			else {
				GlobalErrorMessage = F("loadConfig ERROR: STAsubnet key missing");
			}
		//Set gw
			if (json.containsKey(F("STAgateway"))) {
				STAgateway[0] = json[F("STAgateway")][0];
				STAgateway[1] = json[F("STAgateway")][1];
				STAgateway[2] = json[F("STAgateway")][2];
				STAgateway[3] = json[F("STAgateway")][3];
			}
			else {
				GlobalErrorMessage = F("loadConfig ERROR: STAgateway key missing");
			}		

		//Get scaling for Data0
			if (json.containsKey(F("Data0"))) {
				Data[0].rawLo = json[F("Data0")][0];
				Data[0].rawHi = json[F("Data0")][1];
				Data[0].scaleLo = json[F("Data0")][2];
				Data[0].scaleHi = json[F("Data0")][3];
				Data[0].EngUnits = json[F("Data0")][4].asString();
			}
			else {
				GlobalErrorMessage = F("loadConfig ERROR: Data0 key missing");
			}
		//Get scaling for Data1
			if (json.containsKey(F("Data1"))) {
				Data[1].rawLo = json[F("Data1")][0];
				Data[1].rawHi = json[F("Data1")][1];
				Data[1].scaleLo = json[F("Data1")][2];
				Data[1].scaleHi = json[F("Data1")][3];
				Data[1].EngUnits = json[F("Data1")][4].asString();
				Data[1].precisionRaw = json[F("Data1")][5];
				Data[1].precision = json[F("Data1")][6];
			}
			else {
				GlobalErrorMessage = F("loadConfig ERROR: Data1 key missing");
			}
		//Get scaling for Data2			
			if (json.containsKey(F("Data2"))) {
				Data[2].rawLo = json[F("Data2")][0];
				Data[2].rawHi = json[F("Data2")][1];
				Data[2].scaleLo = json[F("Data2")][2];
				Data[2].scaleHi = json[F("Data2")][3];
				Data[2].EngUnits = json[F("Data2")][4].asString();
				Data[2].precisionRaw = json[F("Data2")][5];
				Data[2].precision = json[F("Data2")][6];
			}
			else {
				GlobalErrorMessage = F("loadConfig ERROR: Data2 key missing");
			}


		return true;
	}
	else { //failure to parse json
		GlobalErrorMessage = F("loadConfig ERROR: Failed to parse config.json.txt");
	}

	//Process any error by printing it to Serial and returning
	f.close();
	ProcessErrorMessage();
	return false;

}//===============================================================================================
void saveConfig() {

	File f = SPIFFS.open(F("/config.json.txt"), "w");
	if (!f) EndProgram(F("saveConfig ERROR: Failed to open config.json.txt File from SPIFFS for writing"));

	Serial.println(F("Saving configuration to /config.json.txt"));
	//Allocate memory
		StaticJsonBuffer<CONFIGJSONFILZESIZE> jsonBuffer; //allocate memory
	//create a Json Object
		JsonObject& json = jsonBuffer.createObject(); //create a json object
	//Create member
		json[F("ProjectName")] = ProjectName;
	//Create member
		json[F("AutoDHCP")] = AutoDHCP;

	//Create member
		json[F("NTPServerName")] = NTPServerName;
	//Create member
		json[F("NTPTimeZone")] = NTPTimeZone;
	//Create member
		json[F("NTPdstRule")] = NTPdstRule;

	//Create member
		json[F("EmailAddress")] = EmailAddress;
		
	//Create member
		json[F("ThingSpeakEnable")] = ThingSpeakEnable;
	//Create member
		json[F("ThingSpeakChannel")] = ThingSpeakChannel;
	//Create member
		json[F("ThingSpeakapiKey")] = ThingSpeakapiKey;

	//create a station IP member
		JsonArray& jsonSTAip = json.createNestedArray(F("STAip")); //create an array then add each member
		jsonSTAip.add(STAip[0]);
		jsonSTAip.add(STAip[1]);
		jsonSTAip.add(STAip[2]);
		jsonSTAip.add(STAip[3]);
	//Create subnet Mask member
		JsonArray& jsonSTAsubnet = json.createNestedArray(F("STAsubnet"));//create an array then add each member
		jsonSTAsubnet.add(STAsubnet[0]);
		jsonSTAsubnet.add(STAsubnet[1]);
		jsonSTAsubnet.add(STAsubnet[2]);
		jsonSTAsubnet.add(STAsubnet[3]);
	//Create Gateway member
		JsonArray& jsonSSTAgateway = json.createNestedArray(F("STAgateway"));//create an array then add each member
		jsonSSTAgateway.add(STAgateway[0]);
		jsonSSTAgateway.add(STAgateway[1]);
		jsonSSTAgateway.add(STAgateway[2]);
		jsonSSTAgateway.add(STAgateway[3]);
	//Create Data1 member
		JsonArray& jsonData0 = json.createNestedArray(F("Data0"));//create an array then add each member
		jsonData0.add(Data[0].rawLo, 6);  
		jsonData0.add(Data[0].rawHi, 6);
		jsonData0.add(Data[0].scaleLo, 6);
		jsonData0.add(Data[0].scaleHi, 6);
		jsonData0.add(Data[0].EngUnits);
	//Create Data1 member
		JsonArray& jsonData1 = json.createNestedArray(F("Data1"));//create an array then add each member
		jsonData1.add(Data[1].rawLo,6);
		jsonData1.add(Data[1].rawHi, 6);
		jsonData1.add(Data[1].scaleLo, 6);
		jsonData1.add(Data[1].scaleHi, 6);
		jsonData1.add(Data[1].EngUnits);
	//Create Data1 member
		JsonArray& jsonData2 = json.createNestedArray(F("Data2"));//create an array then add each member
		jsonData2.add(Data[2].rawLo,6);
		jsonData2.add(Data[2].rawHi, 6);
		jsonData2.add(Data[2].scaleLo, 6);
		jsonData2.add(Data[2].scaleHi, 6);
		jsonData2.add(Data[2].EngUnits);

	//print Json to file
	if (json.prettyPrintTo(f) <= 0) GlobalErrorMessage = F("saveConfig ERROR: File to save was <= 0B");

	f.close();

}//===============================================================================================
bool hasInvalidChar(String s) {
	//Checks a string for any special characters.
	//Return 1 if there are any non alpha number characters: https://msdn.microsoft.com/en-us/library/cc875839.aspx 
	const char invalidChars[32] = { '(', ')', '`', '~', '!', '@', '#', '$', '%', '^', '&', '*', '-', '+', '=', '|', '\\', '{', '}', '[', ']', ':', ';', '\'', '\"', ' ', '<', '>', ',', '.', '?', '/' };
	for (int i = 0; i < 32; i++) {
		for (int j = 0; j < s.length(); j++) {
			if (s[j] == invalidChars[i]) {
				//Serial.print("Invalid Char detected(");
				//Serial.print(invalidChars[i]);
				//Serial.print(") in string: ");
				//Serial.print(s);
				//Serial.print(", at position ");
				//Serial.print(j);
				return true;
			}
		}
	}
	return false;
}
Gsender* Gsender::Instance()
{
	if (_instance == 0)
		_instance = new Gsender;
	return _instance;
}

Gsender* Gsender::Subject(const char* subject)
{
	delete[] _subject;
	_subject = new char[strlen(subject) + 1];
	strcpy(_subject, subject);
	return _instance;
}
Gsender* Gsender::Subject(const String &subject)
{
	return Subject(subject.c_str());
}

bool Gsender::AwaitSMTPResponse(WiFiClientSecure &client, const String &resp, uint16_t timeOut)
{
	uint32_t ts = millis();
	while (!client.available())
	{
		if (millis() > (ts + timeOut)) {
			_error = "SMTP Response TIMEOUT!";
			return false;
		}
	}
	_serverResponce = client.readStringUntil('\n');
#if defined(GS_SERIAL_LOG_1) || defined(GS_SERIAL_LOG_2) 
	Serial.println(_serverResponce);
#endif
	if (resp && _serverResponce.indexOf(resp) == -1) return false;
	return true;
}

String Gsender::getLastResponce()
{
	return _serverResponce;
}

const char* Gsender::getError()
{
	return _error;
}

bool Gsender::Send(const String &to, const String &message)
{
	WiFiClientSecure client;
#if defined(GS_SERIAL_LOG_2)
	Serial.print("Connecting to :");
	Serial.println(SMTP_SERVER);
#endif
	if (!client.connect(SMTP_SERVER, SMTP_PORT)) {
		_error = "Could not connect to mail server";
		return false;
	}
	if (!AwaitSMTPResponse(client, "220")) {
		_error = "Connection Error";
		return false;
	}

#if defined(GS_SERIAL_LOG_2)
	Serial.println("HELO friend:");
#endif
	client.println("HELO friend");
	if (!AwaitSMTPResponse(client, "250")) {
		_error = "identification error";
		return false;
	}

#if defined(GS_SERIAL_LOG_2)
	Serial.println("AUTH LOGIN:");
#endif
	client.println("AUTH LOGIN");
	AwaitSMTPResponse(client);

#if defined(GS_SERIAL_LOG_2)
	Serial.println("EMAILBASE64_LOGIN:");
#endif
	client.println(EMAILBASE64_LOGIN);
	AwaitSMTPResponse(client);

#if defined(GS_SERIAL_LOG_2)
	Serial.println("EMAILBASE64_PASSWORD:");
#endif
	client.println(EMAILBASE64_PASSWORD);
	if (!AwaitSMTPResponse(client, "235")) {
		_error = "SMTP AUTH error";
		return false;
	}

	String mailFrom = "MAIL FROM: <" + String(FROM) + '>';
#if defined(GS_SERIAL_LOG_2)
	Serial.println(mailFrom);
#endif
	client.println(mailFrom);
	AwaitSMTPResponse(client);

	String rcpt = "RCPT TO: <" + to + '>';
#if defined(GS_SERIAL_LOG_2)
	Serial.println(rcpt);
#endif
	client.println(rcpt);
	AwaitSMTPResponse(client);

#if defined(GS_SERIAL_LOG_2)
	Serial.println("DATA:");
#endif
	client.println("DATA");
	if (!AwaitSMTPResponse(client, "354")) {
		_error = "SMTP DATA error";
		return false;
	}

	client.println("From: <" + String(FROM) + '>');
	client.println("To: <" + to + '>');

	client.print("Subject: ");
	client.println(_subject);

	client.println("Mime-Version: 1.0");
	client.println("Content-Type: text/html; charset=\"UTF-8\"");
	client.println("Content-Transfer-Encoding: 7bit");
	client.println();
	String body = "<!DOCTYPE html><html lang=\"en\">" + message + "</html>";
	client.println(body);
	client.println(".");
	if (!AwaitSMTPResponse(client, "250")) {
		_error = "Sending message error";
		return false;
	}
	client.println("QUIT");
	if (!AwaitSMTPResponse(client, "221")) {
		_error = "SMTP QUIT error";
		return false;
	}
	return true;
}//===============================================================================================