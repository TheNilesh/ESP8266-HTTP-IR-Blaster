#include <FS.h>                                               // This needs to be first, or it all crashes and burns

#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>                                      // https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <ESP8266mDNS.h>                                      // Useful to access to ESP by hostname.local

#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <Ticker.h>                                           // For LED status

// User settings are below here
const unsigned int captureBufSize = 150;                      // Size of the IR capture buffer.

const int receiverPin = 14;                                   // Receiving pin
const int senderPin = 4;                                      // IR sender pin can be 4, 5, 12, 13
const int configpin = 10;                                     // Reset Pin

// User settings are above here
const int ledpin = LED_BUILTIN;                               // Built in LED defined for WEMOS people
const char *wifiConfigName = "IR Remote Config";              // SSID for this
int port = 80;
char password[20] = "";
char hostName[20] = "irremote";
char portStr[6] = "80";
char username[60] = "";

char staticIp[16] = "10.0.1.10";
char staticGw[16] = "10.0.1.1";
char staticSn[16] = "255.255.255.0";

ESP8266WebServer *server = NULL;
Ticker ticker;

bool shouldSaveConfig = false;                                // Flag for saving data
bool holdReceive = false;                                     // Flag to prevent IR receiving while transmitting

IRrecv irrecv(receiverPin, captureBufSize); //receive IR signal from irrecv
IRsend irsend(senderPin);                   //send IR signal to irsend

class Code {
  public:
    char encoding[14] = "";
    char address[20] = "";
    char command[40] = "";
    char data[40] = "";
    String raw = "";
    int bits = 0;
    char timestamp[40] = "";
    bool valid = false;
};

Code lastRecvdCode;
Code lastSentCode;

//+=============================================================================
// Callback notifying us of the need to save config
//
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}


//+=============================================================================
// Reenable IR receiving
//
void resetReceive() {
  if (holdReceive) {
    Serial.println("Reenabling receiving");
    irrecv.resume();
    holdReceive = false;
  }
}

// Toggle state
void tick()
{
  int state = digitalRead(ledpin);  // get the current state of GPIO1 pin
  digitalWrite(ledpin, !state);     // set pin to the opposite state
}

// Turn off the Led after timeout
void disableLed()
{
  Serial.println("Turning off the LED to save power.");
  digitalWrite(ledpin, HIGH);                           // Shut down the LED
  ticker.detach();                                      // Stopping the ticker
}

// Gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  //entered config mode, make led toggle faster
  ticker.attach(0.2, tick);
}


// Gets called when device loses connection to the accesspoint
void lostWifiCallback (const WiFiEventStationModeDisconnected& evt) {
  Serial.println("Lost Wifi");
  // reset and try again, or maybe put it to deep sleep
  ESP.reset();
  delay(1000);
}


// First setup of the Wifi.
// If return true, the Wifi is well connected.
// Should not return false if Wifi cannot be connected, it will loop
bool setupWifi(bool resetConf) {
  // start ticker with 0.5 because we start in AP mode and try to connect
  ticker.attach(0.5, tick);
  WiFiManager wifiManager;
  if (resetConf) {
    wifiManager.resetSettings();
  }

  // set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);
  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Reset device if on config portal for greater than 3 minutes
  wifiManager.setConfigPortalTimeout(180);

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          if (json.containsKey("hostname")) strncpy(hostName, json["hostname"], 20);
          if (json.containsKey("password")) strncpy(password, json["password"], 20);
          if (json.containsKey("username")) strncpy(username, json["username"], 60);
          if (json.containsKey("portStr")) {
            strncpy(portStr, json["portStr"], 6);
            port = atoi(json["portStr"]);
          }
          if (json.containsKey("ip")) strncpy(staticIp, json["ip"], 16);
          if (json.containsKey("gw")) strncpy(staticGw, json["gw"], 16);
          if (json.containsKey("sn")) strncpy(staticSn, json["sn"], 16);
        } else {
          Serial.println("Failed to parse json from config file");
        }
      } //if(configFile)
    } //if configFile present
  } else {
    Serial.println("failed to mount FS");
  }

  WiFiManagerParameter customHostname("hostname", "Hostname", hostName, 20);
  wifiManager.addParameter(&customHostname);
  WiFiManagerParameter customPort("portStr", "Web Server Port", portStr, 6);
  wifiManager.addParameter(&customPort);
  WiFiManagerParameter customUsername("username", "Username", username, 60);
  wifiManager.addParameter(&customUsername);
  WiFiManagerParameter customPassword("password", "Password", password, 20);
  wifiManager.addParameter(&customPassword);

  IPAddress sip, sgw, ssn;
  sip.fromString(staticIp);
  sgw.fromString(staticGw);
  ssn.fromString(staticSn);
  Serial.println("Using Static IP");
  wifiManager.setSTAStaticIPConfig(sip, sgw, ssn);

  // fetches ssid and pass and tries to connect
  // if it does not connect it starts an access point with the specified name
  // and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect(wifiConfigName)) {
    Serial.println("Failed to connect and hit timeout");
    // reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }
  Serial.println("Connected to WiFi");
  strncpy(hostName, customHostname.getValue(), 20);
  strncpy(password, customPassword.getValue(), 20);
  strncpy(portStr, customPort.getValue(), 6);
  strncpy(username, customUsername.getValue(), 60);
  port = atoi(portStr);

  if (server != NULL) {
    delete server;
  }
  server = new ESP8266WebServer(port);
  Serial.println("Created ESP8266WebServer");
  // Reset device if lost wifi Connection
  WiFi.onStationModeDisconnected(&lostWifiCallback);

  Serial.println("WiFi connected! User chose hostname '" + String(hostName) + String("' password '") + String(password) + "' and port '" + String(portStr) + "'");

  // save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println(" config...");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["hostname"] = hostName;
    json["password"] = password;
    json["portStr"] = portStr;
    json["username"] = username;
    json["ip"] = WiFi.localIP().toString();
    json["gw"] = WiFi.gatewayIP().toString();
    json["sn"] = WiFi.subnetMask().toString();

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    Serial.println("");
    Serial.println("Writing config file");
    json.printTo(configFile);
    configFile.close();
    jsonBuffer.clear();
    Serial.println("Config written successfully");
  }
  ticker.detach();

  // keep LED on
  digitalWrite(ledpin, LOW);
  return true;
}

// Setup web server and IR receiver/blaster
void handleRoot() {
  Serial.println("Connection received at home page");
  server->send(200, "text/plain", "Welcome to IR Blaster");
}

void doGet() {
  Serial.println("GET received");
  Serial.println("Connection received to obtain last received ir signal");
  if (lastRecvdCode.valid) {
    String json = "";
    // send last received
    if (String(lastRecvdCode.encoding) == "UNKNOWN") {
      json = "[{\"data\":[" + String(lastRecvdCode.raw) + "],\"type\":\"raw\",\"khz\":38}]";
    } else if (String(lastRecvdCode.encoding) == "PANASONIC") {
      json = "[{\"data\":\"" + String(lastRecvdCode.data) + "\",\"type\":\"" + String(lastRecvdCode.encoding) + "\",\"length\":" + String(lastRecvdCode.bits) + ",\"address\":\"" + String(lastRecvdCode.address) + "\"}";
    } else {
      json = "[{\"data\":\"" + String(lastRecvdCode.data) + "\",\"type\":\"" + String(lastRecvdCode.encoding) + "\",\"length\":" + String(lastRecvdCode.bits) + "}]";
    }
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "application/json", json);
  } else {
    server->send(404, "application/json", "{\"error\":\"No code received\"}");
  }
}

void doPost() {
  Serial.println("POST received");
  Serial.println("Connection received - for sending ir signal");
  if ( ! server->hasArg("plain)) {
    server->send(400, "application/json", "{\"error\":\"JSON not received\"}");
  }
  DynamicJsonBuffer jsonBuffer;
  JsonArray& root = jsonBuffer.parseArray(server->arg("plain"));
  if ( ! root.success()) {
    Serial.println("JSON parsing failed");
    server->send(400, "application/json", "{\"error\":\"JSON parsing failed\"}");
    jsonBuffer.clear();
  } else {
    digitalWrite(ledpin, LOW);
    ticker.attach(0.5, disableLed);
    server->send(200, "application/json", "{\"message\":\"success\"}");
    //Processing array of messages
    for (int i = 0; i < root.size(); i++) {
      String type = root[i]["type"];
      String ip = root[i]["ip"];
      int rdelay = root[i]["rdelay"];
      int pulse = root[i]["pulse"];
      int pdelay = root[i]["pdelay"];
      int repeat = root[i]["repeat"];
      int iout = root[i]["out"];
      int duty = root[i]["duty"];

      if (pulse <= 0) pulse = 1; // Make sure pulse isn't 0
      if (repeat <= 0) repeat = 1; // Make sure repeat isn't 0
      if (pdelay <= 0) pdelay = 100; // Default pdelay
      if (rdelay <= 0) rdelay = 1000; // Default rdelay
      if (duty <= 0) duty = 50; // Default duty

      if (type == "delay") {
        delay(rdelay);
      } else if (type == "raw") {
        JsonArray &raw = root[i]["data"]; // Array of unsigned int values for the raw signal
        int khz = root[i]["khz"];
        if (khz <= 0) khz = 38; // Default to 38khz if not set
        rawblast(raw, khz, rdelay, pulse, pdelay, repeat, irsend, duty);
      } else {
        String data = root[i]["data"];
        String addressString = root[i]["address"];
        long address = strtoul(addressString.c_str(), 0, 0);
        int len = root[i]["length"];
        irblast(type, data, len, rdelay, pulse, pdelay, repeat, address, irsend);
      }
    }
    jsonBuffer.clear();
  }
}

void handleNotFound() {
  server->send(404, "text/plain", "");
}

void setup() {
  // Initialize serial
  Serial.begin(115200);

  // set led pin as output
  pinMode(ledpin, OUTPUT);

  Serial.println("");
  Serial.println("ESP8266 IR Controller");
  pinMode(configpin, INPUT_PULLUP);
  Serial.print("Config pin GPIO-");
  Serial.print(configpin);
  Serial.print(" set to: ");
  Serial.println(digitalRead(configpin));
  if ( ! setupWifi(digitalRead(configpin) == LOW)) {
    return;
  }
  Serial.println("WiFi configuration complete");

  if (strlen(hostName) > 0) {
    WiFi.hostname(hostName);
  } else {
    WiFi.hostname().toCharArray(hostName, 20);
  }

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  wifi_set_sleep_type(LIGHT_SLEEP_T);
  digitalWrite(ledpin, LOW);
  // Turn off the led in 2s
  ticker.attach(2, disableLed);

  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP().toString());
  Serial.println("URL to send commands: http://" + WiFi.localIP().toString() + ":" + portStr);
  MDNS.addService("http", "tcp", port); // Announce the ESP as an HTTP service
  Serial.println("MDNS http service added. Hostname is set to " + String(hostName) + ".local");
  Serial.println("Adding request handlers for HTTP");
  server->on("/ircodes", []() {
    Serial.print("Request recvd");
    //TODO: Enable authentications
    /*
    if ( ! server->authenticate(username, password)) {
      Serial.println("Invalid username/password");
      return server->requestAuthentication();
    }
    Serial.println("Authenticated");
    */
    if (server->method() == HTTP_GET) {
      doGet();
    } else if (server->method() == HTTP_POST) {
      doPost();
    } else {
      server->send(415, "text/plain", "");
    }
  });
  
  server->on("/", handleRoot);
  server->onNotFound(handleNotFound);

  Serial.println("Done adding url handlers. Starting HTTP server");
  server->begin();
  Serial.println("HTTP Server started on port " + String(port));

  irsend.begin();
  irrecv.enableIRIn();
  Serial.println("Ready to send and receive IR signals");
}

// Split string by character
String getValue(String data, char separator, int index)
{
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }

  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

// Display encoding type
String encoding(decode_results *results) {
  String output;
  switch (results->decode_type) {
    default:
    case UNKNOWN:      output = "UNKNOWN";            break;
    case NEC:          output = "NEC";                break;
    case SONY:         output = "SONY";               break;
    case RC5:          output = "RC5";                break;
    case RC6:          output = "RC6";                break;
    case DISH:         output = "DISH";               break;
    case SHARP:        output = "SHARP";              break;
    case JVC:          output = "JVC";                break;
    case SANYO:        output = "SANYO";              break;
    case SANYO_LC7461: output = "SANYO_LC7461";       break;
    case MITSUBISHI:   output = "MITSUBISHI";         break;
    case SAMSUNG:      output = "SAMSUNG";            break;
    case LG:           output = "LG";                 break;
    case WHYNTER:      output = "WHYNTER";            break;
    case AIWA_RC_T501: output = "AIWA_RC_T501";       break;
    case PANASONIC:    output = "PANASONIC";          break;
    case DENON:        output = "DENON";              break;
    case COOLIX:       output = "COOLIX";             break;
    case GREE:         output = "GREE";               break;
  }
  return output;
}

// Code to string
void fullCode (decode_results *results) {
  Serial.print("One line: ");
  serialPrintUint64(results->value, 16);
  Serial.print(":");
  Serial.print(encoding(results));
  Serial.print(":");
  Serial.print(results->bits, DEC);
  if (results->repeat) Serial.print(" (Repeat)");
  Serial.println("");
  if (results->overflow)
    Serial.println("WARNING: IR code too long. "
                   "Edit IRController.ino and increase captureBufSize");
}

// Code to JsonObject
void cvrtCode(Code& codeData, decode_results *results) {
  strncpy(codeData.data, uint64ToString(results->value, 16).c_str(), 40);
  strncpy(codeData.encoding, encoding(results).c_str(), 14);
  codeData.bits = results->bits;
  String r = "";
      for (uint16_t i = 1; i < results->rawlen; i++) {
      r += results->rawbuf[i] * RAWTICK;
      if (i < results->rawlen - 1)
        r += ",";                           // ',' not needed on last one
    }
  codeData.raw = r;
  if (results->decode_type != UNKNOWN) {
    strncpy(codeData.address, ("0x" + String(results->address, HEX)).c_str(), 20);
    strncpy(codeData.command, ("0x" + String(results->command, HEX)).c_str(), 40);
  } else {
    strncpy(codeData.address, "0x0", 20);
    strncpy(codeData.command, "0x0", 40);
  }
}

//+=============================================================================
// Dump out the decode_results structure.
//
void dumpInfo(decode_results *results) {
  if (results->overflow)
    Serial.println("WARNING: IR code too long. "
                   "Edit IRrecv.h and increase RAWBUF");

  // Show Encoding standard
  Serial.print("Encoding  : ");
  Serial.print(encoding(results));
  Serial.println("");

  // Show Code & length
  Serial.print("Code      : ");
  serialPrintUint64(results->value, 16);
  Serial.print(" (");
  Serial.print(results->bits, DEC);
  Serial.println(" bits)");
}


// Dump out the decode_results structure.
void dumpRaw(decode_results *results) {
  // Print Raw data
  Serial.print("Timing[");
  Serial.print(results->rawlen - 1, DEC);
  Serial.println("]: ");

  for (uint16_t i = 1;  i < results->rawlen;  i++) {
    if (i % 100 == 0)
      yield();  // Preemptive yield every 100th entry to feed the WDT.
    uint32_t x = results->rawbuf[i] * RAWTICK;
    if (!(i & 1)) {  // even
      Serial.print("-");
      if (x < 1000) Serial.print(" ");
      if (x < 100) Serial.print(" ");
      Serial.print(x, DEC);
    } else {  // odd
      Serial.print("     ");
      Serial.print("+");
      if (x < 1000) Serial.print(" ");
      if (x < 100) Serial.print(" ");
      Serial.print(x, DEC);
      if (i < results->rawlen - 1)
        Serial.print(", ");  // ',' not needed for last one
    }
    if (!(i % 8)) Serial.println("");
  }
  Serial.println("");  // Newline
}


//+=============================================================================
// Dump out the decode_results structure.
//
void dumpCode(decode_results *results) {
  // Start declaration
  Serial.print("uint16_t  ");              // variable type
  Serial.print("rawData[");                // array name
  Serial.print(results->rawlen - 1, DEC);  // array size
  Serial.print("] = {");                   // Start declaration

  // Dump data
  for (uint16_t i = 1; i < results->rawlen; i++) {
    Serial.print(results->rawbuf[i] * RAWTICK, DEC);
    if (i < results->rawlen - 1)
      Serial.print(",");  // ',' not needed on last one
    if (!(i & 1)) Serial.print(" ");
  }

  // End declaration
  Serial.print("};");  //

  // Comment
  Serial.print("  // ");
  Serial.print(encoding(results));
  Serial.print(" ");
  serialPrintUint64(results->value, 16);

  // Newline
  Serial.println("");

  // Now dump "known" codes
  if (results->decode_type != UNKNOWN) {
    // Some protocols have an address &/or command.
    // NOTE: It will ignore the atypical case when a message has been decoded
    // but the address & the command are both 0.
    if (results->address > 0 || results->command > 0) {
      Serial.print("uint32_t  address = 0x");
      Serial.print(results->address, HEX);
      Serial.println(";");
      Serial.print("uint32_t  command = 0x");
      Serial.print(results->command, HEX);
      Serial.println(";");
    }

    // All protocols have data
    Serial.print("uint64_t  data = 0x");
    serialPrintUint64(results->value, 16);
    Serial.println(";");
  }
}


//+=============================================================================
// Binary value to hex
//
String bin2hex(const uint8_t* bin, const int length) {
  String hex = "";

  for (int i = 0; i < length; i++) {
    if (bin[i] < 16) {
      hex += "0";
    }
    hex += String(bin[i], HEX);
  }
  return hex;
}

//+=============================================================================
// Send IR codes to variety of sources
//
void irblast(String type, String dataStr, unsigned int len, int rdelay, int pulse, int pdelay, int repeat, long address, IRsend irsend) {
  Serial.println("Blasting off");
  type.toLowerCase();
  uint64_t data = strtoull(("0x" + dataStr).c_str(), 0, 0);
  holdReceive = true;
  Serial.println("Blocking incoming IR signals");
  // Repeat Loop
  for (int r = 0; r < repeat; r++) {
    // Pulse Loop
    for (int p = 0; p < pulse; p++) {
      serialPrintUint64(data, HEX);
      Serial.print(":");
      Serial.print(type);
      Serial.print(":");
      Serial.println(len);
      if (type == "nec") {
        irsend.sendNEC(data, len);
      } else if (type == "sony") {
        irsend.sendSony(data, len);
      } else if (type == "coolix") {
        irsend.sendCOOLIX(data, len);
      } else if (type == "whynter") {
        irsend.sendWhynter(data, len);
      } else if (type == "panasonic") {
        Serial.print("Address: ");
        Serial.println(address);
        irsend.sendPanasonic(address, data);
      } else if (type == "jvc") {
        irsend.sendJVC(data, len, 0);
      } else if (type == "samsung") {
        irsend.sendSAMSUNG(data, len);
      } else if (type == "sharpraw") {
        irsend.sendSharpRaw(data, len);
      } else if (type == "dish") {
        irsend.sendDISH(data, len);
      } else if (type == "rc5") {
        irsend.sendRC5(data, len);
      } else if (type == "rc6") {
        irsend.sendRC6(data, len);
      } else if (type == "denon") {
        irsend.sendDenon(data, len);
      } else if (type == "lg") {
        irsend.sendLG(data, len);
      } else if (type == "sharp") {
        irsend.sendSharpRaw(data, len);
      } else if (type == "rcmm") {
        irsend.sendRCMM(data, len);
      } else if (type == "gree") {
        irsend.sendGree(data, len);
      } else if (type == "roomba") {
        //Not implemented
      }
      if (p + 1 < pulse) delay(pdelay);
    }
    if (r + 1 < repeat) delay(rdelay);
  }
  Serial.println("Transmission complete");
  // Save last sent ir code in lastSentCode variable
  strncpy(lastSentCode.data, dataStr.c_str(), 40);
  lastSentCode.bits = len;
  strncpy(lastSentCode.encoding, type.c_str(), 14);
  strncpy(lastSentCode.address, ("0x" + String(address, HEX)).c_str(), 20);
  lastSentCode.valid = true;
  // Resume ir receiving
  resetReceive();
}

void rawblast(JsonArray &raw, int khz, int rdelay, int pulse, int pdelay, int repeat, IRsend irsend, int duty) {
  Serial.println("Raw transmit");
  holdReceive = true;
  Serial.println("Blocking incoming IR signals");
  // Repeat Loop
  for (int r = 0; r < repeat; r++) {
    // Pulse Loop
    for (int p = 0; p < pulse; p++) {
      Serial.println("Sending code");
      irsend.enableIROut(khz, duty);
      for (unsigned int i = 0; i < raw.size(); i++) {
        int val = raw[i];
        if (i & 1) irsend.space(std::max(0, val));
        else       irsend.mark(val);
      }
      irsend.space(0);
      if (p + 1 < pulse) delay(pdelay);
    }
    if (r + 1 < repeat) delay(rdelay);
  }

  Serial.println("Transmission complete");
  //Copy last sent code and store
  strncpy(lastSentCode.data, "", 40);
  lastSentCode.bits = raw.size();
  strncpy(lastSentCode.encoding, "RAW", 14);
  strncpy(lastSentCode.address, "0x0", 20);
  lastSentCode.valid = true;
  //resume receiving
  resetReceive();
}

void loop() {
  server->handleClient();
  decode_results results;                                        // Somewhere to store the results
  if (irrecv.decode(&results) && !holdReceive) {                  // Grab an IR code
    //TODO: Lot of noise signal received, ignore them dont let them overwrite lastRecvdCode
    Serial.println("Signal received:");
    fullCode(&results);                                           // Print the singleline value
    dumpCode(&results);                                           // Output the results as source code
    cvrtCode(lastRecvdCode, &results);                                // Store the results
    lastRecvdCode.valid = true;
    Serial.println("");                                           // Blank line between entries
    irrecv.resume();                                              // Prepare for the next value
    digitalWrite(ledpin, LOW);                                    // Turn on the LED for 0.5 seconds
    ticker.attach(0.5, disableLed);
  }
  delay(200);
}
