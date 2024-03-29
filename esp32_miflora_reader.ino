/**
   A BLE client for the Xiaomi Mi Plant Sensor, pushing measurements to an MQTT server.
*/
#include <Arduino.h>
#include "BLEDevice.h"
#include <WiFi.h>
#include "time.h"
#include <ESP_Google_Sheet_Client.h>
#include "config.h"

// boot count used to check if battery status should be read
RTC_DATA_ATTR int bootCount = 0;

// device count
static int deviceCount = ArrayCount(FLORA_DEVICES);

// the remote service we wish to connect to
static BLEUUID serviceUUID("00001204-0000-1000-8000-00805f9b34fb");

// the characteristic of the remote service we are interested in
static BLEUUID uuid_version_battery("00001a02-0000-1000-8000-00805f9b34fb");
static BLEUUID uuid_sensor_data("00001a01-0000-1000-8000-00805f9b34fb");
static BLEUUID uuid_write_mode("00001a00-0000-1000-8000-00805f9b34fb");

// ESP32 MAC address
char macAddr[18];

TaskHandle_t hibernateTaskHandle = NULL;

WiFiClient espClient;

void connectWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("");

  byte ar[6];
  WiFi.macAddress(ar);
  sprintf(macAddr, "%02X:%02X:%02X:%02X:%02X:%02X", ar[0], ar[1], ar[2], ar[3], ar[4], ar[5]);
}

void disconnectWifi() {
  WiFi.disconnect(true);
  Serial.println("WiFi disonnected");
}

BLEClient* getFloraClient(BLEAddress floraAddress) {
  BLEClient* floraClient = BLEDevice::createClient();

  if (!floraClient->connect(floraAddress)) {
    Serial.println("- Connection failed, skipping");
    return nullptr;
  }

  Serial.println("- Connection successful");
  return floraClient;
}

BLERemoteService* getFloraService(BLEClient* floraClient) {
  BLERemoteService* floraService = nullptr;

  try {
    floraService = floraClient->getService(serviceUUID);
  } catch (...) {
    // something went wrong
  }
  if (floraService == nullptr) {
    Serial.println("- Failed to find data service");
    Serial.println(serviceUUID.toString().c_str());
  } else {
    Serial.println("- Found data service");
  }

  return floraService;
}

bool forceFloraServiceDataMode(BLERemoteService* floraService) {
  BLERemoteCharacteristic* floraCharacteristic;

  // get device mode characteristic, needs to be changed to read data
  Serial.println("- Force device in data mode");
  floraCharacteristic = nullptr;
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_write_mode);
  } catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping device");
    return false;
  }

  // write the magic data
  uint8_t buf[2] = { 0xA0, 0x1F };
  floraCharacteristic->writeValue(buf, 2, true);

  delay(500);
  return true;
}

bool readFloraDataCharacteristic(BLERemoteService* floraService, String deviceIdentifier , String deviceName) {
  BLERemoteCharacteristic* floraCharacteristic = nullptr;

  // get the main device data characteristic
  Serial.println("- Access characteristic from device");
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_sensor_data);
  } catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping device");
    return false;
  }

  // read characteristic value
  Serial.print("- Read value from characteristic: ");
  std::string value;
  try {
    value = floraCharacteristic->readValue();
  } catch (...) {
    // something went wrong
    Serial.println("-- Failed, skipping device");
    return false;
  }
  const char* val = value.c_str();

  Serial.print("Hex: ");
  for (int i = 0; i < 16; i++) {
    Serial.print((int)val[i], HEX);
    Serial.print(" ");
  }
  Serial.println(" ");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return false;
  }

  char timeStringBuff[25];

  strftime(timeStringBuff,
           sizeof(timeStringBuff),
           "%d-%m-%Y %H:%M:%S",
           &timeinfo);

  Serial.print("-- Time:         ");
  Serial.println(timeStringBuff);

  char buffer[64];

  int16_t* temp_raw = (int16_t*)val;
  float temperature = (*temp_raw) / ((float)10.0);

  Serial.print("-- Temperature:  ");
  Serial.print(temperature);
  Serial.print("°C");
  if (temperature != 0 && temperature > -20 && temperature < 40) {
    snprintf(buffer, 64, "%2.1f", temperature);
    Serial.println("");
  } else {
    Serial.println("   >> Skip");
  }

  int moisture = val[7];
  Serial.print("-- Moisture:     ");
  Serial.print(moisture);
  Serial.print(" %");
  if (moisture <= 100 && moisture >= 0) {
    snprintf(buffer, 64, "%d", moisture);
    Serial.println("");
  } else {
    Serial.println("   >> Skip");
  }

  int light = val[3] + val[4] * 256;
  Serial.print("-- Light:        ");
  Serial.print(light);
  Serial.print(" lux");
  if (light >= 0) {
    snprintf(buffer, 64, "%d", light);
    Serial.println("");
  } else {
    Serial.println("   >> Skip");
  }

  int conductivity = val[8] + val[9] * 256;
  Serial.print("-- Conductivity: ");
  Serial.print(conductivity);
  Serial.print(" uS/cm");
  if (conductivity >= 0 && conductivity < 5000) {
    snprintf(buffer, 64, "%d", conductivity);
    Serial.println("");
  } else {
    Serial.println("   >> Skip");
  }

  String response;
  // Instead of using FirebaseJson for response, you can use String for response to the functions
  // especially in low memory device that deserializing large JSON response may be failed as in ESP8266

  Serial.print("\nUpdate Miflora spreadsheet for device ");
  Serial.print(deviceName);
  Serial.println(" ...");
  Serial.println("------------------------");

  String sheetName = deviceName + "_sensor";
  String rangeValue = sheetName + "!A2:D2";

  FirebaseJson valueRange;
  valueRange.add("range", rangeValue);
  valueRange.add("majorDimension", "COLUMNS");
  valueRange.set("values/[0]/[0]", timeStringBuff); // column A/row 1
  valueRange.set("values/[1]/[0]", deviceName); // column B/row 1
  valueRange.set("values/[2]/[0]", temperature); // column C/row 1
  valueRange.set("values/[3]/[0]", moisture); // column D/row 1


  // For Google Sheet API ref doc, go to https://developers.google.com/sheets/api/reference/rest/v4/spreadsheets.values/update
  bool success = GSheet.values.update(&response /* returned response */, SPREADSHEET_ID /* spreadsheet Id to update */, rangeValue /* range to update */, &valueRange /* data to update */);

  if (success) {
    //response.toString(Serial, true);
    Serial.print(response);
  }
  else {
    Serial.println(GSheet.errorReason());
    return false;
  }

  Serial.println();

  //Serial.println(ESP.getFreeHeap());

  return true;
}

bool readFloraBatteryCharacteristic(BLERemoteService* floraService, String deviceIdentifier , String deviceName) {
  BLERemoteCharacteristic* floraCharacteristic = nullptr;

  // get the device battery characteristic
  Serial.println("- Access battery characteristic from device");
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_version_battery);
  } catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping battery level");
    return false;
  }

  // read characteristic value
  Serial.println("- Read value from characteristic");
  std::string value;
  try {
    value = floraCharacteristic->readValue();
  } catch (...) {
    // something went wrong
    Serial.println("-- Failed, skipping battery level");
    return false;
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return false;
  }

  char timeStringBuff[25];

  strftime(timeStringBuff,
           sizeof(timeStringBuff),
           "%d-%m-%Y %H:%M:%S",
           &timeinfo);

  Serial.print("-- Time:         ");
  Serial.println(timeStringBuff);

  const char* val2 = value.c_str();
  int battery = val2[0];

  char buffer[64];
  Serial.print("-- Battery:      ");
  Serial.print(battery);
  Serial.println(" %");
  snprintf(buffer, 64, "%d", battery);
  Serial.println("");
  // client.publish((baseTopic + "battery").c_str(), buffer);

  String sheetName = deviceName + "_battery";
  String rangeValue = sheetName + "!A2:C2";

  FirebaseJson valueRange;
  valueRange.add("range", rangeValue);
  valueRange.add("majorDimension", "COLUMNS");
  valueRange.set("values/[0]/[0]", timeStringBuff); // column A/row 2
  valueRange.set("values/[1]/[0]", deviceName); // column B/row 2
  valueRange.set("values/[2]/[0]", battery); // column C/row 2

  String response;

  // For Google Sheet API ref doc, go to https://developers.google.com/sheets/api/reference/rest/v4/spreadsheets.values/update
  bool success = GSheet.values.update(&response /* returned response */, SPREADSHEET_ID /* spreadsheet Id to update */, rangeValue /* range to update */, &valueRange /* data to update */);

  if (success) {
    //response.toString(Serial, true);
    Serial.print(response);
  }
  else
  { Serial.println(GSheet.errorReason());
    return false;
  }

  Serial.println();

  return true;
}

bool processFloraService(BLERemoteService* floraService, char* deviceIdentifier, char* deviceName, bool readBattery) {
  // set device in data mode
  if (!forceFloraServiceDataMode(floraService)) {
    return false;
  }

  //Begin the access token generation for Google API authentication
  GSheet.begin(CLIENT_EMAIL, PROJECT_ID, PRIVATE_KEY);

  //Call ready() repeatedly in loop for authentication checking and processing
  bool ready = GSheet.ready();

  bool dataSuccess = readFloraDataCharacteristic(floraService, deviceIdentifier, deviceName);

  bool batterySuccess = true;
  if (readBattery) {
    batterySuccess = readFloraBatteryCharacteristic(floraService, deviceIdentifier, deviceName);
  }

  return dataSuccess && batterySuccess;
}

bool processFloraDevice(BLEAddress floraAddress, char* deviceIdentifier, char* deviceName, bool getBattery, int tryCount) {
  Serial.print("Processing Flora device at ");
  Serial.print(floraAddress.toString().c_str());
  Serial.print(" (try ");
  Serial.print(tryCount);
  Serial.println(")");

  // connect to flora ble server
  BLEClient* floraClient = getFloraClient(floraAddress);
  if (floraClient == nullptr) {
    return false;
  }

  // connect data service
  BLERemoteService* floraService = getFloraService(floraClient);
  if (floraService == nullptr) {
    floraClient->disconnect();
    return false;
  }

  // process devices data
  bool success = processFloraService(floraService, deviceIdentifier, deviceName, getBattery);

  // disconnect from device
  floraClient->disconnect();

  return success;
}

void hibernate() {
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION * 1000000ll);
  Serial.println("Going to sleep now.");
  delay(100);
  esp_deep_sleep_start();
}

void delayedHibernate(void* parameter) {
  delay(EMERGENCY_HIBERNATE * 1000);  // delay for five minutes
  Serial.println("Something got stuck, entering emergency hibernate...");
  hibernate();
}

void setTimezone(String timezone) {
  Serial.printf("  Setting Timezone to %s\n", timezone.c_str());
  setenv("TZ", timezone.c_str(), 1); //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
}

void setup() {
  // all action is done when device is woken up
  Serial.begin(115200);
  delay(1000);

  // increase boot count
  bootCount++;

  // create a hibernate task in case something gets stuck
  xTaskCreate(delayedHibernate, "hibernate", 4096, NULL, 1, &hibernateTaskHandle);

  Serial.println("Initialize BLE client...");
  BLEDevice::init("");
  BLEDevice::setPower(ESP_PWR_LVL_P7);

  // connecting wifi
  connectWifi();

  // Init and get the time
  configTime(0, 0, ntpServer);    // First connect to NTP server, with 0 TZ offset

  // Now we can set the real timezone
  setTimezone(TIMEZONE);

  Serial.println("");
  // check if battery status should be read - based on boot count
  bool readBattery = ((bootCount % BATTERY_INTERVAL) == 0);

  // process devices
  for (int i = 0; i < deviceCount; i++) {
    int tryCount = 0;
    char* deviceMacAddress = FLORA_DEVICES[i][0];
    char* deviceName = FLORA_DEVICES[i][1];
    BLEAddress floraAddress(deviceMacAddress);

    while (tryCount < RETRY) {
      tryCount++;
      if (processFloraDevice(floraAddress, deviceMacAddress, deviceName, readBattery, tryCount)) {
        break;
      }
      delay(1000);
    }
    delay(1500);
  }

  // disconnect wifi
  disconnectWifi();

  // delete emergency hibernate task
  vTaskDelete(hibernateTaskHandle);

  // go to sleep now
  hibernate();
}

void loop() {
  /// we're not doing anything in the loop, only on device wakeup
  delay(10000);
}
