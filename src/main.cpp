#include <Arduino.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <FastLED.h>

BLEServer *pServer = NULL;
BLECharacteristic *pSensorCharacteristic = NULL;
BLECharacteristic *pLedCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;
uint32_t value = 0;

const int ledPin = 2;
const int flashPin = GPIO_NUM_17;
const int cameraPin = GPIO_NUM_16;
const int sensorPin = GPIO_NUM_32;
const int rgbPin = GPIO_NUM_26;

bool ready = true;
uint flashDelay = 480;

#define COLOR_ORDER GRB
#define CHIPSET WS2811
#define NUM_LEDS 1
#define BRIGHTNESS 75
CRGB leds[NUM_LEDS];

static TaskHandle_t task_flash_fire;
static TaskHandle_t task_camera_fire;
static TaskHandle_t task_sensor_triggered;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;

#define SERVICE_UUID "5d898c34-12aa-4af5-8adc-0b105f04b528"
#define SENSOR_CHARACTERISTIC_UUID "db8d3ab6-9cdb-4159-aac6-ab155aabaa71"
#define LED_CHARACTERISTIC_UUID "4add5085-9905-4a7b-8800-032e7db97f32"

static void FlashFireTask(void *pvParameters)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Serial.println("Trigger a flash");
    digitalWrite(flashPin, HIGH);
    delay(15);
    digitalWrite(flashPin, LOW);

    delay(2000);
    taskENTER_CRITICAL(&mutex);
    ready = true;
    taskEXIT_CRITICAL(&mutex);
    if (deviceConnected)
    {
      leds[0] = CRGB::Yellow;
    }
    else
    {
      leds[0] = CRGB::Green;
    }
    FastLED.show();
  }
}

static void SensorTriggeredTask(void *pvParameters)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (ready)
    {
      taskENTER_CRITICAL(&mutex);
      ready = false;
      taskEXIT_CRITICAL(&mutex);

      if (deviceConnected)
      {
        leds[0] = CRGB::Purple;
      }
      else
      {
        leds[0] = CRGB::Red;
      }
      FastLED.show();
      xTaskNotify(task_camera_fire, 0, eSetValueWithOverwrite);

      delay(flashDelay);
      xTaskNotify(task_flash_fire, 0, eSetValueWithOverwrite);
    }
  }
}

static void CameraTask(void *pvParameters)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Serial.println("Fire camera");
    digitalWrite(cameraPin, HIGH);
    delay(1000);
    digitalWrite(cameraPin, LOW);
  }
}

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    deviceConnected = true;
    leds[0] = CRGB::Blue;
    FastLED.show();
  };

  void onDisconnect(BLEServer *pServer)
  {
    deviceConnected = false;
    leds[0] = CRGB::Black;
    FastLED.show();
  }
};

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pLedCharacteristic, esp_ble_gatts_cb_param_t *param)
  {

    std::string value = pLedCharacteristic->getValue();
    if (value.length() > 0)
    {
      Serial.print("Characteristic event, written: ");
      Serial.println(static_cast<int>(value[0])); // Print the integer value

      int receivedValue = static_cast<int>(value[0]);
      if (receivedValue == 1)
      {
        digitalWrite(ledPin, HIGH);
        xTaskNotify(task_flash_fire, 0, eSetValueWithOverwrite);
      }
      else
      {
        digitalWrite(ledPin, LOW);
      }
    }
  }
};

void onSensorTriggered()
{
  xTaskNotify(task_sensor_triggered, 0, eSetValueWithOverwrite);
}

void setup()
{
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  pinMode(flashPin, OUTPUT);
  pinMode(cameraPin, OUTPUT);

  pinMode(sensorPin, INPUT);

  digitalWrite(flashPin, LOW);
  digitalWrite(cameraPin, LOW);
  Serial.setDebugOutput(true);

  FastLED.addLeds<WS2812, rgbPin, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  leds[0] = CRGB::Black;
  FastLED.show();

  // esp_log_level_set("*", ESP_LOG_VERBOSE); // Activer les logs de débogage

  // Create the BLE Device
  BLEDevice::init("SPLASH");

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic
  pSensorCharacteristic = pService->createCharacteristic(
      SENSOR_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY |
          BLECharacteristic::PROPERTY_INDICATE);

  // Create the ON button Characteristic
  pLedCharacteristic = pService->createCharacteristic(
      LED_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE);

  // Register the callback for the ON button characteristic
  pLedCharacteristic->setCallbacks(new MyCharacteristicCallbacks());

  // https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.descriptor.gatt.client_characteristic_configuration.xml
  // Create a BLE Descriptor
  pSensorCharacteristic->addDescriptor(new BLE2902());
  pLedCharacteristic->addDescriptor(new BLE2902());

  // Start the service
  pService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0); // set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  Serial.println("Waiting a client connection to notify...");

  attachInterrupt(digitalPinToInterrupt(sensorPin), onSensorTriggered, RISING);

  xTaskCreate(
      FlashFireTask,     /* Task function. */
      "Flash",           /* String with name of task. */
      10000,             /* Stack size in bytes. */
      NULL,              /* Parameter passed as input of the task */
      1,                 /* Priority of the task. */
      &task_flash_fire); /* Task handle. */

  xTaskCreate(
      SensorTriggeredTask,     /* Task function. */
      "Sensor",                /* String with name of task. */
      10000,                   /* Stack size in bytes. */
      NULL,                    /* Parameter passed as input of the task */
      1,                       /* Priority of the task. */
      &task_sensor_triggered); /* Task handle. */

  xTaskCreate(
      CameraTask, /* Task function. */
      "Camera",   /* String with name of task. */
      10000,      /* Stack size in bytes. */
      NULL,       /* Parameter passed as input of the task */
      1,          /* Priority of the task. */
      &task_camera_fire);
}

//

void loop()
{
  // notify changed value
  if (deviceConnected)
  {
    /*pSensorCharacteristic->setValue(String(value).c_str());
     pSensorCharacteristic->notify();
     value++;
     Serial.print("New value notified: ");
     Serial.println(value);

     delay(3000); // bluetooth stack will go into congestion, if too many packets are sent, in 6 hours test i was able to go as low as 3ms
   */
  }
  // disconnecting
  if (!deviceConnected && oldDeviceConnected)
  {
    Serial.println("Device disconnected.");
    delay(500);                  // give the bluetooth stack the chance to get things ready
    pServer->startAdvertising(); // restart advertising
    Serial.println("Start advertising");
    oldDeviceConnected = deviceConnected;
  }
  // connecting
  if (deviceConnected && !oldDeviceConnected)
  {
    // do stuff here on connecting
    oldDeviceConnected = deviceConnected;
    Serial.println("Device Connected");
  }
}