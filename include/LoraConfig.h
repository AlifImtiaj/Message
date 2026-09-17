#pragma once
#include <Arduino.h>
#include <LoRa.h>
#include <LiquidCrystal_I2C.h>


#define LORA_SS 5
#define LORA_RST 12
#define LORA_DIO0 14

#define MY_DEVICE_ID 2

struct LoraConfig {
  int spreadingFactor = 7;
  int transmissionPower = 17;
  long bandwidth = 125E3;
  int codingRate = 5;
};

extern LoraConfig loraConfig;

struct LoRaPacket
{
    char magic[10];          // 10 bytes
    uint16_t messageID;      // 2
    uint16_t senderID;       // 2
    uint16_t receiverID;     // 2
    uint16_t totalPackets;   // 2
    uint16_t packetIndex;    // 2
    uint8_t payloadLength;   // 1
    char payload[150];       // 150
};

void SetTransmissionPower();
void SetSpreadingFactor();
void SetBandwidth();
void SetCodingRate();
String ReadSerialLine();

void ConfigureLoRa();