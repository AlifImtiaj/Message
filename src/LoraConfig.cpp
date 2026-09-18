#include "LoraConfig.h"
#include "AppState.h"
#include "debug.h"

#include <Preferences.h>


LoraConfig loraConfig;

void SetTransmissionPower()
{
    LcdPrint(0, 0, "Enter TX Power", true);
    LcdPrint(0, 1, "Range: 2-20 dBm");
    String currentValue = "Current: ";
    currentValue += loraConfig.transmissionPower;
    currentValue += " dBm";
    LcdPrint(0,2, currentValue);
    LcdPrint(0, 3, "Option: ");
    SetInputPosition(8, 3);

    Serial.println();
    Serial.print("Enter TX Power (2-20 dBm): ");

    String value = "";
    while (value == "")
    {
        value = ReadSerialLine();
        delay(10);
    }

    int power = value.toInt();

    // toInt() returns 0 both for "0" and for a non-numeric
    // string. Since 0 is outside our valid range anyway,
    // the range check below correctly rejects both cases.

    if (power < 2 || power > 20)
    {
        Serial.println("Invalid power! Range: 2-20 dBm");

        LcdPrint(0, 0, "Invalid Option", true);
        delay(800);

        ConfigureLoRa();
        return;
    }

    loraConfig.transmissionPower = power;
    LoRa.setTxPower(power);
    Preferences prefs;
    prefs.begin("config", false);
    prefs.putInt("txPower", power);
    prefs.end();

    Serial.print("TX Power set to: ");
    Serial.print(power);
    Serial.println(" dBm");

    LcdPrint(0, 0, "TX Power set:", true);
    LcdPrint(0, 1, String(power) + " dBm");
    delay(800);

    ConfigureLoRa();
}

// ============================================================
// SET SPREADING FACTOR
// ============================================================

void SetSpreadingFactor()
{
    LcdPrint(0, 0, "Enter Spreading", true);
    LcdPrint(0, 1, "Factor. Range:7-12");
    String currentValue = "Current: SF";
    currentValue += loraConfig.spreadingFactor; 
    LcdPrint(0,2, currentValue);
    LcdPrint(0, 3, "Option: ");
    SetInputPosition(8, 3);

    Serial.println();
    Serial.print("Enter Spreading Factor (7-12): ");

    String value = "";
    while (value == "")
    {
        value = ReadSerialLine();
        delay(10);
    }

    int sf = value.toInt();

    if (sf < 7 || sf > 12)
    {
        Serial.println("Invalid spreading factor! Range: 7-12");

        LcdPrint(0, 0, "Invalid Option", true);
        delay(800);

        ConfigureLoRa();
        return;
    }

    loraConfig.spreadingFactor = sf;
    LoRa.setSpreadingFactor(sf);
    Preferences prefs;
    prefs.begin("config", false);
    prefs.putInt("sf", sf);
    prefs.end();

    Serial.print("Spreading factor set to: SF");
    Serial.println(sf);

    LcdPrint(0, 0, "SF set:", true);
    LcdPrint(0, 1, "SF" + String(sf));
    delay(800);

    ConfigureLoRa();
}

// ============================================================
// SET BANDWIDTH
// ============================================================

void SetBandwidth()
{
    LcdPrint(0, 0, "Enter Bandwidth Hz", true);
    LcdPrint(0, 1, "See Serial Monitor");
    String currentValue = "Current: ";
    currentValue += loraConfig.bandwidth;
    currentValue += " Hz";
    LcdPrint(0,2, currentValue);
    LcdPrint(0, 3, "Option: ");
    SetInputPosition(8, 3);

    Serial.println();
    Serial.println("Available Bandwidths:");
    Serial.println("7800");
    Serial.println("10400");
    Serial.println("15600");
    Serial.println("20800");
    Serial.println("31250");
    Serial.println("41700");
    Serial.println("62500");
    Serial.println("125000");
    Serial.println("250000");
    Serial.println("500000");
    Serial.print("Enter Bandwidth (Hz): ");

    String value = "";
    while (value == "")
    {
        value = ReadSerialLine();
        delay(10);
    }

    long bw = value.toInt();

    bool valid = false;

    switch (bw)
    {
        case 7800:
        case 10400:
        case 15600:
        case 20800:
        case 31250:
        case 41700:
        case 62500:
        case 125000:
        case 250000:
        case 500000:
            valid = true;
            break;
        default:
            valid = false;
            break;
    }

    if (!valid)
    {
        Serial.println("Invalid bandwidth!");

        LcdPrint(0, 0, "Invalid Option", true);
        delay(800);

        ConfigureLoRa();
        return;
    }

    loraConfig.bandwidth = bw;
    LoRa.setSignalBandwidth(bw);
    Preferences prefs;
    prefs.begin("config", false);
    prefs.putLong("bdwidth", bw);
    prefs.end();

    Serial.print("Bandwidth set to: ");
    Serial.print(bw);
    Serial.println(" Hz");

    LcdPrint(0, 0, "Bandwidth set:", true);
    LcdPrint(0, 1, String(bw) + " Hz");
    delay(800);

    ConfigureLoRa();
}

void SetCodingRate() {
    Serial.println();
    Serial.print("Enter Coding Rate denominator (5-8): ");
    String value = "";
    while (value == "") { value = ReadSerialLine(); delay(10); }
    int cr = value.toInt();
    if (cr < 5 || cr > 8) { Serial.println("Invalid Coding Rate! Range: 5-8"); return; }
    loraConfig.codingRate = cr;
    LoRa.setCodingRate4(cr);
    Serial.print("Coding Rate set to: 4/"); Serial.println(cr);
}

String ReadSerialLine()
{
    while (Serial.available())
    {
        char c = Serial.read();


        // ========================================================
        // BACKSPACE
        // ========================================================

        if (c == '\b' || c == 127)
        {
            if (currentInput.length() > 0)
            {
                currentInput.remove(
                    currentInput.length() - 1
                );

                Serial.print("\b \b");
            }

            // --------------------------------------------
            // SEND_MESSAGE has scrolling input.
            // --------------------------------------------

            if (appState == AppState::SEND_MESSAGE)
            {
                RedrawScrollingInput(
                    currentInput,
                    inputRow
                );
            }

            // --------------------------------------------
            // MENU / CHECK_MESSAGE / CONFIGURE use the
            // normal LCD echo.
            // --------------------------------------------

            else
            {
                LcdEchoChar('\b');
            }
        }


        // ========================================================
        // ENTER
        // ========================================================

        else if (c == '\n' || c == '\r')
        {
            currentInput.trim();

            String result =
                currentInput;

            currentInput = "";

            Serial.println();


            // Clear SEND_MESSAGE input area after Enter.
            //
            // HandleMenuInput() will immediately move to the
            // next screen if the message is sent.

            if (appState == AppState::SEND_MESSAGE)
            {
                RedrawScrollingInput(
                    "",
                    inputRow
                );
            }

            return result;
        }


        // ========================================================
        // NORMAL CHARACTER
        // ========================================================

        else
        {
            // ----------------------------------------------------
            // Do NOT accept keyboard input while the notification
            // is displayed.
            //
            // This prevents characters typed during the
            // notification from secretly entering the command
            // buffer.
            // ----------------------------------------------------

            if (messageNotificationActive)
            {
                continue;
            }


            currentInput += c;

            Serial.print(c);


            // ----------------------------------------------------
            // SEND_MESSAGE
            // ----------------------------------------------------

            if (appState == AppState::SEND_MESSAGE)
            {
                RedrawScrollingInput(
                    currentInput,
                    inputRow
                );
            }


            // ----------------------------------------------------
            // MENU / CHECK_MESSAGE / CONFIGURE
            // ----------------------------------------------------

            else
            {
                LcdEchoChar(c);
            }
        }
    }

    return "";
}

void ConfigureLoRa()
{
    LcdPrint(0, 0, "1. Spreading Factor", true);
    LcdPrint(0, 1, "2. Bandwidth");
    LcdPrint(0, 2, "3. TX Power");
    LcdPrint(0, 3, "Option: ");
    SetInputPosition(8, 3);

    Serial.println();
    Serial.println("1. Set Spreading Factor");
    Serial.println("2. Set Bandwidth");
    Serial.println("3. Set Transmission Power");
    Serial.println("Type 'done' to exit configuration.");
}