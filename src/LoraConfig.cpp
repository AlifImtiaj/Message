#include "LoraConfig.h"
#include "AppState.h"


LoraConfig loraConfig;

void SetTransmissionPower() {
    Serial.println();
    Serial.print("Enter TX Power (2-20 dBm): ");
    String value = "";
    while (value == "") { value = ReadSerialLine(); delay(10); }
    int power = value.toInt();
    if (power < 2 || power > 20) { Serial.println("Invalid power! Range: 2-20 dBm"); return; }
    loraConfig.transmissionPower = power;
    LoRa.setTxPower(power);
    Serial.print("TX Power set to: "); Serial.print(power); Serial.println(" dBm");
}

void SetSpreadingFactor() {
    Serial.println();
    Serial.print("Enter Spreading Factor (7-12): ");
    String value = "";
    while (value == "") { value = ReadSerialLine(); delay(10); }
    int sf = value.toInt();
    if (sf < 7 || sf > 12) { Serial.println("Invalid spreading factor! Range: sf7-sf12"); return; }
    loraConfig.spreadingFactor = sf;
    LoRa.setSpreadingFactor(sf);
    Serial.print("Spreading factor set to: SF"); Serial.print(sf); Serial.println();
}

void SetBandwidth() {
    Serial.println();
    Serial.println("Available Bandwidths:");
    Serial.println("7800"); Serial.println("10400"); Serial.println("15600");
    Serial.println("20800"); Serial.println("31250"); Serial.println("41700");
    Serial.println("62500"); Serial.println("125000"); Serial.println("250000");
    Serial.println("500000");
    Serial.print("Enter Bandwidth (Hz): ");
    String value = "";
    while (value == "") { value = ReadSerialLine(); delay(10); }
    long bw = value.toInt();
    switch (bw) {
        case 7800: case 10400: case 15600: case 20800: case 31250:
        case 41700: case 62500: case 125000: case 250000: case 500000:
            break;
        default: Serial.println("Invalid bandwidth!"); return;
    }
    loraConfig.bandwidth = bw;
    LoRa.setSignalBandwidth(bw);
    Serial.print("Bandwidth set to: "); Serial.print(bw); Serial.println(" Hz");
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

void ConfigureLoRa() {
    LcdPrint(0, 0, "1. Spreading Factor", true);
    LcdPrint(0, 1, "2. Bandwidth");
    LcdPrint(0, 2, "3. TX Power");
    LcdPrint(0, 3, "Option: ");
    SetInputPosition(10, 3);
    Serial.println("1. Set Spreading Factor");
    Serial.println("2. Set Bandwidth");
    Serial.println("3. Transmission Power");
    Serial.println("4. Enter your option");
}
