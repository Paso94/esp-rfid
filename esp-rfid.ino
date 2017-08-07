/*
  Copyright (c) 2017 Omer Siar Baysal

  Released to Public Domain

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.

  The following table shows the typical pin layout used:

  | Signal        | MFRC522       | WeMos D1 mini  | NodeMcu | Generic      |
  |---------------|:-------------:|:--------------:| :------:|:------------:|
  | RST/Reset     | RST           | NC             | NC      | NC           |
  | SPI SS        | SDA [3]       | D8 [2]         | D8 [2]  | GPIO-15 [2]  |
  | SPI MOSI      | MOSI          | D7             | D7      | GPIO-13      |
  | SPI MISO      | MISO          | D6             | D6      | GPIO-12      |
  | SPI SCK       | SCK           | D5             | D5      | GPIO-14      |

  NC. Not Connected
  2. Configurable via web page
  3. The SDA pin might be labeled SS on some/older MFRC522 boards.

*/

#include <ESP8266WiFi.h>              // Whole thing is about using Wi-Fi networks
#include <SPI.h>                      // RFID MFRC522 Module uses SPI protocol
#include <ESP8266mDNS.h>              // Zero-config Library (Bonjour, Avahi) http://esp-rfid.local
#include <DNSServer.h>                // Used for captive portal
#include <MFRC522.h>                  // Library for Mifare RC522 Devices
#include <ArduinoJson.h>              // JSON Library for Encoding and Parsing Json object to send browser. We do that because Javascript has built-in JSON parsing.
#include <FS.h>                       // SPIFFS Library for storing web files to serve to web browsers
#include <ESPAsyncTCP.h>              // Async TCP Library is mandatory for Async Web Server
#include <ESPAsyncWebServer.h>        // Async Web Server with built-in WebSocket Plug-in
#include <SPIFFSEditor.h>             // This creates a web page on server which can be used to edit text based files.
#include <TimeLib.h>                  // Library for converting epochtime to a date
#include <WiFiUdp.h>                  // Library for manipulating UDP packets which is used by NTP Client to get Timestamps

// Variables for whole scope
String filename = "/P/";
//flag to use from web update to reboot the ESP
bool shouldReboot = false;
bool activateRelay = false;
unsigned long previousMillis = 0;
int relayPin;
int activateTime;
const byte DNS_PORT = 53;
bool inAPMode = false;
int timeZone;

WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets

time_t getNtpTime();
void sendNTPpacket(IPAddress &address);
char ntpServerName[44];
int ntpinter;

IPAddress apIP(192, 168, 4, 1);

DNSServer dnsServer;

// Create MFRC522 RFID instance
MFRC522 mfrc522 = MFRC522();

// Create AsyncWebServer instance on port "80"
AsyncWebServer server(80);
// Create WebSocket instance on URL "/ws"
AsyncWebSocket ws("/ws");

// Set things up
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("[ INFO ] ESP RFID v0.2rc2"));

  // Start SPIFFS filesystem
  SPIFFS.begin();

  // Try to load configuration file so we can connect to an Wi-Fi Access Point
  // Do not worry if no config file is present, we fall back to Access Point mode and device can be easily configured
  if (!loadConfiguration()) {
    fallbacktoAPMode();
  }

  // Start WebSocket Plug-in and handle incoming message on "onWsEvent" function
  server.addHandler(&ws);
  ws.onEvent(onWsEvent);

  // Configure web server
  // Add Text Editor (http://esp-rfid.local/edit) to Web Server. This feature likely will be dropped on final release.
  server.addHandler(new SPIFFSEditor("admin", "admin"));


  // Serve all files in root folder
  server.serveStatic("/", SPIFFS, "/");
  // Handle what happens when requested web file couldn't be found
  server.onNotFound([](AsyncWebServerRequest * request) {
    if (captivePortal(request)) { // If captive portal redirect instead of displaying the error page.
      return;
    }

    String message = "File Not Found\n\n";
    message += "URI: ";
    message += request->url();
    message += "\nMethod: ";
    message += ( request->method() == HTTP_GET ) ? "GET" : "POST";
    message += "\nArguments: ";
    message += request->args();
    message += "\n";

    for (uint8_t i = 0; i < request->args(); i++ ) {
      message += " " + request->argName ( i ) + ": " + request->arg ( i ) + "\n";
    }

    AsyncWebServerResponse *response = request->beginResponse(404, "text/plain", message);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
  });

  // Simple Firmware Update Handler
  server.on("/auth/update", HTTP_POST, [](AsyncWebServerRequest * request) {
    shouldReboot = !Update.hasError();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "OK" : "FAIL");
    response->addHeader("Connection", "close");
    request->send(response);
  }, [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      Serial.printf("[ UPDT ] Firmware update started: %s\n", filename.c_str());
      Update.runAsync(true);
      if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
        Update.printError(Serial);
      }
    }
    if (!Update.hasError()) {
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }
    }
    if (final) {
      if (Update.end(true)) {
        Serial.printf("[ UPDT ] Firmware update finished: %uB\n", index + len);
      } else {
        Update.printError(Serial);
      }
    }
  });


  //Setting up dns for the captive portal
  dnsServer.start(53, "*", apIP);

  Udp.begin(localPort);

  if (!inAPMode) {
    setSyncProvider(getNtpTime);
    setSyncInterval(ntpinter * 60);
  }

  // Start Web Server
  server.begin();
}

// Main Loop
void loop() {
  // check for a new update and restart
  if (shouldReboot) {
    Serial.println(F("[ UPDT ] Rebooting..."));
    delay(100);
    ESP.restart();
  }
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= activateTime) {
    activateRelay = false;
    digitalWrite(relayPin, HIGH);
  }
  if (activateRelay) {
    digitalWrite(relayPin, LOW);
  }
  dnsServer.processNextRequest();
  

  // Another loop for RFID Events, since we are using polling method instead of Interrupt we need to check RFID hardware for events
  rfidloop();
}

boolean captivePortal(AsyncWebServerRequest *request) {
  if (!isIp(request->host()) ) {
    AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
    response->addHeader("Location", String("http://" + ipToString(apIP)));
    request->send(response);
    return true;
  }
  return false;
}

boolean isIp(String str) {
  for (int i = 0; i < str.length(); i++) {
    int c = str.charAt(i);
    if (c != '.' && (c < '0' || c > '9')) {
      return false;
    }
  }
  return true;
}

String ipToString(IPAddress ip) {
  String res = "";
  for (int i = 0; i < 3; i++) {
    res += String((ip >> (8 * i)) & 0xFF) + ".";
  }
  res += String(((ip >> 8 * 3)) & 0xFF);
  return res;
}


// RFID Specific Loop
void rfidloop() {
  //If a new PICC placed to RFID reader continue
  if ( ! mfrc522.PICC_IsNewCardPresent()) {
    delay(50);
    return;
  }
  //Since a PICC placed get Serial (UID) and continue
  if ( ! mfrc522.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }
  // We got UID tell PICC to stop responding
  mfrc522.PICC_HaltA();

  // There are Mifare PICCs which have 4 byte or 7 byte UID
  // Get PICC's UID and store on a variable
  Serial.print(F("[ INFO ] PICC's UID: "));
  String uid = "";
  for (int i = 0; i < mfrc522.uid.size; ++i) {
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.print(uid);
  // Get PICC type
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  String type = mfrc522.PICC_GetTypeName(piccType);

  // We are going to use filesystem to store known UIDs.
  int isKnown = 0;  // First assume we don't know until we got a match
  // If we know the PICC we need to know if its User have an Access
  int haveAcc = 0;  // First assume User do not have access
  // Prepend /P/ on filename so we distinguish UIDs from the other files
  filename = "/P/";
  filename += uid;

  File f = SPIFFS.open(filename, "r");
  // Check if we could find it above function returns true if the file is exist
  if (f) {
    isKnown = 1; // we found it and label it as known
    // Now we need to read contents of the file to parse JSON object contains Username and Access Status
    size_t size = f.size();
    // Allocate a buffer to store contents of the file.
    std::unique_ptr<char[]> buf(new char[size]);
    // We don't use String here because ArduinoJson library requires the input
    // buffer to be mutable. If you don't use ArduinoJson, you may as well
    // use configFile.readString instead.
    f.readBytes(buf.get(), size);
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.parseObject(buf.get());
    // Check if we succesfully parse JSON object
    if (json.success()) {
      // Get username Access Status
      String username = json["user"];
      haveAcc = json["haveAcc"];
      Serial.println(" = known PICC");
      Serial.print("[ INFO ] User Name: ");
      Serial.print(username);
      // Check if user have an access
      if (haveAcc == 1) {
        activateRelay = true;  // Give user Access to Door, Safe, Box whatever you like
        previousMillis = millis();
        Serial.println(" have access");
      }
      else {
        Serial.println(" does not have access");
      }
      // Also inform Administrator Portal
      // Encode a JSON Object and send it to All WebSocket Clients
      DynamicJsonBuffer jsonBuffer2;
      JsonObject& root = jsonBuffer2.createObject();
      root["command"] = "piccscan";
      // UID of Scanned RFID Tag
      root["uid"] = uid;
      // Type of PICC
      root["type"] = type;
      // A boolean 1 for known tags 0 for unknown
      root["known"] = isKnown;
      // A boolean 1 for granted 0 for denied access
      root["access"] = haveAcc;
      // Username
      root["user"] = username;
      size_t len = root.measureLength();
      AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
      if (buffer) {
        root.printTo((char *)buffer->get(), len + 1);
        ws.textAll(buffer);
      }
    }
    else {
      Serial.println("");
      Serial.println(F("[ WARN ] Failed to parse User Data"));
    }
    f.close();
  }
  else {
    // If we don't know the UID, inform Administrator Portal so admin can give access or add it to database
    Serial.println(" = unknown PICC");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["command"] = "piccscan";
    // UID of Scanned RFID Tag
    root["uid"] = uid;
    // Type of PICC
    root["type"] = type;
    // A boolean 1 for known tags 0 for unknown
    root["known"] = isKnown;
    size_t len = root.measureLength();
    AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
    if (buffer) {
      root.printTo((char *)buffer->get(), len + 1);
      ws.textAll(buffer);
    }
  }
  // So far got we got UID of Scanned RFID Tag, checked it if it's on the database and access status, informed Administrator Portal
}

// Handles WebSocket Events
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_ERROR) {
    Serial.printf("[ WARN ] WebSocket[%s][%u] error(%u): %s\r\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  }
  else if (type == WS_EVT_DATA) {

    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    String msg = "";

    if (info->final && info->index == 0 && info->len == len) {
      //the whole message is in a single frame and we got all of it's data
      for (size_t i = 0; i < info->len; i++) {
        msg += (char) data[i];
      }

      // We should always get a JSON object (stringfied) from browser, so parse it
      DynamicJsonBuffer jsonBuffer;
      JsonObject& root = jsonBuffer.parseObject(msg);
      if (!root.success()) {
        Serial.println(F("[ WARN ] Couldn't parse WebSocket message"));
        return;
      }

      // Web Browser sends some commands, check which command is given
      const char * command = root["command"];

      // Check whatever the command is and act accordingly
      if (strcmp(command, "remove")  == 0) {
        const char* uid = root["uid"];
        filename = "/P/";
        filename += uid;
        SPIFFS.remove(filename);
      }
      else if (strcmp(command, "configfile")  == 0) {
        File f = SPIFFS.open("/auth/config.json", "w+");
        if (f) {
          root.prettyPrintTo(f);
          //f.print(msg);
          f.close();
          ESP.reset();
        }
      }
      else if (strcmp(command, "picclist")  == 0) {
        sendPICClist();
      }
      else if (strcmp(command, "status")  == 0) {
        sendStatus();
      }
      else if (strcmp(command, "userfile")  == 0) {
        const char* uid = root["uid"];
        filename = "/P/";
        filename += uid;
        File f = SPIFFS.open(filename, "w+");
        // Check if we created the file
        if (f) {
          f.print(msg);
          f.close();
        }
      }
      else if (strcmp(command, "testrelay")  == 0) {
        activateRelay = true;
        previousMillis = millis();
      }
      else if (strcmp(command, "scan")  == 0) {
        WiFi.scanNetworksAsync(printScanResult);
      }
      else if (strcmp(command, "gettime")  == 0) {
        sendTime();
      }
      else if (strcmp(command, "settime")  == 0) {
        unsigned long t = root["epoch"];
        setTime(t);
        sendTime();
      }
      else if (strcmp(command, "getconf")  == 0) {
        File configFile = SPIFFS.open("/auth/config.json", "r");
        if (configFile) {
          size_t len = configFile.size();
          AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
          if (buffer) {
            configFile.readBytes((char *)buffer->get(), len + 1);
            ws.textAll(buffer);
          }
          configFile.close();
        }
      }
    }
  }
}

void sendTime() {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["command"] = "gettime";
  root["epoch"] = now();
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
}

void sendPICClist() {
  Dir dir = SPIFFS.openDir("/P/");
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["command"] = "picclist";

  JsonArray& data = root.createNestedArray("piccs");
  JsonArray& data2 = root.createNestedArray("users");
  JsonArray& data3 = root.createNestedArray("access");
  while (dir.next()) {
    File f = SPIFFS.open(dir.fileName(), "r");
    size_t size = f.size();
    // Allocate a buffer to store contents of the file.
    std::unique_ptr<char[]> buf(new char[size]);
    // We don't use String here because ArduinoJson library requires the input
    // buffer to be mutable. If you don't use ArduinoJson, you may as well
    // use configFile.readString instead.
    f.readBytes(buf.get(), size);
    DynamicJsonBuffer jsonBuffer2;
    JsonObject& json = jsonBuffer2.parseObject(buf.get());
    if (json.success()) {
      String username = json["user"];
      int haveAcc = json["haveAcc"];
      data2.add(username);
      data3.add(haveAcc);
    }
    data.add(dir.fileName());
  }
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
}

void sendStatus() {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["command"] = "status";
  root["heap"] = ESP.getFreeHeap();
  root["chipid"] = String(ESP.getChipId(), HEX);
  root["cpu"] = ESP.getCpuFreqMHz();
  root["availsize"] = ESP.getFreeSketchSpace();
  root["ssid"] = (String)WiFi.SSID();
  root["ip"] = (String)WiFi.localIP()[0] + "." + (String)WiFi.localIP()[1] + "." + (String)WiFi.localIP()[2] + "." + (String)WiFi.localIP()[3];
  root["gateway"] = (String)WiFi.gatewayIP()[0] + "." + (String)WiFi.gatewayIP()[1] + "." + (String)WiFi.gatewayIP()[2] + "." + (String)WiFi.gatewayIP()[3];
  root["netmask"] = (String)WiFi.subnetMask()[0] + "." + (String)WiFi.subnetMask()[1] + "." + (String)WiFi.subnetMask()[2] + "." + (String)WiFi.subnetMask()[3];
  root["dns"] = (String)WiFi.dnsIP()[0] + "." + (String)WiFi.dnsIP()[1] + "." + (String)WiFi.dnsIP()[2] + "." + (String)WiFi.dnsIP()[3];
  root["mac"] = getMacAddress();
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
}

String getMacAddress() {
  uint8_t mac[6];
  char macStr[18] = { 0 };
  WiFi.macAddress(mac);
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return  String(macStr);
}

// Send Scanned SSIDs to websocket clients as JSON object
void printScanResult(int networksFound) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["command"] = "ssidlist";
  JsonArray& data = root.createNestedArray("ssid");
  for (int i = 0; i < networksFound; ++i) {
    // Print SSID for each network found
    data.add(WiFi.SSID(i));
  }
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
  WiFi.scanDelete();
}


// Fallback to AP Mode, so we can connect to ESP if there is no Internet connection
void fallbacktoAPMode() {
  inAPMode = true;
  WiFi.mode(WIFI_AP);
  Serial.print(F("[ INFO ] Configuring access point... "));
  Serial.println(WiFi.softAP("ESP-RFID") ? "Ready" : "Failed!");
  // Access Point IP
  IPAddress myIP = WiFi.softAPIP();
  Serial.print(F("[ INFO ] AP IP address: "));
  Serial.println(myIP);
  server.serveStatic("/auth/", SPIFFS, "/auth/").setDefaultFile("users.htm").setAuthentication("admin", "admin");
}

bool loadConfiguration() {
  File configFile = SPIFFS.open("/auth/config.json", "r");
  if (!configFile) {
    Serial.println(F("[ WARN ] Failed to open config file"));
    return false;
  }
  size_t size = configFile.size();
  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);
  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());
  if (!json.success()) {
    Serial.println(F("[ WARN ] Failed to parse config file"));
    return false;
  }
  Serial.println(F("[ INFO ] Config file found"));
  json.prettyPrintTo(Serial);
  Serial.println();
  int rfidss = json["sspin"];
  int rfidgain = json["rfidgain"];
  setupRFID(rfidss, rfidgain);

  const char * hstname = json["hostnm"];

  // Set Hostname.
  WiFi.hostname(hstname);

  // Start mDNS service so we can connect to http://esp-rfid.local (if Bonjour installed on Windows or Avahi on Linux)
  if (!MDNS.begin(hstname)) {
    Serial.println("Error setting up MDNS responder!");
  }
  // Add Web Server service to mDNS
  MDNS.addService("http", "tcp", 80);

  const char * ntpserver = json["ntpserver"];
  strcpy (ntpServerName, ntpserver);
  ntpinter = json["ntpinterval"];
  timeZone = json["timezone"];




  activateTime = json["rtime"];
  relayPin = json["rpin"];
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);

  const char * ssid = json["ssid"];
  const char * password = json["pswd"];
  int wmode = json["wmode"];

  const char * adminpass = json["adminpwd"];

  // Serve confidential files in /auth/ folder with a Basic HTTP authentication
  server.serveStatic("/auth/", SPIFFS, "/auth/").setDefaultFile("users.htm").setAuthentication("admin", adminpass);

  if (wmode == 1) {
    inAPMode = true;
    Serial.println(F("[ INFO ] ESP-RFID is running in AP Mode "));
    WiFi.mode(WIFI_AP);
    Serial.print(F("[ INFO ] Configuring access point... "));
    Serial.println(WiFi.softAP(ssid, password) ? "Ready" : "Failed!");
    // Access Point IP
    IPAddress myIP = WiFi.softAPIP();
    Serial.print(F("[ INFO ] AP IP address: "));
    Serial.println(myIP);
    Serial.print(F("[ INFO ] AP SSID: "));
    Serial.println(ssid);
    return true;
  }
  else if (!connectSTA(ssid, password)) {
    return false;
  }

  return true;
}

// Configure RFID Hardware
void setupRFID(int rfidss, int rfidgain) {
  SPI.begin();           // MFRC522 Hardware uses SPI protocol
  mfrc522.PCD_Init(rfidss, UINT8_MAX);    // Initialize MFRC522 Hardware
  // Set RFID Hardware Antenna Gain
  // This may not work with some boards
  mfrc522.PCD_SetAntennaGain(rfidgain);
  Serial.printf("[ INFO ] RFID SS_PIN: %u and Gain Factor: %u", rfidss, rfidgain);
  Serial.println("");
  ShowReaderDetails(); // Show details of PCD - MFRC522 Card Reader details
}

// Try to connect Wi-Fi
bool connectSTA(const char* ssid, const char* password) {
  WiFi.mode(WIFI_STA);
  // First connect to a wi-fi network
  WiFi.begin(ssid, password);
  // Inform user we are trying to connect
  Serial.print(F("[ INFO ] Trying to connect WiFi: "));
  Serial.print(ssid);
  // We try it for 20 seconds and give up on if we can't connect
  unsigned long now = millis();
  uint8_t timeout = 20; // define when to time out in seconds
  // Wait until we connect or 20 seconds pass
  do {
    if (WiFi.status() == WL_CONNECTED) {
      break;
    }
    delay(500);
    Serial.print(F("."));
  }
  while (millis() - now < timeout * 1000);
  // We now out of the while loop, either time is out or we connected. check what happened
  if (WiFi.status() == WL_CONNECTED) { // Assume time is out first and check
    Serial.println();
    Serial.print(F("[ INFO ] Client IP address: ")); // Great, we connected, inform
    Serial.println(WiFi.localIP());
    return true;
  }
  else { // We couln't connect, time is out, inform
    Serial.println();
    Serial.println(F("[ WARN ] Couldn't connect in time"));
    return false;
  }
}

void ShowReaderDetails() {
  // Get the MFRC522 software version
  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  Serial.print(F("[ INFO ] MFRC522 Version: 0x"));
  Serial.print(v, HEX);
  if (v == 0x91)
    Serial.print(F(" = v1.0"));
  else if (v == 0x92)
    Serial.print(F(" = v2.0"));
  else if (v == 0x88)
    Serial.print(F(" = clone"));
  else
    Serial.print(F(" (unknown)"));
  Serial.println("");
  // When 0x00 or 0xFF is returned, communication probably failed
  if ((v == 0x00) || (v == 0xFF)) {
    Serial.println(F("[ WARN ] Communication failure, check if MFRC522 properly connected"));
  }
}

/*-------- NTP code ----------*/

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t getNtpTime() {
  IPAddress ntpServerIP; // NTP server's ip address
  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}
