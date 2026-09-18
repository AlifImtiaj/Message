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
#include "debug.h"

#define ROW 4
#define COL 20

#define LED_PIN 13

LiquidCrystal_I2C display(
    0x27,
    COL,
    ROW
);

void playStartupAnimation4(LiquidCrystal_I2C &lcd);


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

    prefs.begin("config", false);
    if (!prefs.isKey("txPower")) {
        prefs.putInt("txPower", loraConfig.transmissionPower);
        DEBUG_PRINT("TxPower set: ");
        DEBUG_PRINTLN(loraConfig.transmissionPower);
    }
    else {
        loraConfig.transmissionPower = prefs.getInt("txPower", 17);
        DEBUG_PRINT("TxPower get: ");
        DEBUG_PRINTLN(loraConfig.transmissionPower);
    }
    if (!prefs.isKey("sf")) {
        prefs.putInt("sf", loraConfig.spreadingFactor);
        DEBUG_PRINT("Spreading Factor set: ");
        DEBUG_PRINTLN(loraConfig.spreadingFactor);

    }
    else {
        loraConfig.spreadingFactor = prefs.getInt("sf", 8);
        DEBUG_PRINT("Spreading Factor get: ");
        DEBUG_PRINTLN(loraConfig.spreadingFactor);
    }
    if (!prefs.isKey("bdwidth")) {
        prefs.putLong("bdwidth", loraConfig.bandwidth);
        DEBUG_PRINT("Bandwidth set: ");
        DEBUG_PRINTLN(loraConfig.bandwidth);
    }
    else {
        loraConfig.bandwidth = prefs.getLong("bdwidth", 125E3);
        DEBUG_PRINT("Bandwidth get: ");
        DEBUG_PRINTLN(loraConfig.bandwidth);
    }
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

    // debug print lines
    DEBUG_PRINTLN();
    DEBUG_PRINTLN("========== LORA CONFIG ==========");

    DEBUG_PRINT("Frequency: ");
    DEBUG_PRINTLN(433E6);

    DEBUG_PRINT("TX Power: ");
    DEBUG_PRINT(loraConfig.transmissionPower);
    DEBUG_PRINTLN(" dBm");

    DEBUG_PRINT("SF: ");
    DEBUG_PRINTLN(loraConfig.spreadingFactor);

    DEBUG_PRINT("Bandwidth: ");
    DEBUG_PRINTLN(loraConfig.bandwidth);

    DEBUG_PRINT("Coding Rate: 4/");
    DEBUG_PRINTLN(loraConfig.codingRate);

    DEBUG_PRINT("Packet size: ");
    DEBUG_PRINTLN(sizeof(LoRaPacket));

    DEBUG_PRINTLN("=================================");
    // debug ends here

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
    playStartupAnimation4(display);
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

void playStartupAnimation4(LiquidCrystal_I2C &lcd) {
  // CGRAM slots 0-3: bar shade levels (dim fill -> bright leading edge)
  static byte shade1[8] = {0b00000,0b00000,0b01010,0b00000,0b00000,0b01010,0b00000,0b00000}; // sparse dither
  static byte shade2[8] = {0b10101,0b01010,0b10101,0b01010,0b10101,0b01010,0b10101,0b01010}; // medium dither
  static byte shadeFull[8]={0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111}; // solid
  static byte shadeEdge[8]={0b11111,0b11111,0b01110,0b01110,0b01110,0b01110,0b11111,0b11111}; // bright glowing tip
 
  lcd.createChar(0, shade1);
  lcd.createChar(1, shade2);
  lcd.createChar(2, shadeFull);
  lcd.createChar(3, shadeEdge);
 
  const uint8_t COLS = 20;
  const unsigned long ANIM_DURATION_MS = 4000;
  const unsigned long FRAME_INTERVAL_MS = 40; // ~25fps cap
 
  const char* DEVICE_TITLE = " LoRa NODE ";
  const char* logLines[] = {
    "Init radio module",
    "Loading config",
    "Calibrating sensor",
    "Starting mesh link"
  };
  const uint8_t NUM_LOGS = 4;
 
  lcd.clear();
 
  // --- Static border, drawn once ---
  lcd.setCursor(0, 0);
  lcd.print("+------------------+");
  lcd.setCursor(0, 3);
  lcd.print("+------------------+");
  lcd.setCursor(1, 0);
  lcd.print(DEVICE_TITLE);
  lcd.setCursor(0, 1);
  lcd.print("|");
  lcd.setCursor(19, 1);
  lcd.print("|");
  lcd.setCursor(0, 2);
  lcd.print("|");
  lcd.setCursor(19, 2);
  lcd.print("|");
 
  const uint8_t BAR_COL_START = 1;
  const uint8_t BAR_WIDTH = 18;
  const uint8_t BAR_ROW = 2;
 
  unsigned long startTime = millis();
  unsigned long lastFrameTime = 0;
  int8_t lastLogIndex = -1;
  uint8_t lastRevealCount = 0;
  int lastPercent = -1;
 
  while (true) {
    unsigned long now = millis();
    unsigned long elapsed = now - startTime;
    if (elapsed >= ANIM_DURATION_MS) break;
 
    if (now - lastFrameTime < FRAME_INTERVAL_MS) continue;
    lastFrameTime = now;
 
    float progress = (float)elapsed / (float)ANIM_DURATION_MS;
 
    // --- Boot log: cycle through log lines, each "typing in" over its time slice ---
    float logSlice = 1.0f / NUM_LOGS;
    uint8_t logIndex = (uint8_t)(progress / logSlice);
    if (logIndex >= NUM_LOGS) logIndex = NUM_LOGS - 1;
    float logLocalProgress = (progress - logIndex * logSlice) / logSlice;
    uint8_t lineLen = strlen(logLines[logIndex]);
    uint8_t revealCount = (uint8_t)(logLocalProgress * (lineLen + 3)); // +3 gives a brief pause after full reveal
    if (revealCount > lineLen) revealCount = lineLen;
 
    if (logIndex != lastLogIndex) {
      lcd.setCursor(1, 1);
      lcd.print("                  "); // clear line (18 spaces, inside border)
      lastRevealCount = 0;
      lastLogIndex = logIndex;
    }
    if (revealCount != lastRevealCount) {
      lcd.setCursor(1, 1);
      for (uint8_t i = 0; i < revealCount; i++) {
        lcd.print(logLines[logIndex][i]);
      }
      // blinking cursor block at the typing position
      if (revealCount < lineLen) {
        lcd.write((uint8_t)2);
      }
      lastRevealCount = revealCount;
    }
 
    // --- Progress bar with glowing leading edge ---
    float filledUnitsF = progress * BAR_WIDTH;
    uint8_t fullCells = (uint8_t)filledUnitsF;
    if (fullCells > BAR_WIDTH) fullCells = BAR_WIDTH;
 
    for (uint8_t i = 0; i < BAR_WIDTH; i++) {
      lcd.setCursor(BAR_COL_START + i, BAR_ROW);
      if (i < fullCells) {
        lcd.write((uint8_t)2); // solid fill
      } else if (i == fullCells) {
        lcd.write((uint8_t)3); // glowing edge at the leading tip
      } else if (i == fullCells + 1) {
        lcd.write((uint8_t)1); // medium dither just ahead
      } else if (i == fullCells + 2) {
        lcd.write((uint8_t)0); // sparse dither further ahead
      } else {
        lcd.print(' ');
      }
    }
 
    // --- Percentage counter, printed on row 0 right side inside border ---
    int percent = (int)(progress * 100);
    if (percent != lastPercent) {
      lcd.setCursor(15, 0);
      char buf[5];
      sprintf(buf, "%3d%%", percent);
      lcd.print(buf);
      lastPercent = percent;
    }
  }
 
  // --- Final state: full bar, 100%, then READY flash ---
  for (uint8_t i = 0; i < BAR_WIDTH; i++) {
    lcd.setCursor(BAR_COL_START + i, BAR_ROW);
    lcd.write((uint8_t)2);
  }
  lcd.setCursor(15, 0);
  lcd.print("100%");
  lcd.setCursor(1, 1);
  lcd.print("                  ");
 
  delay(200);
  lcd.setCursor(7, 1);
  lcd.print("READY");
  delay(500);
 
  lcd.clear();
}