// -*- mode: c++; indent-tabs-mode: nil; c-file-style: "stroustrup" -*-
#include <FS.h>                   //this needs to be first, or it all crashes and burns...

// If HOME_ASSISTANT_DISCOVERY is defined, the Anavi Fume Extractor will
// publish MQTT messages that makes Home Assistant auto-discover the
// device.  See https://www.home-assistant.io/docs/mqtt/discovery/.
//
// This requires PubSubClient 2.7.

#define HOME_ASSISTANT_DISCOVERY 1

// If DEBUG is defined additional message will be printed in serial console
#undef DEBUG

// If PUBLISH_CHIP_ID is defined, the Anavi Fume Extractor will publish
// the chip ID using MQTT.  This can be considered a privacy issue,
// and is disabled by default.
#undef PUBLISH_CHIP_ID

// Should Over-the-Air upgrades be supported?  They are only supported
// if this define is set, and the user configures an OTA server on the
// wifi configuration page (or defines OTA_SERVER below).  If you use
// OTA you are strongly adviced to use signed builds (see
// https://arduino-esp8266.readthedocs.io/en/2.5.0/ota_updates/readme.html#advanced-security-signed-updates)
//
// You perform an OTA upgrade by publishing a MQTT command, like
// this:
//
//   mosquitto_pub -h mqtt-server.example.com \
//     -t cmnd/$MACHINEID/update \
//     -m '{"file": "/anavi.bin", "server": "www.example.com", "port": 8080 }'
//
// The port defaults to 80.
#define OTA_UPGRADES 1
// #define OTA_SERVER "www.example.com"

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <ESP8266httpUpdate.h>

//needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#include <PubSubClient.h>        //https://github.com/knolleary/pubsubclient

#include <MD5Builder.h>
// For OLED display
#include <U8g2lib.h>
#include <Wire.h>
#include "Adafruit_HTU21DF.h"
#include "Adafruit_APDS9960.h"
// For BMP180
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP085_U.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

Adafruit_BMP085_Unified bmp = Adafruit_BMP085_Unified(10085);

Adafruit_HTU21DF htu = Adafruit_HTU21DF();

Adafruit_APDS9960 apds;

//Configure supported I2C sensors
const int sensorHTU21D =  0x40;
const int sensorBH1750 = 0x23;
const int sensorBMP180 = 0x77;
const int i2cDisplayAddress = 0x3c;

// Configure pins
const int pinAlarm = 16;
const int pinButton = 0;
const int pinCustom = 2;
const int pinAdc = 0;

#define PIN_FAN         12
#define PIN_FAN_BUTTON  13
#define PIN_WIFI_CONFIG 14

bool fanOn = false;
bool configWiFi = true;

unsigned long sensorPreviousMillis = 0;
const long sensorInterval = 3000;
unsigned long gasPreviousMillis = 0;
const long gasInterval = 1000;

// Last known good values for air quality and
// gas concentration (through air conductivity)
String prevQuality = "";
int prevConductivity = 0;
int prevGas = -1;

unsigned long mqttConnectionPreviousMillis = millis();
const long mqttConnectionInterval = 60000;

bool isTempSensorAttached = false;
float sensorTemperature = 0;
float sensorHumidity = 0;
uint16_t sensorAmbientLight = 0;

//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[40] = "mqtt.eclipse.org";
char mqtt_port[6] = "1883";
char workgroup[32] = "workgroup";
// MQTT username and password
char username[20] = "";
char password[20] = "";
#ifdef HOME_ASSISTANT_DISCOVERY
// Make sure the machineId fits.
char ha_name[32+1] = "";
#endif
#ifdef OTA_UPGRADES
char ota_server[40];
#endif
char temp_scale[40] = "celsius";

// Set the temperature in Celsius or Fahrenheit
// true - Celsius, false - Fahrenheit
bool configTempCelsius = true;

// MD5 of chip ID.  If you only have a handful of Fume Extractors and
// use your own MQTT broker (instead of iot.eclips.org) you may want
// to truncate the MD5 by changing the 32 to a smaller value.
char machineId[32+1] = "";

//flag for saving data
bool shouldSaveConfig = false;

// MQTT
WiFiClient espClient;
PubSubClient mqttClient(espClient);

#ifdef OTA_UPGRADES
char cmnd_update_topic[12 + sizeof(machineId)];
#endif

char line1_topic[11 + sizeof(machineId)];
char line2_topic[11 + sizeof(machineId)];
char line3_topic[11 + sizeof(machineId)];
char cmnd_temp_coefficient_topic[14 + sizeof(machineId)];
char cmnd_temp_format[16 + sizeof(machineId)];
char cmnd_fan[16 + sizeof(machineId)];
char state_fan[22 + sizeof(machineId)];

// The display can fit 26 "i":s on a single line.  It will fit even
// less of other characters.
char mqtt_line1[26+1];
char mqtt_line2[26+1];
char mqtt_line3[26+1];

String sensor_line1;
String sensor_line2;
String sensor_line3;

bool need_redraw = false;

//callback notifying us of the need to save config
void saveConfigCallback ()
{
    Serial.println("Should save config");
    shouldSaveConfig = true;
}

void drawDisplay(const char *line1, const char *line2 = "", const char *line3 = "", bool smallSize = false)
{
    // Write on OLED display
    // Clear the internal memory
    u8g2.clearBuffer();
    // Set appropriate font
    if ( true == smallSize)
    {
      u8g2.setFont(u8g2_font_ncenR10_tr);
      u8g2.drawStr(0,14, line1);
      u8g2.drawStr(0,39, line2);
      u8g2.drawStr(0,60, line3);
    }
    else
    {
      u8g2.setFont(u8g2_font_ncenR14_tr);
      u8g2.drawStr(0,14, line1);
      u8g2.drawStr(0,39, line2);
      u8g2.drawStr(0,64, line3);
    }
    // Transfer internal memory to the display
    u8g2.sendBuffer();
}

void apWiFiCallback(WiFiManager *myWiFiManager)
{
    String configPortalSSID = myWiFiManager->getConfigPortalSSID();
    // Print information in the serial output
    Serial.print("Created access point for configuration: ");
    Serial.println(configPortalSSID);
    // Show information on the display
    String apId = configPortalSSID.substring(configPortalSSID.length()-5);
    String configHelper("AP ID: "+apId);
    drawDisplay("Fume Extractor", "Please configure", configHelper.c_str(), true);
}

void saveConfig()
{
    Serial.println("Saving configurations to file.");
    DynamicJsonDocument json(1024);
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["workgroup"] = workgroup;
    json["username"] = username;
    json["password"] = password;
    json["temp_scale"] = temp_scale;
#ifdef HOME_ASSISTANT_DISCOVERY
    json["ha_name"] = ha_name;
#endif
#ifdef OTA_UPGRADES
    json["ota_server"] = ota_server;
#endif

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile)
    {
        Serial.println("ERROR: failed to open config file for writing");
        return;
    }

    serializeJson(json, Serial);
    serializeJson(json, configFile);
    configFile.close();
}

void checkDisplay()
{
    Serial.print("Mini I2C OLED Display at address ");
    Serial.print(i2cDisplayAddress, HEX);
    if (isSensorAvailable(i2cDisplayAddress))
    {
        Serial.println(": OK");
    }
    else
    {
        Serial.println(": N/A");
    }
}

void fanTurnOn()
{
    digitalWrite(PIN_FAN, HIGH);
    fanOn = true;
    sensor_line3 = "Fan: ON";
    Serial.println("Fan: ON");
}

void fanTurnOff()
{
    digitalWrite(PIN_FAN, LOW);
    fanOn = false;
    sensor_line3 = "Fan: OFF";
    Serial.println("Fan: OFF");
}

void initFan()
{
    Serial.println("Turning on the fan...");
    pinMode(PIN_FAN_BUTTON, INPUT);
    pinMode(PIN_WIFI_CONFIG, INPUT);
    fanTurnOn();
    if (true == configWiFi)
    {
      publishFanState(true);
    }
}

void setup()
{
    // put your setup code here, to run once:
    strcpy(mqtt_line1, "");
    strcpy(mqtt_line2, "");
    strcpy(mqtt_line3, "");
    need_redraw = true;
    Serial.begin(115200);
    Serial.println();

    Wire.begin();
    checkDisplay();

    u8g2.begin();

    delay(10);

    // LED
    pinMode(pinAlarm, OUTPUT);
    // Button
    pinMode(pinButton, INPUT);
    // Custom GPIO
    pinMode(pinCustom, OUTPUT);
    // Fan
    pinMode(PIN_FAN, OUTPUT);

    waitForFactoryReset();

    // Machine ID
    calculateMachineId();

    //read configuration from FS json
    Serial.println("mounting FS...");

    if (SPIFFS.begin())
    {
        Serial.println("mounted file system");
        if (SPIFFS.exists("/config.json")) {
            //file exists, reading and loading
            Serial.println("reading config file");
            File configFile = SPIFFS.open("/config.json", "r");
            if (configFile)
            {
                Serial.println("opened config file");
                const size_t size = configFile.size();
                // Allocate a buffer to store contents of the file.
                std::unique_ptr<char[]> buf(new char[size]);

                configFile.readBytes(buf.get(), size);
                DynamicJsonDocument json(1024);
                if (DeserializationError::Ok == deserializeJson(json, buf.get()))
                {
#ifdef DEBUG
                    // Content stored in the memory of the microcontroller contains
                    // sensitive data such as username and passwords therefore
                    // should be printed only during debugging
                    serializeJson(json, Serial);
                    Serial.println("\nparsed json");
#endif

                    strcpy(mqtt_server, json["mqtt_server"]);
                    strcpy(mqtt_port, json["mqtt_port"]);
                    strcpy(workgroup, json["workgroup"]);
                    strcpy(username, json["username"]);
                    strcpy(password, json["password"]);
                    {
                        const char *s = json["temp_scale"];
                        if (!s)
                            s = "celsius";
                        strcpy(temp_scale, s);
                    }
#ifdef HOME_ASSISTANT_DISCOVERY
                    {
                        const char *s = json["ha_name"];
                        if (!s)
                            s = machineId;
                        snprintf(ha_name, sizeof(ha_name), "%s", s);
                    }
#endif
#ifdef OTA_UPGRADES
                    {
                        const char *s = json["ota_server"];
                        if (!s)
                            s = ""; // The empty string never matches.
                        snprintf(ota_server, sizeof(ota_server), "%s", s);
                    }
#endif
                }
                else
                {
                    Serial.println("failed to load json config");
                }
            }
        }
    }
    else
    {
        Serial.println("failed to mount FS");
    }
    //end read

    // Set MQTT topics
    sprintf(line1_topic, "cmnd/%s/line1", machineId);
    sprintf(line2_topic, "cmnd/%s/line2", machineId);
    sprintf(line3_topic, "cmnd/%s/line3", machineId);
    sprintf(cmnd_temp_coefficient_topic, "cmnd/%s/tempcoef", machineId);
    sprintf(cmnd_temp_format, "cmnd/%s/tempformat", machineId);
    sprintf(cmnd_fan,"%s/%s/fan", workgroup, machineId);
    sprintf(state_fan,"%s/%s/fan/state", workgroup, machineId);
#ifdef OTA_UPGRADES
    sprintf(cmnd_update_topic, "cmnd/%s/update", machineId);
#endif

    Serial.print("WiFi: ");
    if (LOW == digitalRead(PIN_WIFI_CONFIG))
    {
        configWiFi = false;
        Serial.println("OFF");
    }
    else
    {
        configWiFi = true;
        Serial.println("ON");

        // The extra parameters to be configured (can be either global or just in the setup)
        // After connecting, parameter.getValue() will get you the configured value
        // id/name placeholder/prompt default length
        WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, sizeof(mqtt_server));
        WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, sizeof(mqtt_port));
        WiFiManagerParameter custom_workgroup("workgroup", "workgroup", workgroup, sizeof(workgroup));
        WiFiManagerParameter custom_mqtt_user("user", "MQTT username", username, sizeof(username));
        WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", password, sizeof(password));
#ifdef HOME_ASSISTANT_DISCOVERY
        WiFiManagerParameter custom_mqtt_ha_name("ha_name", "Sensor name for Home Assistant", ha_name, sizeof(ha_name));
#endif
#ifdef OTA_UPGRADES
        WiFiManagerParameter custom_ota_server("ota_server", "OTA server", ota_server, sizeof(ota_server));
#endif
        WiFiManagerParameter custom_temperature_scale("temp_scale", "Temperature scale", temp_scale, sizeof(temp_scale));
    
        char htmlMachineId[200];
        sprintf(htmlMachineId,"<p style=\"color: red;\">Machine ID:</p><p><b>%s</b></p><p>Copy and save the machine ID because you will need it to control the device.</p>", machineId);
        WiFiManagerParameter custom_text_machine_id(htmlMachineId);
    
        //WiFiManager
        //Local intialization. Once its business is done, there is no need to keep it around
        WiFiManager wifiManager;
    
        //set config save notify callback
        wifiManager.setSaveConfigCallback(saveConfigCallback);
    
        //add all your parameters here
        wifiManager.addParameter(&custom_mqtt_server);
        wifiManager.addParameter(&custom_mqtt_port);
        wifiManager.addParameter(&custom_workgroup);
        wifiManager.addParameter(&custom_mqtt_user);
        wifiManager.addParameter(&custom_mqtt_pass);
        wifiManager.addParameter(&custom_temperature_scale);
#ifdef HOME_ASSISTANT_DISCOVERY
        wifiManager.addParameter(&custom_mqtt_ha_name);
#endif
#ifdef OTA_UPGRADES
        wifiManager.addParameter(&custom_ota_server);
#endif
        wifiManager.addParameter(&custom_text_machine_id);
    
        //reset settings - for testing
        //wifiManager.resetSettings();
    
        //set minimu quality of signal so it ignores AP's under that quality
        //defaults to 8%
        //wifiManager.setMinimumSignalQuality();
    
        //sets timeout until configuration portal gets turned off
        //useful to make it all retry or go to sleep
        //in seconds
        wifiManager.setTimeout(300);
    
        digitalWrite(pinAlarm, HIGH);
        drawDisplay("Fume Extractor", "Connecting...", WiFi.SSID().c_str());
    
        //fetches ssid and pass and tries to connect
        //if it does not connect it starts an access point
        //and goes into a blocking loop awaiting configuration
        wifiManager.setAPCallback(apWiFiCallback);
        // Append the last 5 character of the machine id to the access point name
        String apId(machineId);
        apId = apId.substring(apId.length() - 5);
        String accessPointName = "ANAVI Fume Extractor " + apId;
        if (!wifiManager.autoConnect(accessPointName.c_str(), ""))
        {
            digitalWrite(pinAlarm, LOW);
            Serial.println("failed to connect and hit timeout");
            delay(3000);
            //reset and try again, or maybe put it to deep sleep
            ESP.reset();
            delay(5000);
        }
    
        //if you get here you have connected to the WiFi
        Serial.println("connected...yeey :)");
        digitalWrite(pinAlarm, LOW);
    
        //read updated parameters
        strcpy(mqtt_server, custom_mqtt_server.getValue());
        strcpy(mqtt_port, custom_mqtt_port.getValue());
        strcpy(workgroup, custom_workgroup.getValue());
        strcpy(username, custom_mqtt_user.getValue());
        strcpy(password, custom_mqtt_pass.getValue());
        strcpy(temp_scale, custom_temperature_scale.getValue());
#ifdef HOME_ASSISTANT_DISCOVERY
        strcpy(ha_name, custom_mqtt_ha_name.getValue());
#endif
#ifdef OTA_UPGRADES
        strcpy(ota_server, custom_ota_server.getValue());
#endif
    
        //save the custom parameters to FS
        if (shouldSaveConfig)
        {
            saveConfig();
        }
    
        Serial.println("local ip");
        Serial.println(WiFi.localIP());
        drawDisplay("Connected!", "Local IP:", WiFi.localIP().toString().c_str());
        delay(2000);
        
        // MQTT
        Serial.print("MQTT Server: ");
        Serial.println(mqtt_server);
        Serial.print("MQTT Port: ");
        Serial.println(mqtt_port);
        // Print MQTT Username
        Serial.print("MQTT Username: ");
        Serial.println(username);
        // Hide password from the log and show * instead
        char hiddenpass[20] = "";
        for (size_t charP=0; charP < strlen(password); charP++)
        {
            hiddenpass[charP] = '*';
        }
        hiddenpass[strlen(password)] = '\0';
        Serial.print("MQTT Password: ");
        Serial.println(hiddenpass);
        Serial.print("Saved temperature scale: ");
        Serial.println(temp_scale);
        configTempCelsius = ( (0 == strlen(temp_scale)) || String(temp_scale).equalsIgnoreCase("celsius"));
        Serial.print("Temperature scale: ");
        if (true == configTempCelsius)
        {
          Serial.println("Celsius");
        }
        else
        {
          Serial.println("Fahrenheit");
        }
    #ifdef HOME_ASSISTANT_DISCOVERY
        Serial.print("Home Assistant sensor name: ");
        Serial.println(ha_name);
    #endif
    #ifdef OTA_UPGRADES
        if (ota_server[0] != '\0')
        {
            Serial.print("OTA server: ");
            Serial.println(ota_server);
        }
        else
        {
    #  ifndef OTA_SERVER
            Serial.println("No OTA server");
    #  endif
        }
    
    #  ifdef OTA_SERVER
        Serial.print("Hardcoded OTA server: ");
        Serial.println(OTA_SERVER);
    #  endif
    
    #endif
    
        const int mqttPort = atoi(mqtt_port);
        mqttClient.setServer(mqtt_server, mqttPort);
        mqttClient.setCallback(mqttCallback);
    
        mqttReconnect(true);

    }

    // Sensors
    htu.begin();
    bmp.begin();
    
    Serial.println("");
    Serial.println("-----");
    Serial.print("Machine ID: ");
    Serial.println(machineId);
    Serial.println("-----");
    Serial.println("");

    setupADPS9960();

    initFan();
}

void setupADPS9960()
{
    if(apds.begin())
    {
        //gesture mode will be entered once proximity mode senses something close
        apds.enableProximity(true);
        apds.enableGesture(true);
    }
}

void waitForFactoryReset()
{
    Serial.println("Press button within 2 seconds for factory reset...");
    for (int iter = 0; iter < 20; iter++)
    {
        digitalWrite(pinAlarm, HIGH);
        delay(50);
        if (false == digitalRead(pinButton))
        {
            factoryReset();
            return;
        }
        digitalWrite(pinAlarm, LOW);
        delay(50);
        if (false == digitalRead(pinButton))
        {
            factoryReset();
            return;
        }
    }
}

void factoryReset()
{
    if (false == digitalRead(pinButton))
    {
        Serial.println("Hold the button to reset to factory defaults...");
        bool cancel = false;
        for (int iter=0; iter<30; iter++)
        {
            digitalWrite(pinAlarm, HIGH);
            delay(100);
            if (true == digitalRead(pinButton))
            {
                cancel = true;
                break;
            }
            digitalWrite(pinAlarm, LOW);
            delay(100);
            if (true == digitalRead(pinButton))
            {
                cancel = true;
                break;
            }
        }
        if (false == digitalRead(pinButton) && !cancel)
        {
            digitalWrite(pinAlarm, HIGH);
            Serial.println("Disconnecting...");
            WiFi.disconnect();

            // NOTE: the boot mode:(1,7) problem is known and only happens at the first restart after serial flashing.

            Serial.println("Restarting...");
            // Clean the file system with configurations
            SPIFFS.format();
            // Restart the board
            ESP.restart();
        }
        else
        {
            // Cancel reset to factory defaults
            Serial.println("Reset to factory defaults cancelled.");
            digitalWrite(pinAlarm, LOW);
        }
    }
}

#ifdef OTA_UPGRADES
void do_ota_upgrade(char *text)
{
    DynamicJsonDocument json(1024);
    auto error = deserializeJson(json, text);
    if (error)
    {
        Serial.println("No success decoding JSON.\n");
    }
    else if (!json.containsKey("server"))
    {
        Serial.println("JSON is missing server\n");
    }
    else if (!json.containsKey("file"))
    {
        Serial.println("JSON is missing file\n");
    }
    else
    {
        int port = 0;
        if (json.containsKey("port"))
        {
            port = json["port"];
            Serial.print("Port configured to ");
            Serial.println(port);
        }

        if (0 >= port || 65535 < port)
        {
            port = 80;
        }

        String server = json["server"];
        String file = json["file"];

        bool ok = false;
        if (ota_server[0] != '\0' && !strcmp(server.c_str(), ota_server))
            ok = true;

#  ifdef OTA_SERVER
        if (!strcmp(server.c_str(), OTA_SERVER))
            ok = true;
#  endif

        if (!ok)
        {
            Serial.println("Wrong OTA server. Refusing to upgrade.");
            return;
        }

        Serial.print("Attempting to upgrade from ");
        Serial.print(server);
        Serial.print(":");
        Serial.print(port);
        Serial.println(file);
        ESPhttpUpdate.setLedPin(pinAlarm, HIGH);
        WiFiClient update_client;
        t_httpUpdate_return ret = ESPhttpUpdate.update(update_client,
                                                       server, port, file);
        switch (ret)
        {
        case HTTP_UPDATE_FAILED:
            Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
            break;

        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("HTTP_UPDATE_NO_UPDATES");
            break;

        case HTTP_UPDATE_OK:
            Serial.println("HTTP_UPDATE_OK");
            break;
        }
    }
}
#endif

void processMessageFan(const char* text)
{
    StaticJsonDocument<100> data;
    deserializeJson(data, text);
    // Set temperature to Celsius or Fahrenheit and redraw screen
    Serial.print("Changing the temperature scale to: ");
    if (true == data.containsKey("fan"))
    {
        if (true == data["fan"])
        {
            fanTurnOn();
        }
        else
        {
            fanTurnOff();
        }
        need_redraw = true;

        // publish state topic
        publishFanState(data["fan"]);
    }
}

void processMessageScale(const char* text)
{
    StaticJsonDocument<200> data;
    deserializeJson(data, text);
    // Set temperature to Celsius or Fahrenheit and redraw screen
    Serial.print("Changing the temperature scale to: ");
    if (data.containsKey("scale") && (0 == strcmp(data["scale"], "celsius")) )
    {
        Serial.println("Celsius");
        configTempCelsius = true;
        strcpy(temp_scale, "celsius");
    }
    else
    {
        Serial.println("Fahrenheit");
        configTempCelsius = false;
        strcpy(temp_scale, "fahrenheit");
    }
    need_redraw = true;
    // Save configurations to file
    saveConfig();
}

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    // Convert received bytes to a string
    char text[length + 1];
    snprintf(text, length + 1, "%s", payload);

    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    Serial.println(text);

    if (strcmp(topic, cmnd_fan) == 0)
    {
        processMessageFan(text);
    }

    if (strcmp(topic, line1_topic) == 0)
    {
        snprintf(mqtt_line1, sizeof(mqtt_line1), "%s", text);
        need_redraw = true;
    }

    if (strcmp(topic, line2_topic) == 0)
    {
        snprintf(mqtt_line2, sizeof(mqtt_line2), "%s", text);
        need_redraw = true;
    }

    if (strcmp(topic, line3_topic) == 0)
    {
        snprintf(mqtt_line3, sizeof(mqtt_line3), "%s", text);
        need_redraw = true;
    }

    if (strcmp(topic, cmnd_temp_format) == 0)
    {
        processMessageScale(text);
    }

#ifdef OTA_UPGRADES
    if (strcmp(topic, cmnd_update_topic) == 0)
    {
        Serial.println("OTA request seen.\n");
        do_ota_upgrade(text);
        // Any OTA upgrade will stop the mqtt client, so if the
        // upgrade failed and we get here publishState() will fail.
        // Just return here, and we will reconnect from within the
        // loop().
        return;
    }
#endif

    publishState();
}

void calculateMachineId()
{
    MD5Builder md5;
    md5.begin();
    char chipId[25];
    sprintf(chipId,"%d",ESP.getChipId());
    md5.add(chipId);
    md5.calculate();
    md5.toString().toCharArray(machineId, sizeof(machineId));
}

void mqttReconnect(bool isFirstConnect)
{
    char clientId[18 + sizeof(machineId)];
    snprintf(clientId, sizeof(clientId), "anavi-gas-detector-%s", machineId);

    // Loop until we're reconnected
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        if (true == mqttClient.connect(clientId, username, password))
        {
            Serial.println("connected");

            if (true == isFirstConnect)
            {
                fanTurnOn();
                publishFanState(true);
            }

            // Subscribe to MQTT topics
            mqttClient.subscribe(line1_topic);
            mqttClient.subscribe(line2_topic);
            mqttClient.subscribe(line3_topic);
            mqttClient.subscribe(cmnd_temp_coefficient_topic);
            mqttClient.subscribe(cmnd_temp_format);
            mqttClient.subscribe(cmnd_fan);
#ifdef OTA_UPGRADES
            mqttClient.subscribe(cmnd_update_topic);
#endif
            publishState();
            break;

        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
        }
    }
}

#ifdef HOME_ASSISTANT_DISCOVERY
bool publishSensorDiscovery(const char *config_key,
                            const char *device_class,
                            const char *name_suffix,
                            const char *state_topic,
                            const char *unit,
                            const char *value_template,
                            bool binary = false)
{
    DynamicJsonDocument json(1024);

    String sensorType = "sensor";
    if (true == binary)
    {
        sensorType = "binary_sensor";
    }
    else
    {
        // Unit of measurement is supported only by non-binary sensors
        json["unit_of_measurement"] = unit;
        json["value_template"] = value_template;
    }
    static char topic[48 + sizeof(machineId)];
    snprintf(topic, sizeof(topic),
             "homeassistant/%s/%s/%s/config", sensorType.c_str(), machineId, config_key);

    if (0 < strlen(device_class))
    {
      json["device_class"] = device_class;
    }
    json["name"] = String(ha_name) + " " + name_suffix;
    json["unique_id"] = String("anavi-") + machineId + "-" + config_key;
    json["state_topic"] = String(workgroup) + "/" + machineId + "/" + state_topic;

    json["device"]["identifiers"] = machineId;
    json["device"]["manufacturer"] = "ANAVI Technology";
    json["device"]["model"] = "ANAVI Fume Extractor";
    json["device"]["name"] = ha_name;
    json["device"]["sw_version"] = ESP.getSketchMD5();

    JsonArray connections = json["device"].createNestedArray("connections").createNestedArray();
    connections.add("mac");
    connections.add(WiFi.macAddress());

    Serial.print("Home Assistant discovery topic: ");
    Serial.println(topic);

    int payload_len = measureJson(json);
    if (!mqttClient.beginPublish(topic, payload_len, true))
    {
        Serial.println("beginPublish failed!\n");
        return false;
    }

    if (serializeJson(json, mqttClient) != payload_len)
    {
        Serial.println("writing payload: wrong size!\n");
        return false;
    }

    if (!mqttClient.endPublish())
    {
        Serial.println("endPublish failed!\n");
        return false;
    }

    return true;
}

boolean publishFanDiscovery()
{
    static char topic[36 + sizeof(machineId)];
    snprintf(topic, sizeof(topic),
             "homeassistant/switch/%s/fan/config", machineId);

    DynamicJsonDocument json(1024);
    String componentName = String(ha_name);
    if (0 < componentName.length())
    {
      componentName += " ";
    }
    componentName += String("ANAVI Fume Extractor");
    json["name"] = componentName;
    json["unique_id"] = String("anavi-") + machineId + String("-fan");
    json["state_topic"] = String(workgroup) + "/" + machineId + String("/fan/state");
    json["command_topic"] = String(workgroup) + "/" + machineId + String("/fan");
    json["payload_on"] = String("{\"fan\":true}");
    json["payload_off"] = String("{\"fan\":false}");

    json["device"]["identifiers"] = machineId;
    json["device"]["manufacturer"] = "ANAVI Technology";
    json["device"]["model"] = "ANAVI Fume Extractor";
    json["device"]["name"] = ha_name;
    json["device"]["sw_version"] = ESP.getSketchMD5();

    JsonArray connections = json["device"].createNestedArray("connections").createNestedArray();
    connections.add("mac");
    connections.add(WiFi.macAddress());

    Serial.print("Home Assistant discovery topic: ");
    Serial.println(topic);

    int payload_len = measureJson(json);
    if (!mqttClient.beginPublish(topic, payload_len, true))
    {
        Serial.println("beginPublish failed!\n");
        return false;
    }

    if (serializeJson(json, mqttClient) != payload_len)
    {
        Serial.println("writing payload: wrong size!\n");
        return false;
    }

    if (!mqttClient.endPublish())
    {
        Serial.println("endPublish failed!\n");
        return false;
    }

    return true;
}
#endif

void publishState()
{
    static char payload[300];
    static char topic[80];

#ifdef HOME_ASSISTANT_DISCOVERY

    String homeAssistantTempScale = (true == configTempCelsius) ? "°C" : "°F";

    publishFanDiscovery();

    publishSensorDiscovery("DangerousGas",
                           "gas",
                           "Dangerous Gas",
                           "DangerousGas",
                           "",
                           "",
                           true);

    publishSensorDiscovery("AirConductivity",
                           "",
                           "Air Conductivity",
                           "AirConductivity",
                           "%",
                           "{{ value_json.Conductivity | round(2) }}");


    if (isSensorAvailable(sensorHTU21D))
    {
        publishSensorDiscovery("temp",
                               "temperature",
                               "Temperature",
                               "temperature",
                               homeAssistantTempScale.c_str(),
                               "{{ value_json.temperature | round(1) }}");

        publishSensorDiscovery("humidity",
                               "humidity",
                               "Humidity",
                               "humidity",
                               "%",
                               "{{ value_json.humidity | round(0) }}");
    }

    if (isSensorAvailable(sensorBMP180))
    {
        publishSensorDiscovery("BMPtemperature",
                       "temperature",
                       "BMP Temperature",
                       "BMPtemperature",
                       homeAssistantTempScale.c_str(),
                       "{{ value_json.BMPtemperature | round(1) }}");

        publishSensorDiscovery("BMPpressure",
               "pressure",
               "Pressure",
               "BMPpressure",
               "hPa",
               "{{ value_json.BMPpressure | round(0) }}");

        publishSensorDiscovery("BMPaltitude",
               "",
               "Altitude",
               "BMPaltitude",
               "m",
               "{{ value_json.BMPaltitude | round(0) }}");
    }

    if (isSensorAvailable(sensorBH1750))
    {
        publishSensorDiscovery("light",
                       "illuminance",
                       "Light",
                       "light",
                       "Lux",
                       "{{ value_json.light }}");
    }

#endif
}

void publishSensorData(const char* subTopic, const char* key, const float value)
{
    StaticJsonDocument<100> json;
    json[key] = value;
    char payload[100];
    serializeJson(json, payload);
    char topic[200];
    sprintf(topic,"%s/%s/%s", workgroup, machineId, subTopic);
    mqttClient.publish(topic, payload, true);
}

void publishSensorData(const char* subTopic, const char* key, const String& value)
{
    StaticJsonDocument<100> json;
    json[key] = value;
    char payload[100];
    serializeJson(json, payload);
    char topic[200];
    sprintf(topic,"%s/%s/%s", workgroup, machineId, subTopic);
    mqttClient.publish(topic, payload, true);
}

void publishSensorDataPlain(const char* subTopic, const String& payload)
{
    char topic[200];
    sprintf(topic,"%s/%s/%s", workgroup, machineId, subTopic);
    mqttClient.publish(topic, payload.c_str(), true);
}

void publishFanState(bool isFanOn)
{
    StaticJsonDocument<100> json;
    json["fan"] = isFanOn;
    char payload[100];
    serializeJson(json, payload);
    mqttClient.publish(state_fan, payload, true);
}

bool isSensorAvailable(int sensorAddress)
{
    // Check if I2C sensor is present
    Wire.beginTransmission(sensorAddress);
    return 0 == Wire.endTransmission();
}

void handleHTU21D()
{
    // Check if temperature has changed
    const float tempTemperature = htu.readTemperature();
    if (0.3 <= abs(tempTemperature - sensorTemperature))
    {
        // Print new temprature value
        sensorTemperature = tempTemperature;
        Serial.print("Temperature: ");
        Serial.println(formatTemperature(sensorTemperature));

        // Publish new temperature value through MQTT
        publishSensorData("temperature", "temperature", convertTemperature(sensorTemperature));

        // Temperature and humidity are shown on the display
        // so the text has to be refreshed
        need_redraw = true;
    }

    // Check if humidity has changed
    const float tempHumidity = htu.readHumidity();
    if (1 <= abs(tempHumidity - sensorHumidity))
    {
        // Print new humidity value
        sensorHumidity = tempHumidity;
        Serial.print("Humidity: ");
        Serial.print(sensorHumidity);
        Serial.println("%");

        // Publish new humidity value through MQTT
        publishSensorData("humidity", "humidity", sensorHumidity);

        // Temperature and humidity are shown on the display
        // so the text has to be refreshed
        need_redraw = true;
    }
}

void sensorWriteData(int i2cAddress, uint8_t data)
{
    Wire.beginTransmission(i2cAddress);
    Wire.write(data);
    Wire.endTransmission();
}

void handleBH1750()
{
    //Wire.begin();
    // Power on sensor
    sensorWriteData(sensorBH1750, 0x01);
    // Set mode continuously high resolution mode
    sensorWriteData(sensorBH1750, 0x10);

    uint16_t tempAmbientLight;

    Wire.requestFrom(sensorBH1750, 2);
    tempAmbientLight = Wire.read();
    tempAmbientLight <<= 8;
    tempAmbientLight |= Wire.read();
    // s. page 7 of datasheet for calculation
    tempAmbientLight = tempAmbientLight/1.2;

    if (1 <= abs(tempAmbientLight - sensorAmbientLight))
    {
        // Print new brightness value
        sensorAmbientLight = tempAmbientLight;
        Serial.print("Light: ");
        Serial.print(tempAmbientLight);
        Serial.println("Lux");

        // Publish new brightness value through MQTT
        publishSensorData("light", "light", sensorAmbientLight);
    }
}

void detectGesture()
{
    //read a gesture from the device
    const uint8_t gestureCode = apds.readGesture();
    // Skip if gesture has not been detected
    if (0 == gestureCode)
    {
        return;
    }
    String gesture = "";
    switch(gestureCode)
    {
    case APDS9960_DOWN:
        gesture = "down";
        break;
    case APDS9960_UP:
        gesture = "up";
        break;
    case APDS9960_LEFT:
        gesture = "left";
        break;
    case APDS9960_RIGHT:
        gesture = "right";
        break;
    }
    Serial.print("Gesture: ");
    Serial.println(gesture);
    // Publish the detected gesture through MQTT
    publishSensorData("gesture", "gesture", gesture);
}

void handleBMP()
{
  sensors_event_t event;
  bmp.getEvent(&event);
  if (!event.pressure)
  {
    // BMP180 sensor error
    return;
  }
  Serial.print("BMP180 Pressure: ");
  Serial.print(event.pressure);
  Serial.println(" hPa");
  float temperature;
  bmp.getTemperature(&temperature);
  Serial.print("BMP180 Temperature: ");
  Serial.println(formatTemperature(temperature));
  // For accurate results replace SENSORS_PRESSURE_SEALEVELHPA with the current SLP
  float seaLevelPressure = SENSORS_PRESSURE_SEALEVELHPA;
  float altitude;
  altitude = bmp.pressureToAltitude(seaLevelPressure, event.pressure, temperature);
  Serial.print("BMP180 Altitude: ");
  Serial.print(altitude);
  Serial.println(" m");

  // Publish new pressure values through MQTT
  publishSensorData("BMPpressure", "BMPpressure", event.pressure);
  publishSensorData("BMPtemperature", "BMPtemperature", convertTemperature(temperature));
  publishSensorData("BMPaltitude", "BMPaltitude", altitude);
}

void handleSensors()
{
    isTempSensorAttached = false;
    if (isSensorAvailable(sensorHTU21D))
    {
        isTempSensorAttached = true;
        handleHTU21D();
    }
    if (isSensorAvailable(sensorBH1750))
    {
        handleBH1750();
    }
    if (isSensorAvailable(sensorBMP180))
    {
      handleBMP();
    }
}

void detectGas()
{
  int gas = analogRead(pinAdc);
  // Calculate conductivity in pecents
  // The gas concetration depends on the coductivity
  // If the analog MQ sensor detects more gases
  // the conductivity will be higher
  int conductivity = round(((float)gas/1023)*100);

  String quality = "Good";
  String gasState = "off";
  if (gas <= 190)
  {
    gasState = "OFF";
  }
  else if (gas <= 300)
  {
    quality="Moderate";
    gasState = "OFF";
  }
  else
  {
    quality="Poor";
    gasState = "ON";
  }

  // Do not print or draw on the display if there is no
  // change of the state from the MQ gas sensor
  // or if the change of the value for ADC is minimal
  if ( (5 > abs(gas - prevGas)) || ((prevConductivity == conductivity) && (prevQuality == quality)) )
  {
    return;
  }

  // Update the detected gas values
  prevConductivity = conductivity;
  prevQuality = quality;
  prevGas = gas;

  sensor_line2 = "Quality: " + quality;

  // Publish new pressure values through MQTT
  publishSensorData("AirQuality", "Quality", quality);
  publishSensorDataPlain("DangerousGas", gasState);
  publishSensorData("AirConductivity", "Conductivity", conductivity);

  // Print values in the serial output
  Serial.print("Gas value: ");
  Serial.println(gas);
  Serial.println(sensor_line1);
  Serial.println(sensor_line2);
  Serial.println(sensor_line3);

  need_redraw = true;
}

float convertCelsiusToFahrenheit(float temperature)
{
    return (temperature * 9/5 + 32);
}

float convertTemperature(float temperature)
{
    return (true == configTempCelsius) ? temperature : convertCelsiusToFahrenheit(temperature);
}

String formatTemperature(float temperature)
{
    String unit = (true == configTempCelsius) ? "°C" : "°F";
    return String(convertTemperature(temperature), 1) + unit;
}

void loop()
{
    // put your main code here, to run repeatedly:
    mqttClient.loop();

    // Reconnect if there is an issue with the MQTT connection
    const unsigned long mqttConnectionMillis = millis();
    if ( (true == configWiFi) && (false == mqttClient.connected()) &&
          (mqttConnectionInterval <= (mqttConnectionMillis - mqttConnectionPreviousMillis)) )
    {
        mqttConnectionPreviousMillis = mqttConnectionMillis;
        mqttReconnect(false);
    }
    
    if (LOW == digitalRead(PIN_FAN_BUTTON))
    {
        if (false == fanOn)
        {
            fanTurnOn();
            publishFanState(true);
        }
        else
        {
            fanTurnOff();
            publishFanState(false);
        }
        need_redraw = true;
        // Avoid accidentally double clicking the button
        delay(500);
    }
  
    // Handle gestures at a shorter interval
    if (isSensorAvailable(APDS9960_ADDRESS))
    {
        detectGesture();
    }

    const unsigned long currentMillis = millis();
    // First check the I2C sensors and WiFi signal
    if (sensorInterval <= (currentMillis - sensorPreviousMillis))
    {
        sensorPreviousMillis = currentMillis;
        handleSensors();

        long rssiValue = WiFi.RSSI();
        String rssi = String(rssiValue) + " dBm";
        Serial.println(rssi);
        
        publishSensorData("wifi/ssid", "ssid", WiFi.SSID());
        publishSensorData("wifi/bssid", "bssid", WiFi.BSSIDstr());
        publishSensorData("wifi/rssi", "rssi", rssiValue);
        publishSensorData("wifi/ip", "ip", WiFi.localIP().toString());
        publishSensorData("sketch", "sketch", ESP.getSketchMD5());

#ifdef PUBLISH_CHIP_ID
        char chipid[9];
        snprintf(chipid, sizeof(chipid), "%08x", ESP.getChipId());
        publishSensorData("chipid", "chipid", chipid);
#endif
    }
    // Update the values from the analog MQ gas sensor
    if (gasInterval <= (currentMillis - sensorPreviousMillis))
    {
        gasPreviousMillis = currentMillis;
        detectGas();
    }

    if (true == need_redraw)
    {
        sensor_line1 = "Air ";
        if (true == isTempSensorAttached)
        {
          sensor_line1 += formatTemperature(sensorTemperature) + " ";
          sensor_line1 += (int)round(sensorHumidity);
          sensor_line1 += "%";
        }
        drawDisplay(mqtt_line1[0] ? mqtt_line1 : sensor_line1.c_str(),
                    mqtt_line2[0] ? mqtt_line2 : sensor_line2.c_str(),
                    mqtt_line3[0] ? mqtt_line3 : sensor_line3.c_str(), false);
        need_redraw = false;
    }

    // Press and hold the button to reset to factory defaults
    factoryReset();
}
