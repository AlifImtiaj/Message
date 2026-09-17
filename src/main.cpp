#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <LiquidCrystal_I2C.h>
#include <esp_task_wdt.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "LoraConfig.h"
#include "AppState.h"

#define ROW 4
#define COL 20

#define LED_PIN 13

LiquidCrystal_I2C display(
    0x27,
    COL,
    ROW
);


void setup()
{
    Serial.begin(9600);

    Wire.begin();

    SPI.begin();

    display.init();

    display.backlight();

    display.setCursor(
        0,
        0
    );

    // ========================================================
    // LoRa
    // ========================================================

    LoRa.setPins(
        LORA_SS,
        LORA_RST,
        LORA_DIO0
    );

    if (!LoRa.begin(433E6))
    {
        Serial.println(
            "Setup failed"
        );

        display.clear();

        display.setCursor(
            0,
            0
        );

        display.print(
            "Setup failed"
        );

        esp_task_wdt_delete(NULL);

        while (true)
        {
            delay(1000);
        }
    }

    
    Preferences prefs;
    bool isThereNewMessage = false;
    prefs.begin("MsgNot", false);
    if (!prefs.isKey("isNewMsg"))
        prefs.putBool("isNewMsg", false);
    else
        isThereNewMessage = prefs.getBool("isNewMsg");
    prefs.end();

    pinMode(
        LED_PIN,
        OUTPUT
    );
    if (!isThereNewMessage)
        digitalWrite(13, LOW);
    if (isThereNewMessage)
        digitalWrite(13, HIGH);

    LoRa.setTxPower(
        loraConfig.transmissionPower
    );

    LoRa.setCodingRate4(
        loraConfig.codingRate
    );

    LoRa.setSpreadingFactor(
        loraConfig.spreadingFactor
    );

    LoRa.setSignalBandwidth(
        loraConfig.bandwidth
    );

    // ========================================================
    // Display mutex
    // ========================================================

    displayMutex =
        xSemaphoreCreateMutex();

    if (displayMutex == NULL)
    {
        Serial.println(
            "ERROR: Display mutex failed"
        );

        display.clear();

        display.print(
            "Mutex failed"
        );

        while (true)
        {
            delay(1000);
        }
    }

    // ========================================================
    // LittleFS message storage
    // ========================================================

    if (!InitializeMessageStorage())
{
    Serial.println("================================");
    Serial.println("LittleFS INITIALIZATION FAILED");
    Serial.println("================================");
    
    display.clear();
    display.setCursor(0, 0);
    display.print("LittleFS FAILED");
    display.setCursor(0, 1);
    display.print("See Serial Monitor");

    while (true)
    {
        delay(1000);
    }
}

// if (LittleFS.exists("/messages.dat"))
// {
//     LittleFS.remove("/messages.dat");
//     Serial.println("All stored messages deleted.");
// }



    // ========================================================
    // Start application
    // ========================================================

    ShowMenu();

    xTaskCreate(
        ReceiveTask,
        "ReceiveTask",
        4096,
        NULL,
        1,
        &receiveTaskHandle
    );
}

void loop()
{
    String line =
        ReadSerialLine();

    if (line.length() == 0)
        return;

    HandleMenuInput(
        line
    );
}