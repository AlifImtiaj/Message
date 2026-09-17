#include "AppState.h"

#include <LoRa.h>
#include <LiquidCrystal_I2C.h>

#include "LoraConfig.h"

#define ROW 4
#define COL 20

extern LiquidCrystal_I2C display;


// ============================================================
// APPLICATION STATE
// ============================================================

AppState appState = AppState::MENU;

TaskHandle_t receiveTaskHandle = NULL;

SemaphoreHandle_t displayMutex = NULL;


// ============================================================
// RECEIVED MESSAGES
// ============================================================

String recvdMessages[5];

int recvdMessageCount = 0;


// ============================================================
// SERIAL INPUT
// ============================================================
//
// This is the single input buffer used by ReadSerialLine().
//
// IMPORTANT:
// There is no longer a static local String inside
// ReadSerialLine(). This prevents an old input such as "123"
// from surviving a notification invisibly.
//

String currentInput = "";


// ============================================================
// LCD INPUT CURSOR
// ============================================================

int inputCol = 0;
int inputRow = 3;
int inputMinCol = 0;


// ============================================================
// MESSAGE NOTIFICATION
// ============================================================

bool messageNotificationActive = false;

unsigned long messageNotificationStart = 0;

AppState notificationPreviousState;

String notificationPreviousInput;


// ============================================================
// CHECK_MESSAGE UI
// ============================================================

int checkMessagePage = 0;

int selectedMessageIndex = -1;


// ============================================================
// CHECK_MESSAGE STATE FOR NOTIFICATION RESTORATION
// ============================================================

int notificationPreviousMessagePage = 0;

int notificationPreviousSelectedMessage = -1;

// Save the actual message that was being viewed.
//
// This is important because PushRecvdMessage() inserts a new
// message at index 0 and shifts all existing indexes.
//
// Example:
//
// Before new message:
// index 0 = A
// index 1 = B
// index 2 = C
//
// User is viewing C (index 2).
//
// New message arrives:
//
// index 0 = NEW
// index 1 = A
// index 2 = B
// index 3 = C
//
// Therefore restoring index 2 would incorrectly show B.
//
// Saving the actual message allows us to find C again.


// this section is for scrolling the text
String notificationPreviousSelectedMessageText = "";

static uint16_t nextMessageID = 0;

bool messageScrollActive = false;

int messageScrollPosition = 0;

unsigned long messageScrollTimer = 0;

enum class MessageScrollState
{
    WAIT_AT_START,
    SCROLLING,
    WAIT_AT_END
};

MessageScrollState messageScrollState =
    MessageScrollState::WAIT_AT_START;
// section ends

// ============================================================
// INPUT POSITION
// ============================================================

void SetInputPosition(int col, int row)
{
    inputCol = col;
    inputMinCol = col;
    inputRow = row;
}


// ============================================================
// START MESSAGE NOTIFICATION
// ============================================================

void StartMessageNotification()
{
    if (messageNotificationActive)
        return;

    messageNotificationActive = true;

    messageNotificationStart = millis();

    // Save application state.
    notificationPreviousState = appState;

    // Save current input.
    //
    // We save it here so SEND_MESSAGE can restore the message
    // being typed if a notification interrupts it.
    notificationPreviousInput = currentInput;

    // Save CHECK_MESSAGE UI state.
    notificationPreviousMessagePage = checkMessagePage;
    notificationPreviousSelectedMessage = selectedMessageIndex;

    // Save the actual selected message.
    notificationPreviousSelectedMessageText = "";

    if (appState == AppState::CHECK_MESSAGE &&
        selectedMessageIndex >= 0 &&
        selectedMessageIndex < recvdMessageCount)
    {
        notificationPreviousSelectedMessageText =
            recvdMessages[selectedMessageIndex];
    }

    // --------------------------------------------------------
    // IMPORTANT:
    //
    // A notification interrupts an unfinished OPTION command.
    //
    // If the user typed:
    //
    //     123
    //
    // and then a message arrived, we do NOT want the hidden
    // serial buffer to remain "123" while the LCD later shows:
    //
    //     Option:
    //
    // Otherwise typing 0 would produce:
    //
    //     1230
    //
    // So for MENU / CHECK_MESSAGE / CONFIGURE we discard the
    // unfinished command.
    //
    // SEND_MESSAGE is different: there the input itself is the
    // message being composed, so we preserve it.
    // --------------------------------------------------------

    if (appState != AppState::SEND_MESSAGE)
    {
        currentInput = "";
    }

    // Show notification.
    LcdPrint(
        0,
        0,
        "Message Received",
        true
    );
}


// ============================================================
// RESTORE PREVIOUS SCREEN
// ============================================================

void RestorePreviousScreen()
{
    switch (notificationPreviousState)
    {
        // ----------------------------------------------------
        // MENU
        // ----------------------------------------------------

        case AppState::MENU:
        {
            ShowMenu();

            // MENU command input was intentionally discarded
            // when notification started, so the restored menu
            // starts with an empty Option field.

            currentInput = "";

            break;
        }


        // ----------------------------------------------------
        // SEND MESSAGE
        // ----------------------------------------------------

        case AppState::SEND_MESSAGE:
        {
            LcdPrint(
                0,
                0,
                "Enter message:",
                true
            );

            RedrawScrollingInput(
                notificationPreviousInput,
                1
            );

            // Restore the actual message being typed.
            currentInput = notificationPreviousInput;

            int len = currentInput.length();

            if (len < COL)
            {
                SetInputPosition(len, 1);
            }
            else
            {
                SetInputPosition(COL, 1);
            }

            break;
        }


        // ----------------------------------------------------
        // CHECK MESSAGE
        // ----------------------------------------------------

        case AppState::CHECK_MESSAGE:
        {
            checkMessagePage = notificationPreviousMessagePage;

            // ------------------------------------------------
            // User was viewing a specific message.
            // ------------------------------------------------

            if (notificationPreviousSelectedMessageText.length() > 0)
            {
                int restoredIndex = -1;

                // Search for the actual message after the new
                // message has shifted the array.
                for (int i = 0; i < recvdMessageCount; i++)
                {
                    if (recvdMessages[i] ==
                        notificationPreviousSelectedMessageText)
                    {
                        restoredIndex = i;
                        break;
                    }
                }

                if (restoredIndex >= 0)
                {
                    selectedMessageIndex = restoredIndex;

                    ShowSelectedMessage(
                        selectedMessageIndex
                    );
                }
                else
                {
                    // The old message is no longer stored.
                    selectedMessageIndex = -1;

                    ShowMessagePage();
                }
            }

            // ------------------------------------------------
            // User was on the message list.
            // ------------------------------------------------

            else
            {
                selectedMessageIndex = -1;

                ShowMessagePage();
            }

            // CHECK_MESSAGE command input is reset.
            currentInput = "";

            break;
        }


        // ----------------------------------------------------
        // CONFIGURE
        // ----------------------------------------------------

        case AppState::CONFIGURE:
        {
            LcdPrint(
                0,
                0,
                "Configuring...",
                true
            );

            LcdPrint(
                0,
                1,
                "Check Serial Monitor"
            );

            SetInputPosition(0, 2);

            currentInput = "";

            break;
        }
    }
}


// ============================================================
// UPDATE MESSAGE NOTIFICATION
// ============================================================

void UpdateMessageNotification()
{
    if (!messageNotificationActive)
        return;

    if (millis() - messageNotificationStart >= 2000)
    {
        messageNotificationActive = false;

        RestorePreviousScreen();
    }
}


// ============================================================
// LCD PRINT
// ============================================================

void LcdPrint(
    int col,
    int row,
    const String& text,
    bool clearFirst
)
{
    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        if (clearFirst)
            display.clear();

        display.setCursor(col, row);

        display.print(text);

        xSemaphoreGive(displayMutex);
    }
}


// ============================================================
// LCD ECHO CHARACTER
// ============================================================
//
// This function only handles the LCD.
//
// The actual serial input buffer is maintained by
// ReadSerialLine().
//
// Keeping those responsibilities separate prevents the old
// double-buffer problem.
//

void LcdEchoChar(char c)
{
    // Notification screen must not be overwritten.
    if (messageNotificationActive)
        return;

    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        if (c == '\b')
        {
            if (inputCol > inputMinCol)
            {
                inputCol--;

                display.setCursor(
                    inputCol,
                    inputRow
                );

                display.print(' ');

                display.setCursor(
                    inputCol,
                    inputRow
                );
            }
        }
        else
        {
            if (inputCol < COL)
            {
                display.setCursor(
                    inputCol,
                    inputRow
                );

                display.print(c);

                inputCol++;
            }
        }

        xSemaphoreGive(displayMutex);
    }
}


// ============================================================
// SCROLLING INPUT
// ============================================================

void RedrawScrollingInput(
    const String& text,
    int row
)
{
    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        display.setCursor(0, row);

        for (int i = 0; i < COL; i++)
            display.print(' ');

        int len = text.length();

        int start =
            (len > COL)
                ? (len - COL)
                : 0;

        String visible =
            text.substring(start, len);

        display.setCursor(0, row);

        display.print(visible);

        xSemaphoreGive(displayMutex);
    }
}


// ============================================================
// MAIN MENU
// ============================================================

void ShowMenu()
{
    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        display.clear();

        display.setCursor(0, 0);
        display.print("1. Check Message");

        display.setCursor(0, 1);
        display.print("2. Send Message");

        display.setCursor(0, 2);
        display.print("3. Configure");

        display.setCursor(0, 3);
        display.print("Option: ");

        xSemaphoreGive(displayMutex);
    }

    SetInputPosition(8, 3);
}


// ============================================================
// MESSAGE PREVIEW
// ============================================================

String GetMessagePreview(const String& message)
{
    if (message.length() <= 14)
        return message;

    return message.substring(0, 14) + "...";
}


// ============================================================
// SHOW MESSAGE PAGE
// ============================================================
//
// IMPORTANT:
//
// The displayed numbers are ALWAYS:
//
//     1.
//     2.
//
// They are page-local numbers.
//
// Internally:
//
// page 0:
//     1 -> message index 0
//     2 -> message index 1
//
// page 1:
//     1 -> message index 2
//     2 -> message index 3
//
// page 2:
//     1 -> message index 4
//
// This means you can later expand the storage without having
// to create options 3, 4, 5, 6, etc.
//

void ShowMessagePage()
{
    if (recvdMessageCount <= 0)
    {
        LcdPrint(
            0,
            0,
            "No Message To Show",
            true
        );

        LcdPrint(
            0,
            2,
            "0) Go Back"
        );

        LcdPrint(
            0,
            3,
            "Option: "
        );

        SetInputPosition(8, 3);

        return;
    }

    const int messagesPerPage = 2;

    int startIndex =
        checkMessagePage * messagesPerPage;

    // Safety check.
    if (startIndex >= recvdMessageCount)
    {
        checkMessagePage = 0;

        startIndex = 0;
    }

    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        display.clear();


        // ----------------------------------------------------
        // Message 1
        // ----------------------------------------------------

        if (startIndex < recvdMessageCount)
        {
            display.setCursor(0, 0);

            display.print("1. ");

            display.print(
                GetMessagePreview(
                    recvdMessages[startIndex]
                )
            );
        }


        // ----------------------------------------------------
        // Message 2
        // ----------------------------------------------------

        if (startIndex + 1 < recvdMessageCount)
        {
            display.setCursor(0, 1);

            display.print("2. ");

            display.print(
                GetMessagePreview(
                    recvdMessages[startIndex + 1]
                )
            );
        }


        // ----------------------------------------------------
        // Navigation
        // ----------------------------------------------------

        display.setCursor(0, 2);

        display.print(
            "0.Ex 00.Nxt 000.Bck"
        );


        // ----------------------------------------------------
        // Input
        // ----------------------------------------------------

        display.setCursor(0, 3);

        display.print("Option: ");

        xSemaphoreGive(displayMutex);
    }

    SetInputPosition(8, 3);
}

// ============================================================
// RESET MESSAGE SCROLL
// ============================================================

// ============================================================
// UPDATE MESSAGE SCROLL
// ============================================================

void UpdateMessageScroll()
{
    if (!messageScrollActive)
        return;

    if (selectedMessageIndex < 0 ||
        selectedMessageIndex >= recvdMessageCount)
    {
        ResetMessageScroll();
        return;
    }

    String message =
        recvdMessages[selectedMessageIndex];

    // Safety check.
    if (message.length() <= COL)
    {
        ResetMessageScroll();
        return;
    }

    unsigned long now = millis();

    // ========================================================
    // WAIT AT START
    //
    // Message is initially displayed from character 0.
    //
    // Wait exactly 1 second before moving.
    // ========================================================

    if (messageScrollState ==
        MessageScrollState::WAIT_AT_START)
    {
        if (now - messageScrollTimer >= 1000)
        {
            messageScrollState =
                MessageScrollState::SCROLLING;

            messageScrollTimer = now;
        }

        return;
    }

    // ========================================================
    // SCROLLING
    //
    // Move one character every 250 ms.
    // ========================================================

    if (messageScrollState ==
        MessageScrollState::SCROLLING)
    {
        if (now - messageScrollTimer >= 250)
        {
            messageScrollTimer = now;

            int maxPosition =
                message.length() - COL;

            messageScrollPosition++;

            // ------------------------------------------------
            // Reached the final 20-character window.
            // ------------------------------------------------

            if (messageScrollPosition >= maxPosition)
            {
                messageScrollPosition = maxPosition;

                // Display final position.
                if (xSemaphoreTake(
                        displayMutex,
                        pdMS_TO_TICKS(100)
                    ) == pdTRUE)
                {
                    display.setCursor(0, 1);

                    for (int i = 0; i < COL; i++)
                        display.print(' ');

                    display.setCursor(0, 1);

                    display.print(
                        message.substring(
                            messageScrollPosition,
                            messageScrollPosition + COL
                        )
                    );

                    xSemaphoreGive(displayMutex);
                }

                // Start 1-second wait at the end.
                messageScrollState =
                    MessageScrollState::WAIT_AT_END;

                messageScrollTimer = now;

                return;
            }

            // ------------------------------------------------
            // Display the next scrolling position.
            // ------------------------------------------------

            if (xSemaphoreTake(
                    displayMutex,
                    pdMS_TO_TICKS(100)
                ) == pdTRUE)
            {
                display.setCursor(0, 1);

                for (int i = 0; i < COL; i++)
                    display.print(' ');

                display.setCursor(0, 1);

                display.print(
                    message.substring(
                        messageScrollPosition,
                        messageScrollPosition + COL
                    )
                );

                xSemaphoreGive(displayMutex);
            }
        }

        return;
    }

    // ========================================================
    // WAIT AT END
    //
    // Final 20 characters stay on screen for 1 second.
    //
    // Then return to position 0 and wait another 1 second.
    // ========================================================

    if (messageScrollState ==
        MessageScrollState::WAIT_AT_END)
    {
        if (now - messageScrollTimer >= 1000)
        {
            messageScrollPosition = 0;

            if (xSemaphoreTake(
                    displayMutex,
                    pdMS_TO_TICKS(100)
                ) == pdTRUE)
            {
                display.setCursor(0, 1);

                for (int i = 0; i < COL; i++)
                    display.print(' ');

                display.setCursor(0, 1);

                display.print(
                    message.substring(
                        0,
                        COL
                    )
                );

                xSemaphoreGive(displayMutex);
            }

            // ------------------------------------------------
            // IMPORTANT:
            //
            // We are back at the beginning.
            // Wait another full 1 second before scrolling.
            // ------------------------------------------------

            messageScrollState =
                MessageScrollState::WAIT_AT_START;

            messageScrollTimer = now;
        }
    }
}

void ResetMessageScroll()
{
    messageScrollActive = false;
    messageScrollPosition = 0;
    messageScrollTimer = millis();

    messageScrollState =
        MessageScrollState::WAIT_AT_START;
}

// ============================================================
// SHOW SELECTED MESSAGE
// ============================================================

void ShowSelectedMessage(int messageIndex)
{
    if (messageIndex < 0 ||
        messageIndex >= recvdMessageCount)
    {
        return;
    }

    selectedMessageIndex = messageIndex;

    // --------------------------------------------------------
    // Reset scrolling every time the message is opened.
    // This guarantees:
    //
    //     open message
    //          ↓
    //     start at position 0
    //          ↓
    //     wait 1 second
    //          ↓
    //     start scrolling
    //
    // --------------------------------------------------------

    ResetMessageScroll();

    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        display.clear();

        display.setCursor(0, 0);
        display.print("Displaying Message");

        display.setCursor(0, 1);

        String visibleMessage =
            recvdMessages[messageIndex].substring(
                0,
                COL
            );

        display.print(visibleMessage);

        display.setCursor(0, 2);
        display.print("0) Go Back");

        display.setCursor(0, 3);
        display.print("Option: ");

        xSemaphoreGive(displayMutex);
    }

    SetInputPosition(8, 3);

    // --------------------------------------------------------
    // Only messages longer than 20 characters need scrolling.
    // --------------------------------------------------------

    if (recvdMessages[messageIndex].length() > COL)
    {
        messageScrollActive = true;
        messageScrollTimer = millis();
        messageScrollState =
            MessageScrollState::WAIT_AT_START;
    }
}

// ============================================================
// NEXT PAGE?
// ============================================================

bool HasNextMessagePage()
{
    const int messagesPerPage = 2;

    int nextStartIndex =
        (checkMessagePage + 1)
        * messagesPerPage;

    return nextStartIndex < recvdMessageCount;
}


// ============================================================
// PREVIOUS PAGE?
// ============================================================

bool HasPreviousMessagePage()
{
    return checkMessagePage > 0;
}


// ============================================================
// HANDLE MENU INPUT
// ============================================================

void HandleMenuInput(const String& line)
{
    switch (appState)
    {
        // ====================================================
        // MENU
        // ====================================================

        case AppState::MENU:
        {
            if (line == "1")
            {
                appState = AppState::CHECK_MESSAGE;

                checkMessagePage = 0;

                selectedMessageIndex = -1;

                ShowMessagePage();
            }

            else if (line == "2")
            {
                appState = AppState::SEND_MESSAGE;

                currentInput = "";

                LcdPrint(
                    0,
                    0,
                    "Enter message:",
                    true
                );

                SetInputPosition(0, 1);
            }

            else if (line == "3")
            {
                appState = AppState::CONFIGURE;

                if (receiveTaskHandle != NULL)
                {
                    vTaskSuspend(
                        receiveTaskHandle
                    );
                }

                LcdPrint(
                    0,
                    0,
                    "Configuring...",
                    true
                );

                LcdPrint(
                    0,
                    1,
                    "Check Serial Monitor"
                );

                SetInputPosition(0, 2);

                ConfigureLoRa();
            }

            else
            {
                LcdPrint(
                    0,
                    3,
                    "Invalid option ",
                    true
                );

                delay(800);

                currentInput = "";

                ShowMenu();
            }

            break;
        }


        // ====================================================
        // CHECK MESSAGE
        // ====================================================

        case AppState::CHECK_MESSAGE:
        {
            // ------------------------------------------------
            // Currently viewing a message
            // ------------------------------------------------

            if (selectedMessageIndex >= 0)
            {
                if (line == "0")
                {
                    selectedMessageIndex = -1;

                    currentInput = "";

                    ShowMessagePage();
                }
                else
                {
                    LcdPrint(
                        0,
                        0,
                        "Invalid option",
                        true
                    );

                    delay(800);

                    currentInput = "";

                    ShowSelectedMessage(
                        selectedMessageIndex
                    );
                }

                break;
            }


            // ------------------------------------------------
            // Message list
            // ------------------------------------------------

            // 0 = exit CHECK_MESSAGE

            if (line == "0")
            {
                appState = AppState::MENU;

                checkMessagePage = 0;

                selectedMessageIndex = -1;

                currentInput = "";

                ShowMenu();

                break;
            }


            // ------------------------------------------------
            // Select message 1 or 2
            // ------------------------------------------------

            if (line == "1" || line == "2")
            {
                const int messagesPerPage = 2;

                int messageNumber =
                    (line == "1")
                        ? 1
                        : 2;

                int messageIndex =
                    (checkMessagePage
                     * messagesPerPage)
                    + (messageNumber - 1);

                if (messageIndex >= 0 &&
                    messageIndex < recvdMessageCount)
                {
                    currentInput = "";

                    ShowSelectedMessage(
                        messageIndex
                    );
                }
                else
                {
                    LcdPrint(
                        0,
                        0,
                        "Invalid option",
                        true
                    );

                    delay(800);

                    currentInput = "";

                    ShowMessagePage();
                }

                break;
            }


            // ------------------------------------------------
            // 00 = NEXT
            // ------------------------------------------------

            if (line == "00")
            {
                if (HasNextMessagePage())
                {
                    checkMessagePage++;

                    currentInput = "";

                    ShowMessagePage();
                }
                else
                {
                    LcdPrint(
                        0,
                        0,
                        "Invalid option",
                        true
                    );

                    delay(800);

                    currentInput = "";

                    ShowMessagePage();
                }

                break;
            }


            // ------------------------------------------------
            // 000 = PREVIOUS
            // ------------------------------------------------

            if (line == "000")
            {
                if (HasPreviousMessagePage())
                {
                    checkMessagePage--;

                    currentInput = "";

                    ShowMessagePage();
                }
                else
                {
                    LcdPrint(
                        0,
                        0,
                        "Invalid option",
                        true
                    );

                    delay(800);

                    currentInput = "";

                    ShowMessagePage();
                }

                break;
            }


            // ------------------------------------------------
            // INVALID
            // ------------------------------------------------

            LcdPrint(
                0,
                0,
                "Invalid option",
                true
            );

            delay(800);

            currentInput = "";

            ShowMessagePage();

            break;
        }


        // ====================================================
        // SEND MESSAGE
        // ====================================================

        case AppState::SEND_MESSAGE:
        {
            const int chunkSize =
                sizeof(((LoRaPacket*)0)->payload);

            int totalLen = line.length();

            uint16_t totalPackets =
                (totalLen + chunkSize - 1)
                / chunkSize;
            uint16_t messageID = messageID++;

            if (totalPackets == 0)
                totalPackets = 1;


            if (receiveTaskHandle != NULL)
            {
                vTaskSuspend(
                    receiveTaskHandle
                );
            }


            for (uint16_t i = 0;
                 i < totalPackets;
                 i++)
            {
                LoRaPacket packet{};

                strncpy(
                    packet.magic,
                    "THE GREAT",
                    sizeof(packet.magic)
                );

                packet.totalPackets =
                    totalPackets;

                packet.packetIndex = i;
                packet.messageID = messageID;

                // for to make the receiver ID automatic

                packet.receiverID = (MY_DEVICE_ID == 1) ? 2 : 1;

                int start =
                    i * chunkSize;

                int len =
                    min(
                        chunkSize,
                        totalLen - start
                    );

                packet.payloadLength = len;

                if (len > 0)
                {
                    memcpy(
                        packet.payload,
                        line.c_str() + start,
                        len
                    );
                }

                LoRa.beginPacket();

                LoRa.write(
                    (uint8_t*)&packet,
                    sizeof(packet)
                );

                LoRa.endPacket();

                if (i < totalPackets - 1)
                {
                    delay(25);
                }
            }


            if (receiveTaskHandle != NULL)
            {
                vTaskResume(
                    receiveTaskHandle
                );
            }


            currentInput = "";

            LcdPrint(
                0,
                0,
                "Message sent!",
                true
            );

            delay(200);

            appState = AppState::MENU;

            ShowMenu();

            break;
        }


        // ====================================================
        // CONFIGURE
        // ====================================================

        case AppState::CONFIGURE:
        {
            if (line == "done")
            {
                LcdPrint(
                    0,
                    0,
                    "Config saved",
                    true
                );

                delay(800);

                appState = AppState::MENU;

                currentInput = "";

                if (receiveTaskHandle != NULL)
                {
                    vTaskResume(
                        receiveTaskHandle
                    );
                }

                ShowMenu();
            }

            break;
        }
    }
}


// ============================================================
// RECEIVE / MESSAGE REASSEMBLY
// ============================================================

static uint16_t expectedMessageID = 0;
static uint16_t expectedTotalPackets = 0;
static uint16_t fragmentsReceivedCount = 0;

static String fragmentChunks[64];
static bool fragmentReceived[64] = {false};

static bool messageConstructionActive = false;



void PushRecvdMessage(const String& msg)
{
    // Shift older messages down.
    for (int i = 4; i > 0; i--)
    {
        recvdMessages[i] =
            recvdMessages[i - 1];
    }

    // Newest message is index 0.
    recvdMessages[0] = msg;

    if (recvdMessageCount < 5)
    {
        recvdMessageCount++;
    }
}

void ReceiveTask(void* pvParameters)
{
    for (;;)
    {
        // ========================================================
        // Update notification timer
        // ========================================================
        UpdateMessageNotification();

        // ========================================================
        // Update selected-message scrolling animation
        // ========================================================
        UpdateMessageScroll();

        // ========================================================
        // Check for incoming LoRa packet
        // ========================================================
        int packetSize = LoRa.parsePacket();

        if (packetSize > 0)
        {
            // ----------------------------------------------------
            // Only accept packets with exactly the expected
            // LoRaPacket size.
            // ----------------------------------------------------
            if (packetSize == sizeof(LoRaPacket))
            {
                LoRaPacket packet{};

                int bytesRead =
                    LoRa.readBytes(
                        (uint8_t*)&packet,
                        sizeof(packet)
                    );

                // ------------------------------------------------
                // Make sure the complete structure was received.
                // ------------------------------------------------
                if (bytesRead == sizeof(packet))
                {
                    // =================================================
                    // CHECK MAGIC
                    // =================================================
                    if (strncmp(
                            packet.magic,
                            "THE GREAT",
                            9
                        ) == 0)
                    {
                        // =================================================
                        // CHECK RECEIVER ID
                        //
                        // Only process packets intended for this
                        // specific ESP32.
                        // =================================================
                        if (packet.receiverID == MY_DEVICE_ID)
                        {
                            // =================================================
                            // VALIDATE PACKET
                            // =================================================
                            if (packet.totalPackets > 0 &&
                                packet.totalPackets <= 64 &&
                                packet.packetIndex <
                                    packet.totalPackets &&
                                packet.payloadLength <=
                                    sizeof(packet.payload))
                            {
                                // =================================================
                                // START NEW MESSAGE CONSTRUCTION
                                //
                                // The first valid packet we receive becomes
                                // the message currently being constructed.
                                //
                                // It does NOT have to be packetIndex == 0.
                                // Therefore packets can arrive out of order.
                                // =================================================
                                if (!messageConstructionActive)
                                {
                                    expectedMessageID =
                                        packet.messageID;

                                    expectedTotalPackets =
                                        packet.totalPackets;

                                    fragmentsReceivedCount = 0;

                                    // Clear previous construction data.
                                    for (int i = 0;
                                         i < 64;
                                         i++)
                                    {
                                        fragmentReceived[i] =
                                            false;

                                        fragmentChunks[i] =
                                            "";
                                    }

                                    messageConstructionActive =
                                        true;
                                }

                                // =================================================
                                // CHECK MESSAGE ID
                                //
                                // The packet must belong to the message that
                                // is currently being reconstructed.
                                // =================================================
                                if (packet.messageID ==
                                    expectedMessageID)
                                {
                                    // =================================================
                                    // CHECK TOTAL PACKET COUNT
                                    //
                                    // Prevent packets from another message
                                    // with the same message ID but a different
                                    // packet count from being mixed in.
                                    // =================================================
                                    if (packet.totalPackets ==
                                        expectedTotalPackets)
                                    {
                                        // =============================================
                                        // STORE FRAGMENT ONLY ONCE
                                        // =============================================
                                        if (!fragmentReceived[
                                                packet.packetIndex])
                                        {
                                            String fragment = "";

                                            // -----------------------------------------
                                            // Copy exactly payloadLength characters.
                                            // Do not depend on null termination.
                                            // -----------------------------------------
                                            for (uint8_t i = 0;
                                                 i <
                                                 packet.payloadLength;
                                                 i++)
                                            {
                                                fragment +=
                                                    packet.payload[i];
                                            }

                                            fragmentChunks[
                                                packet.packetIndex
                                            ] = fragment;

                                            fragmentReceived[
                                                packet.packetIndex
                                            ] = true;

                                            fragmentsReceivedCount++;
                                        }

                                        // =============================================
                                        // CHECK FOR COMPLETE MESSAGE
                                        // =============================================
                                        if (fragmentsReceivedCount ==
                                            expectedTotalPackets)
                                        {
                                            String fullMessage = "";

                                            // -----------------------------------------
                                            // Reconstruct message in packet order.
                                            // -----------------------------------------
                                            for (uint16_t i = 0;
                                                 i <
                                                 expectedTotalPackets;
                                                 i++)
                                            {
                                                fullMessage +=
                                                    fragmentChunks[i];
                                            }

                                            // -----------------------------------------
                                            // Store completed message.
                                            // -----------------------------------------
                                            PushRecvdMessage(
                                                fullMessage
                                            );

                                            // -----------------------------------------
                                            // Show "Message Received".
                                            // -----------------------------------------
                                            StartMessageNotification();

                                            // =========================================
                                            // RESET MESSAGE CONSTRUCTION STATE
                                            // =========================================
                                            messageConstructionActive =
                                                false;

                                            expectedMessageID = 0;

                                            expectedTotalPackets = 0;

                                            fragmentsReceivedCount = 0;

                                            for (int i = 0;
                                                 i < 64;
                                                 i++)
                                            {
                                                fragmentReceived[i] =
                                                    false;

                                                fragmentChunks[i] =
                                                    "";
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                // ----------------------------------------------------
                // Packet is not our expected structure size.
                // Discard any remaining bytes from this packet.
                // ----------------------------------------------------
                while (LoRa.available())
                {
                    LoRa.read();
                }
            }
        }

        // ========================================================
        // Keep task from running continuously at full CPU speed.
        // ========================================================
        vTaskDelay(
            pdMS_TO_TICKS(1)
        );
    }
}

// void ReceiveTask(void* pvParameters)
// {
//     for (;;)
//     {
//         // Notification timer.
//         UpdateMessageNotification();

//         // Selected-message scrolling animation.
//         UpdateMessageScroll();


//         int packetSize =
//             LoRa.parsePacket();


//         if (packetSize > 0)
//         {
//             LoRaPacket packet{};

//             int bytesRead =
//                 LoRa.readBytes(
//                     (uint8_t*)&packet,
//                     sizeof(packet)
//                 );


//             if (bytesRead == sizeof(packet))
//             {
//                 // ------------------------------------------------
//                 // Check magic
//                 // ------------------------------------------------

//                 if (strncmp(
//                         packet.magic,
//                         "THE GREAT",
//                         9
//                     ) == 0)
//                 {
//                     if (packet.receiverID != MY_DEVICE_ID) {
//                         continue;
//                     }
//                     // ------------------------------------------------
//                     // Validate packet
//                     // ------------------------------------------------

//                     if (packet.totalPackets > 0 &&
//                         packet.totalPackets <= 64 &&
//                         packet.packetIndex < packet.totalPackets &&
//                         packet.payloadLength <=
//                             sizeof(packet.payload))
//                     {
//                         // ------------------------------------------------
//                         // New message
//                         // ------------------------------------------------

//                         if (packet.packetIndex == 0)
//                         {
//                             expectedTotalPackets =
//                                 packet.totalPackets;

//                             fragmentsReceivedCount = 0;

//                             for (int i = 0;
//                                  i < 64;
//                                  i++)
//                             {
//                                 fragmentReceived[i] =
//                                     false;

//                                 fragmentChunks[i] =
//                                     "";
//                             }
//                         }


//                         // ------------------------------------------------
//                         // Only accept packets belonging to current
//                         // message.
//                         // ------------------------------------------------

//                         if (packet.totalPackets ==
//                             expectedTotalPackets)
//                         {
//                             // --------------------------------------------
//                             // Store fragment only once.
//                             // --------------------------------------------

//                             if (!fragmentReceived[
//                                     packet.packetIndex])
//                             {
//                                 String fragment = "";

//                                 for (uint8_t i = 0;
//                                      i < packet.payloadLength;
//                                      i++)
//                                 {
//                                     fragment +=
//                                         packet.payload[i];
//                                 }

//                                 fragmentChunks[
//                                     packet.packetIndex
//                                 ] = fragment;

//                                 fragmentReceived[
//                                     packet.packetIndex
//                                 ] = true;

//                                 fragmentsReceivedCount++;
//                             }


//                             // --------------------------------------------
//                             // Complete message
//                             // --------------------------------------------

//                             if (fragmentsReceivedCount ==
//                                 expectedTotalPackets)
//                             {
//                                 String fullMessage = "";

//                                 for (uint16_t i = 0;
//                                      i < expectedTotalPackets;
//                                      i++)
//                                 {
//                                     fullMessage +=
//                                         fragmentChunks[i];
//                                 }


//                                 PushRecvdMessage(
//                                     fullMessage
//                                 );


//                                 StartMessageNotification();


//                                 expectedTotalPackets = 0;

//                                 fragmentsReceivedCount = 0;


//                                 for (int i = 0;
//                                      i < 64;
//                                      i++)
//                                 {
//                                     fragmentReceived[i] =
//                                         false;

//                                     fragmentChunks[i] =
//                                         "";
//                                 }
//                             }
//                         }
//                     }
//                 }
//             }
//         }


//         // MUST remain inside the loop.
//         vTaskDelay(
//             pdMS_TO_TICKS(1)
//         );
//     }
// }
