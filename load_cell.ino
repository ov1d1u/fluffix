#include <ESP8266WiFi.h>
#include <FS.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <PubSubClient.h>
#include <HX711.h>
#include <ArduinoJson.h>
#include <SimpleKalmanFilter.h>

String ssid;
String password;
String mqtt_server;
int mqtt_port = 0;

// Calibration data
const float tareValue = 1000000; 
const float maxValue = 1240000;

const long interval = 1000;  // Counter value update interval
unsigned long previousMillis = 0;

int oldValue = 0;
bool isConfigured = false;
bool isEmpty = false;

WiFiClient espClient;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ro.pool.ntp.org", 0, 86400000);  // 86400000ms = 24h
PubSubClient mqttClient;
SimpleKalmanFilter kf(82, 82, 0.01);

HX711 scale(D3, D2);    // parameter "gain" is ommited; the default value 128 is used by the library

void setup() {
  Serial.begin(9600);
  Serial.println("Welcome to Fluffix 0.10!");
  Serial.println("Begin initialization...");
  isConfigured = loadConfiguration();
  if (!isConfigured) return;
  mqttClient.setClient(espClient);
  mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(BUILTIN_LED, HIGH);
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid.c_str(), password.c_str());

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(BUILTIN_LED, LOW);
    delay(500);
    digitalWrite(BUILTIN_LED, HIGH);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  digitalWrite(BUILTIN_LED, HIGH);
  timeClient.begin();
  kf.updateEstimate(tareValue);
}

void loop() {
  if (!isConfigured) return;
  timeClient.update();
  if (!mqttClient.connected()) {
    reconnect();
  }
  mqttClient.loop();
  
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    
    // Read values from sensor
    scale.power_up();
    while (!scale.is_ready()) {
      delay(100);
    }
    float rawValue = scale.read();
    float estValue = kf.updateEstimate(rawValue);
    //Serial.println(estValue);
    int fillValue = (estValue - tareValue) / (maxValue - tareValue) * 100;
    if (rawValue < 1000000.00) {
      // Push some debug info
      mqttClient.publish("sensors/water_bowl_level_raw", String(rawValue).c_str(), true);
      mqttClient.publish("sensors/water_bowl_uptime", String(millis()).c_str());
    }
    if (fillValue < 0) {
      fillValue = 0;
      isEmpty = true;
    } else if (fillValue > 100) {
      fillValue = 100;
    }

    if (isEmpty && fillValue > 90) {
      // The bowl was refilled
      isEmpty = false;
      Serial.println("Publishing last refill timestamp on MQTT...");
      mqttClient.publish("sensors/water_bowl_last_refill", String(timeClient.getEpochTime()).c_str(), true);
    }


    if (fillValue != oldValue) {
      // Value changed, notify broker
      // Send value over MQTT
      Serial.print("Publishing value on MQTT: ");
      Serial.println(fillValue);
      mqttClient.publish("sensors/water_bowl_level", String(fillValue).c_str(), true);
      Serial.println("Done.");
    }

    oldValue = fillValue;
    scale.power_down();
  }
}

bool loadConfiguration() {
  Serial.println("Reading config data...");
  SPIFFS.begin();
  // Do some checks...
  if (!SPIFFS.exists("/config.json")) {
    Serial.println("CONFIGURATION FILE DOES NOT EXIST, ABORTING.");
    SPIFFS.end();
    return false;
  }
  
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("FAILED TO OPEN CONFIGURATION FILE, ABORTING.");
    SPIFFS.end();
    return false;
  }

  // Read file's contents
  size_t size = configFile.size();
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);

  // Parse file
  DynamicJsonDocument doc;
  DeserializationError jsonError = deserializeJson(doc, buf.get());
  if (jsonError) {
    Serial.println("FAILED TO PARSE CONFIGURATION FILE, ABORTING.");
    SPIFFS.end();
    return false;
  }

  // Load config values from files
  JsonObject json = doc.as<JsonObject>();
  ssid = json["wifi_name"].as<String>();
  password = json["wifi_pass"].as<String>();
  mqtt_server = json["mqtt_server"].as<String>();
  mqtt_port = json["mqtt_port"];
  SPIFFS.end();

  return true;
}

void reconnect() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection to "); Serial.print(mqtt_server); Serial.print(":"); Serial.print(mqtt_port); Serial.print("... ");
    // Attempt to connect
    if (mqttClient.connect("WaterBowlClient")) {
      Serial.println("connected");
    } else {
      delay(1000);
    }
  }
}
