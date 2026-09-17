#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <LiquidCrystal_I2C.h>
#include <esp_task_wdt.h>

#include "LoraConfig.h"
#include "AppState.h"

#define ROW 4
#define COL 20
#define LED_PIN 13

LiquidCrystal_I2C display(0x27, COL, ROW);

void setup() {
  Serial.begin(9600);
  Wire.begin();
  SPI.begin();
  display.init();
  display.backlight();
  display.setCursor(0,0);
  display.print("Hello World");

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.print("Setup failed");
    display.clear();
    display.setCursor(0,0);
    display.print("Setup failed");
    esp_task_wdt_delete(NULL);
    while (true);
  }
  pinMode(13, OUTPUT);

  LoRa.setTxPower(loraConfig.transmissionPower);
  LoRa.setCodingRate4(loraConfig.codingRate);
  LoRa.setSpreadingFactor(loraConfig.spreadingFactor);
  LoRa.setSignalBandwidth(loraConfig.bandwidth);

  display.setCursor(0,0);
  display.print("SX1278 Success");
  delay(200);

  displayMutex = xSemaphoreCreateMutex();
  ShowMenu();

  xTaskCreate(ReceiveTask, "ReceiveTask", 4096, NULL, 1, &receiveTaskHandle);
}

void loop() {
  String line = ReadSerialLine();
  if (line.length() == 0) return;
  HandleMenuInput(line);
}