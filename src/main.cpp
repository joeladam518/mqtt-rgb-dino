#include "config.h"
// Library Headers
#include <WiFi.h> // ESP32 Wifi client library
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
// Custom Headers
#include "main.h"
#include "led.h"
#include "NeoPixelRing.h"

//==============================================================================
// Globals

/**
 *  Adafruit NeoPixel object
 *
 *  Argument 1 = Number of pixels in NeoPixel strip
 *  Argument 2 = Arduino pin number (most are valid)
 *  Argument 3 = Pixel type flags, add together as needed:
 *      NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
 *      NEO_KHZ400  400 KHz (classic 'v1' (not v2) FLORA pixels, WS2811 drivers)
 *      NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
 *      NEO_RGB     Pixels are wired for RGB bitstream (v1 FLORA pixels, not v2)
 *      NEO_RGBW    Pixels are wired for RGBW bitstream (NeoPixel RGBW products)
 */
Adafruit_NeoPixel neoPixel(NEO_PIXEL_COUNT, NEO_PIXEL_PIN, NEO_GRB + NEO_KHZ800);

// work with the neo pixels in object form
NeoPixelRing ring(&neoPixel);

// Wifi client
WiFiClient client;

// Mqtt client
Adafruit_MQTT_Client mqtt(&client, MQTT_BROKER, MQTT_PORT, MQTT_USER, MQTT_PASS);

// Mqtt client subscriptons
Adafruit_MQTT_Subscribe getColorSub = Adafruit_MQTT_Subscribe(&mqtt, SUB_GET_COLOR);
Adafruit_MQTT_Subscribe setColorSub = Adafruit_MQTT_Subscribe(&mqtt, SUB_SET_COLOR);

// Task handles
static TaskHandle_t processInputTaskHandle = NULL;
static TaskHandle_t processShortActionsTaskHandle = NULL;
static TaskHandle_t processLongActionsTaskHandle = NULL;

// Queues
static QueueHandle_t shortActionsQueue = NULL;
static QueueHandle_t longActionsQueue = NULL;

// Mutexes
static SemaphoreHandle_t ringMutex;

//==============================================================================
// Main

// NOTE: Set up is excuted on core #1
void setup()
{
    Serial.begin(115200);
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Connect to Wifi
    Serial.println("");
    Serial.print(F("Connecting to "));
    Serial.print(WLAN_SSID);

    WiFi.begin(WLAN_SSID, WLAN_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        Serial.print(".");
    }

    Serial.println(F("Success!"));
    Serial.print(F("IP address: "));
    Serial.println(WiFi.localIP());

    // Setup Mqtt
    mqtt.unsubscribe(&getColorSub);
    mqtt.unsubscribe(&setColorSub);

    getColorSub.setCallback(getColor);
    mqtt.subscribe(&getColorSub);
    setColorSub.setCallback(setColor);
    mqtt.subscribe(&setColorSub);

    // Configure RTOS
    shortActionsQueue = xQueueCreate(2, sizeof(SubscriptionAction_t));
    longActionsQueue = xQueueCreate(4, sizeof(SubscriptionAction_t));
    ringMutex = xSemaphoreCreateMutex();

    // Initialize neopixel ring
    if (xSemaphoreTake(ringMutex, 0) == pdTRUE) {
        ring.begin();
        xSemaphoreGive(ringMutex);
    }

    xTaskCreatePinnedToCore(
        processInputTask,               // Function to be called
        "Process Mqtt Input",           // Name of task
        1750,                           // Stack size (bytes in ESP32, words in FreeRTOS)
        NULL,                           // Parameter to pass to function
        1,                              // Task priority (0 to configMAX_PRIORITIES - 1)
        &processInputTaskHandle,        // Task handle
        PRO_CPU_NUM                     // Run on core
    );
    vTaskSuspend(processInputTaskHandle);

    xTaskCreatePinnedToCore(
        processShortActionsTask,        // Function to be called
        "Process Short Actions",        // Name of task
        2048,                           // Stack size (bytes in ESP32, words in FreeRTOS)
        NULL,                           // Parameter to pass to function
        2,                              // Task priority (0 to configMAX_PRIORITIES - 1)
        &processShortActionsTaskHandle, // Task handle
        APP_CPU_NUM                     // Run on core
    );
    vTaskSuspend(processShortActionsTaskHandle);

    xTaskCreatePinnedToCore(
        processLongActionsTask,         // Function to be called
        "Process Long Actions",         // Name of task
        2048,                           // Stack size (bytes in ESP32, words in FreeRTOS)
        NULL,                           // Parameter to pass to function
        1,                              // Task priority (0 to configMAX_PRIORITIES - 1)
        &processLongActionsTaskHandle,  // Task handle
        APP_CPU_NUM                     // Run on core
    );
    vTaskSuspend(processLongActionsTaskHandle);

    // Start the mqtt task
    vTaskResume(processInputTaskHandle);
    // Remove the setup() and loop() task
    vTaskDelete(NULL);
}

// NOTE: Loop is excuted on core #1
void loop() {}

//==============================================================================
// Tasks

void processShortActionsTask(void *parameter)
{
    SubscriptionAction_t action;

    while (1) {
        if (xQueueReceive(shortActionsQueue, (void *)&action, 0) == pdTRUE) {
            #if defined(APP_DEBUG) && APP_DEBUG
                Serial.println(F("processShortActionsTask() executing callback..."));
            #endif
            action.callback(action.data, action.length);
            clearAction(&action);
        }

        vTaskDelay(250 / portTICK_PERIOD_MS);
    }
}

void processLongActionsTask(void *parameter)
{
    SubscriptionAction_t action;

    while (1) {
        if (xQueueReceive(longActionsQueue, (void *)&action, 0) == pdTRUE) {
            #if defined(APP_DEBUG) && APP_DEBUG
                Serial.println(F("processLongActionsTask() executing callback..."));
            #endif
            action.callback(action.data, action.length);
            clearAction(&action);
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

void processInputTask(void *parameter)
{
    Adafruit_MQTT_Subscribe *subscription;
    SubscriptionAction action;
    uint32_t startTime = 0;
    uint32_t endTime = 0;
    uint32_t elapsed = 0;

    while (1) {
        mqttConnect();

        elapsed = 0;
        endTime = 0;
        startTime = millis();
        while (elapsed < READ_SUBSCRIPTION_TIMEOUT) {
            if ((subscription = mqtt.readSubscription(READ_SUBSCRIPTION_TIMEOUT - elapsed))) {
                setAction(&action, subscription);

                if (subscription == &getColorSub) {
                    if (action.callback != NULL) {
                        #if defined(APP_DEBUG) && APP_DEBUG
                            Serial.println(F("Sending to short actions queue..."));
                        #endif
                        xQueueSend(shortActionsQueue, (void *)&action, 0);
                        vTaskDelay(250 / portTICK_PERIOD_MS);
                    } else {
                        #if defined(APP_DEBUG) && APP_DEBUG
                            Serial.println(F(""));
                            Serial.println(F("!! Action couldn't be processed !!"));
                            Serial.println(F(""));
                        #endif
                    }

                    break;
                } else if (subscription == &setColorSub) {
                    if (action.callback != NULL) {
                        #if defined(APP_DEBUG) && APP_DEBUG
                            Serial.println(F("Sending to long actions queue..."));
                        #endif
                        xQueueSend(longActionsQueue, (void *)&action, 0);
                        vTaskDelay(250 / portTICK_PERIOD_MS);
                    } else {
                        #if defined(APP_DEBUG) && APP_DEBUG
                            Serial.println(F(""));
                            Serial.println(F("!! Action couldn't be sent to the queue !!"));
                            Serial.println(F(""));
                        #endif
                    }

                    break;
                }
            }

            endTime = millis();
            if (endTime < startTime) startTime = endTime;
            elapsed += (endTime - startTime);
        }
    }
}

//==============================================================================
// Mqtt Callbacks

void getColor(char *data, uint16_t len)
{
    if (SUBSCRIPTIONDATALEN < len) {
        #if defined(APP_DEBUG) && APP_DEBUG
            Serial.println(F("data is larger than max legnth. Can not parse."));
        #endif

        return;
    }

    #if defined(APP_DEBUG) && APP_DEBUG
        Serial.println("getColor(): ");
        printSubscriptionCallbackData(data, len);
    #endif

    publishRgbStatus();

    #if defined(APP_DEBUG) && APP_DEBUG
        Serial.println(F("Done!"));
    #endif
}

void setColor(char *data, uint16_t len)
{
    if (SUBSCRIPTIONDATALEN < len) {
        #if defined(APP_DEBUG) && APP_DEBUG
            Serial.println(F("data is larger than max legnth. Can not parse."));
        #endif

        return;
    }

    #if defined(APP_DEBUG) && APP_DEBUG
        Serial.println("setColor(): ");
        printSubscriptionCallbackData(data, len);
    #endif

    const int capacity = JSON_OBJECT_SIZE(4);
    StaticJsonDocument<capacity> doc;
    DeserializationError error = deserializeJson(doc, data);

    if (error) {
        #if defined(APP_DEBUG) && APP_DEBUG
            printDeserializeError(&error);
        #endif

        return;
    }

    uint8_t r = doc["r"].as<uint8_t>();
    uint8_t g = doc["g"].as<uint8_t>();
    uint8_t b = doc["b"].as<uint8_t>();
    uint16_t time = doc["time"].as<uint16_t>();

    if (xSemaphoreTake(ringMutex, 0) == pdTRUE) {
        if (time) {
            ring.fadeColor(r, g, b, time);
        } else {
            ring.setColor(r, g, b);
        }
        xSemaphoreGive(ringMutex);
    }

    #if defined(APP_DEBUG) && APP_DEBUG
        Serial.println(F("Done!"));
    #endif

    publishRgbStatus();
}

//==============================================================================
// Methods

void mqttConnect()
{
    if (mqtt.connected()) {
        return;
    }

    Serial.print(F("Connecting to MQTT..."));
    vTaskSuspend(processShortActionsTaskHandle);
    vTaskSuspend(processLongActionsTaskHandle);

    uint8_t ret;
    uint8_t retries = 3;
    while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
        Serial.println(F(""));
        Serial.print(F("Mqtt connection error: "));
        Serial.println(mqtt.connectErrorString(ret));
        Serial.println(F("Retrying MQTT connection in 5 seconds..."));

        mqtt.disconnect();
        vTaskDelay(5000 / portTICK_PERIOD_MS);  // wait 5 seconds

        retries--;
        if (retries == 0) {
            Serial.println(F("Could not connetect to the mqtt broker. Ran out of retries..."));
            while (1); // die and wait for reset
        }
    }

    Serial.println(F("Success!"));
    vTaskResume(processShortActionsTaskHandle);
    vTaskResume(processLongActionsTaskHandle);
    vTaskDelay(250 / portTICK_PERIOD_MS);
}

void clearAction(SubscriptionAction_t *action)
{
    action->callback = NULL;
    memset(action->data, '\0', SUBSCRIPTIONDATALEN);
    action->length = 0;
}

void setAction(SubscriptionAction_t *action, Adafruit_MQTT_Subscribe *subscription)
{
    clearAction(action);

    if (subscription != NULL && subscription->callback_buffer != NULL) {
        action->callback = subscription->callback_buffer;
        strncpy(action->data, (char *)subscription->lastread, (SUBSCRIPTIONDATALEN - 1));
        action->length = subscription->datalen;
    }
}

void publishRgbStatus(void)
{
    char output[SUBSCRIPTIONDATALEN];
    const int capacity = JSON_OBJECT_SIZE(3);
    StaticJsonDocument<capacity> doc;

    RGB_t color = {0, 0, 0};
    ring.getColor(&color);

    doc["r"] = color.r;
    doc["g"] = color.g;
    doc["b"] = color.b;

    serializeJson(doc, output, sizeof(output));
    mqtt.publish(PUB_GET_COLOR, output);
}

//==============================================================================
// Debug Helpers

#if defined(APP_DEBUG) && APP_DEBUG
    void printSubscriptionCallbackData(char *data, uint16_t len)
    {
        Serial.print(F("Data: "));
        Serial.println(data);
        Serial.print(F("Length: "));
        Serial.println(len);
    }

    void printDeserializeError(DeserializationError *error)
    {
        Serial.print(F("Arduino Json eserialization error: "));
        Serial.println(error->c_str());
    }
#endif
