#include "AppState.h"

#include <LoRa.h>
#include <LiquidCrystal_I2C.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "LoraConfig.h"
#include "debug.h"

#define ROW 4
#define COL 20

extern LiquidCrystal_I2C display;

Preferences prefs;

bool isThereNewMessage = false;

// ============================================================
// APPLICATION STATE
// ============================================================

AppState appState = AppState::MENU;

TaskHandle_t receiveTaskHandle = NULL;

SemaphoreHandle_t displayMutex = NULL;
SemaphoreHandle_t storageMutex = NULL;

// ============================================================
// SERIAL INPUT
// ============================================================

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

int notificationPreviousMessagePage = 0;

String notificationPreviousSelectedMessageText = "";

// ============================================================
// MESSAGE UI
// ============================================================

int checkMessagePage = 0;

int selectedMessageIndex = -1;

// ============================================================
// SELECTED MESSAGE CACHE
// ============================================================
//
// Only the message currently being viewed is kept in RAM.
//
// The complete message history remains in LittleFS.
//

String selectedMessageText = "";

// ============================================================
// MESSAGE SCROLLING
// ============================================================

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

// ============================================================
// LITTLEFS MESSAGE STORAGE
// ============================================================
//
// Messages are stored in chronological order:
//
//     oldest
//       |
//       v
//     A
//     B
//     C
//     D
//       ^
//       |
//     newest
//
// But the UI presents them newest-first:
//
//     message index 0 -> D
//     message index 1 -> C
//     message index 2 -> B
//     message index 3 -> A
//
// Therefore we can APPEND new messages without rewriting the
// entire filesystem every time a message arrives.
//

static const char* MESSAGE_FILE = "/messages.dat";

static const uint8_t MESSAGE_MAGIC[4] = {
    'M',
    'S',
    'G',
    '1'
};

static size_t storedMessageCount = 0;

// ============================================================
// MESSAGE ID
// ============================================================

static uint16_t nextMessageID = 0;

// ============================================================
// STORAGE HELPERS
// ============================================================

static bool StorageLock()
{
    if (storageMutex == NULL)
        return false;

    return xSemaphoreTake(
               storageMutex,
               pdMS_TO_TICKS(1000)
           ) == pdTRUE;
}

static void StorageUnlock()
{
    if (storageMutex != NULL)
    {
        xSemaphoreGive(storageMutex);
    }
}

// ------------------------------------------------------------
// Write exactly all requested bytes.
// ------------------------------------------------------------

static bool WriteAll(
    File& file,
    const uint8_t* data,
    size_t length
)
{
    size_t written = 0;

    while (written < length)
    {
        size_t result = file.write(
            data + written,
            length - written
        );

        if (result == 0)
            return false;

        written += result;
    }

    return true;
}

// ------------------------------------------------------------
// Read exactly all requested bytes.
// ------------------------------------------------------------

static bool ReadAll(
    File& file,
    uint8_t* data,
    size_t length
)
{
    size_t received = 0;

    while (received < length)
    {
        size_t result = file.read(
            data + received,
            length - received
        );

        if (result == 0)
            return false;

        received += result;
    }

    return true;
}

// ------------------------------------------------------------
// Read one message record header.
//
// Record format:
//
//     4 bytes  -> "MSG1"
//     4 bytes  -> message length
//     N bytes  -> message data
//
// The uint32_t length is stored in ESP32 native
// little-endian representation.
//

static bool ReadRecordHeader(
    File& file,
    uint32_t& messageLength
)
{
    uint8_t header[8];

    if (!ReadAll(file, header, sizeof(header)))
        return false;

    if (memcmp(
            header,
            MESSAGE_MAGIC,
            4
        ) != 0)
    {
        return false;
    }

    memcpy(
        &messageLength,
        header + 4,
        sizeof(uint32_t)
    );

    size_t remaining =
        file.size() - file.position();

    if (messageLength > remaining)
        return false;

    return true;
}

// ------------------------------------------------------------
// Skip a number of bytes safely.
//

static bool SkipBytes(
    File& file,
    uint32_t length
)
{
    uint8_t buffer[128];

    uint32_t remaining = length;

    while (remaining > 0)
    {
        size_t chunk =
            (remaining > sizeof(buffer))
                ? sizeof(buffer)
                : remaining;

        if (!ReadAll(
                file,
                buffer,
                chunk
            ))
        {
            return false;
        }

        remaining -= chunk;
    }

    return true;
}

// ------------------------------------------------------------
// Read message contents from the current file position.
//

static bool ReadMessageData(
    File& file,
    uint32_t length,
    String& message
)
{
    message = "";

    if (length == 0)
        return true;

    message.reserve(length);

    char buffer[128];

    uint32_t remaining = length;

    while (remaining > 0)
    {
        size_t chunk =
            (remaining > sizeof(buffer))
                ? sizeof(buffer)
                : remaining;

        size_t received =
            file.read(
                reinterpret_cast<uint8_t*>(buffer),
                chunk
            );

        if (received == 0)
            return false;

        for (size_t i = 0; i < received; i++)
        {
            message += buffer[i];
        }

        remaining -= received;
    }

    return true;
}

// ------------------------------------------------------------
// Scan the entire message file.
//
// This validates the complete file and counts its records.
//

static bool ScanMessageFile(
    size_t& count
)
{
    count = 0;

    if (!LittleFS.exists(MESSAGE_FILE))
        return true;

    File file = LittleFS.open(
        MESSAGE_FILE,
        "r"
    );

    if (!file)
        return false;

    while (file.position() < file.size())
    {
        uint32_t messageLength = 0;

        if (!ReadRecordHeader(
                file,
                messageLength
            ))
        {
            file.close();
            return false;
        }

        if (!SkipBytes(
                file,
                messageLength
            ))
        {
            file.close();
            return false;
        }

        count++;
    }

    file.close();

    return true;
}

// ============================================================
// INITIALIZE LITTLEFS
// ============================================================

bool InitializeMessageStorage()
{
    if (storageMutex == NULL)
    {
        storageMutex =
            xSemaphoreCreateMutex();

        if (storageMutex == NULL)
        {
            return false;
        }
    }

    if (!LittleFS.begin(false, "/littlefs", 10, "littlefs"))
    {
        Serial.println(
            "ERROR: LittleFS mount failed."
        );

        return false;
    }

    if (!LittleFS.exists(MESSAGE_FILE))
    {
        File file = LittleFS.open(
            MESSAGE_FILE,
            "w"
        );

        if (!file)
        {
            Serial.println(
                "ERROR: Cannot create message file."
            );

            return false;
        }

        file.close();

        storedMessageCount = 0;

        Serial.println(
            "LittleFS message storage created."
        );

        return true;
    }

    size_t count = 0;

    if (!ScanMessageFile(count))
    {
        Serial.println(
            "ERROR: Message file is corrupted."
        );

        return false;
    }

    storedMessageCount = count;

    Serial.print(
        "LittleFS messages: "
    );

    Serial.println(
        storedMessageCount
    );

    return true;
}

// ============================================================
// GET MESSAGE COUNT
// ============================================================

size_t GetStoredMessageCount()
{
    if (!StorageLock())
        return 0;

    size_t count =
        storedMessageCount;

    StorageUnlock();

    return count;
}

// ============================================================
// SAVE RECEIVED MESSAGE
// ============================================================
//
// New messages are APPENDED.
//
// This means receiving a message does not require rewriting
// the complete message database.
//

bool SaveReceivedMessage(
    const String& message
)
{
    if (!StorageLock())
        return false;

    File file = LittleFS.open(
        MESSAGE_FILE,
        "a"
    );

    if (!file)
    {
        StorageUnlock();
        return false;
    }

    uint32_t messageLength =
        static_cast<uint32_t>(
            message.length()
        );

    bool success = true;

    // Write magic.
    if (!WriteAll(
            file,
            MESSAGE_MAGIC,
            sizeof(MESSAGE_MAGIC)
        ))
    {
        success = false;
    }

    // Write message length.
    if (success)
    {
        if (!WriteAll(
                file,
                reinterpret_cast<const uint8_t*>(
                    &messageLength
                ),
                sizeof(messageLength)
            ))
        {
            success = false;
        }
    }

    // Write message contents.
    if (success && messageLength > 0)
    {
        if (!WriteAll(
                file,
                reinterpret_cast<const uint8_t*>(
                    message.c_str()
                ),
                messageLength
            ))
        {
            success = false;
        }
    }

    file.close();

    if (success)
    {
        storedMessageCount++;

        Serial.print(
            "Message saved. Total messages: "
        );

        Serial.println(
            storedMessageCount
        );
    }

    StorageUnlock();

    return success;
}

// ============================================================
// READ STORED MESSAGE
// ============================================================
//
// messageIndex is the UI index:
//
//     0 = newest
//     1 = second newest
//     2 = third newest
//     ...
//
// The physical file is stored oldest -> newest, so the
// physical index is calculated accordingly.
//

bool ReadStoredMessage(
    size_t messageIndex,
    String& message
)
{
    message = "";

    if (!StorageLock())
        return false;

    if (messageIndex >= storedMessageCount)
    {
        StorageUnlock();
        return false;
    }

    File file = LittleFS.open(
        MESSAGE_FILE,
        "r"
    );

    if (!file)
    {
        StorageUnlock();
        return false;
    }

    size_t physicalIndex =
        storedMessageCount -
        1 -
        messageIndex;

    bool success = false;

    for (size_t i = 0;
         i < storedMessageCount;
         i++)
    {
        uint32_t messageLength = 0;

        if (!ReadRecordHeader(
                file,
                messageLength
            ))
        {
            break;
        }

        if (i == physicalIndex)
        {
            success =
                ReadMessageData(
                    file,
                    messageLength,
                    message
                );

            break;
        }

        if (!SkipBytes(
                file,
                messageLength
            ))
        {
            break;
        }
    }

    file.close();

    StorageUnlock();

    return success;
}

// ============================================================
// FIND MESSAGE
// ============================================================
//
// Used when restoring a message after the temporary
// "Message Received" notification.
//
// Search is newest-first, matching the UI ordering.
//

int FindStoredMessage(
    const String& target
)
{
    if (!StorageLock())
        return -1;

    File file = LittleFS.open(
        MESSAGE_FILE,
        "r"
    );

    if (!file)
    {
        StorageUnlock();
        return -1;
    }

    int result = -1;

    for (size_t physicalIndex = 0;
         physicalIndex < storedMessageCount;
         physicalIndex++)
    {
        uint32_t messageLength = 0;

        if (!ReadRecordHeader(
                file,
                messageLength
            ))
        {
            break;
        }

        String message;

        if (!ReadMessageData(
                file,
                messageLength,
                message
            ))
        {
            break;
        }

        if (message == target)
        {
            // Convert physical index into newest-first
            // logical index.
            result =
                static_cast<int>(
                    storedMessageCount -
                    1 -
                    physicalIndex
                );

            break;
        }
    }

    file.close();

    StorageUnlock();

    return result;
}

// ============================================================
// DELETE STORED MESSAGE
// ============================================================
//
// Deletes a logical UI index.
//
// Example:
//
// UI:
//     0 = E
//     1 = D
//     2 = C
//     3 = B
//     4 = A
//
// Delete index 2:
//
// Result:
//     0 = E
//     1 = D
//     2 = B
//     3 = A
//
// No gaps remain.
//

bool DeleteStoredMessage(
    size_t messageIndex
)
{
    if (!StorageLock())
        return false;

    if (messageIndex >= storedMessageCount)
    {
        StorageUnlock();
        return false;
    }

    File source = LittleFS.open(
        MESSAGE_FILE,
        "r"
    );

    if (!source)
    {
        StorageUnlock();
        return false;
    }

    const char* tempFileName =
        "/messages.tmp";

    const char* backupFileName =
        "/messages.bak";

    // // Remove leftovers from a previous failed operation.
    // if (LittleFS.exists(tempFileName))
    //     LittleFS.remove(tempFileName);

    // if (LittleFS.exists(backupFileName))
    //     LittleFS.remove(backupFileName);

    File temp = LittleFS.open(
        tempFileName,
        "w"
    );

    if (!temp)
    {
        source.close();
        StorageUnlock();
        return false;
    }

    size_t physicalDeleteIndex =
        storedMessageCount -
        1 -
        messageIndex;

    bool success = true;

    for (size_t physicalIndex = 0;
         physicalIndex < storedMessageCount;
         physicalIndex++)
    {
        uint8_t header[8];

        if (!ReadAll(
                source,
                header,
                sizeof(header)
            ))
        {
            success = false;
            break;
        }

        if (memcmp(
                header,
                MESSAGE_MAGIC,
                4
            ) != 0)
        {
            success = false;
            break;
        }

        uint32_t messageLength = 0;

        memcpy(
            &messageLength,
            header + 4,
            sizeof(messageLength)
        );

        size_t remaining =
            source.size() -
            source.position();

        if (messageLength > remaining)
        {
            success = false;
            break;
        }

        // This is the message we are deleting.
        if (physicalIndex ==
            physicalDeleteIndex)
        {
            if (!SkipBytes(
                    source,
                    messageLength
                ))
            {
                success = false;
                break;
            }

            continue;
        }

        // Copy the record header.
        if (!WriteAll(
                temp,
                header,
                sizeof(header)
            ))
        {
            success = false;
            break;
        }

        // Copy the message contents.
        uint8_t buffer[128];

        uint32_t bytesRemaining =
            messageLength;

        while (bytesRemaining > 0)
        {
            size_t chunk =
                (bytesRemaining > sizeof(buffer))
                    ? sizeof(buffer)
                    : bytesRemaining;

            if (!ReadAll(
                    source,
                    buffer,
                    chunk
                ))
            {
                success = false;
                break;
            }

            if (!WriteAll(
                    temp,
                    buffer,
                    chunk
                ))
            {
                success = false;
                break;
            }

            bytesRemaining -= chunk;
        }

        if (!success)
            break;
    }

    source.close();
    temp.close();

    if (!success)
    {
        LittleFS.remove(tempFileName);
        StorageUnlock();
        return false;
    }

    // --------------------------------------------------------
    // Replace original file safely.
    //
    // Original:
    //     messages.dat
    //
    // Temporary:
    //     messages.tmp
    //
    // Backup:
    //     messages.bak
    // --------------------------------------------------------

    if (!LittleFS.rename(
            MESSAGE_FILE,
            backupFileName
        ))
    {
        LittleFS.remove(tempFileName);
        StorageUnlock();
        return false;
    }

    if (!LittleFS.rename(
            tempFileName,
            MESSAGE_FILE
        ))
    {
        // Try to restore original.
        LittleFS.rename(
            backupFileName,
            MESSAGE_FILE
        );

        LittleFS.remove(tempFileName);

        StorageUnlock();
        return false;
    }

    // New file is now active.
    LittleFS.remove(backupFileName);

    storedMessageCount--;

    Serial.print(
        "Message deleted. Total messages: "
    );

    Serial.println(
        storedMessageCount
    );

    StorageUnlock();

    return true;
}

// ============================================================
// MESSAGE ID
// ============================================================

uint16_t AllocateMessageID()
{
    uint16_t id = nextMessageID;

    nextMessageID++;

    return id;
}

// ============================================================
// INPUT POSITION
// ============================================================

void SetInputPosition(
    int col,
    int row
)
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

    // Save current application state.
    notificationPreviousState =
        appState;

    // Save current input.
    notificationPreviousInput =
        currentInput;

    // Save message page.
    notificationPreviousMessagePage =
        checkMessagePage;

    // Save the actual selected message.
    notificationPreviousSelectedMessageText =
        "";

    if (appState ==
            AppState::CHECK_MESSAGE &&
        selectedMessageIndex >= 0)
    {
        notificationPreviousSelectedMessageText =
            selectedMessageText;
    }

    // --------------------------------------------------------
    // MENU / CHECK_MESSAGE / CONFIGURE
    //
    // Discard unfinished command.
    //
    // SEND_MESSAGE is different:
    // preserve the message currently being typed.
    // --------------------------------------------------------

    if (appState !=
        AppState::SEND_MESSAGE)
    {
        currentInput = "";
    }

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
            LcdPrint(
                0,2,
                "Press Enter to Send"
            );

            RedrawScrollingInput(
                notificationPreviousInput,
                1
            );

            currentInput =
                notificationPreviousInput;

            int len =
                currentInput.length();

            if (len < COL)
            {
                SetInputPosition(
                    len,
                    1
                );
            }
            else
            {
                SetInputPosition(
                    COL,
                    1
                );
            }

            break;
        }

        // ----------------------------------------------------
        // CHECK MESSAGE
        // ----------------------------------------------------

        case AppState::CHECK_MESSAGE:
        {

            checkMessagePage =
                notificationPreviousMessagePage;

            // User was viewing a message.
            if (notificationPreviousSelectedMessageText.length() >
                0)
            {
                int restoredIndex =
                    FindStoredMessage(
                        notificationPreviousSelectedMessageText
                    );

                if (restoredIndex >= 0)
                {
                    ShowSelectedMessage(
                        restoredIndex
                    );
                }
                else
                {
                    selectedMessageIndex = -1;
                    selectedMessageText = "";

                    ShowMessagePage();
                }
            }
            else
            {
                selectedMessageIndex = -1;
                selectedMessageText = "";

                ShowMessagePage();
            }

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

            SetInputPosition(
                0,
                2
            );

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

    if (millis() -
            messageNotificationStart >=
        2000)
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
    if (displayMutex == NULL)
        return;

    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        if (clearFirst)
            display.clear();

        display.setCursor(
            col,
            row
        );

        display.print(text);

        xSemaphoreGive(
            displayMutex
        );
    }
}

// ============================================================
// LCD ECHO CHARACTER
// ============================================================

void LcdEchoChar(char c)
{
    if (messageNotificationActive)
        return;

    if (displayMutex == NULL)
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

        xSemaphoreGive(
            displayMutex
        );
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
    if (displayMutex == NULL)
        return;

    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        display.setCursor(
            0,
            row
        );

        for (int i = 0; i < COL; i++)
            display.print(' ');

        int len =
            text.length();

        int start =
            (len > COL)
                ? (len - COL)
                : 0;

        String visible =
            text.substring(
                start,
                len
            );

        display.setCursor(
            0,
            row
        );

        display.print(
            visible
        );

        xSemaphoreGive(
            displayMutex
        );
    }
}

// ============================================================
// MAIN MENU
// ============================================================

void ShowMenu()
{
    if (displayMutex == NULL)
        return;

    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        display.clear();

        display.setCursor(
            0,
            0
        );

        display.print(
            "1. Check Message"
        );

        display.setCursor(
            0,
            1
        );

        display.print(
            "2. Send Message"
        );

        display.setCursor(
            0,
            2
        );

        display.print(
            "3. Configure"
        );

        display.setCursor(
            0,
            3
        );

        display.print(
            "Option: "
        );

        xSemaphoreGive(
            displayMutex
        );
    }

    SetInputPosition(
        8,
        3
    );
}

// ============================================================
// MESSAGE PREVIEW
// ============================================================

static String GetMessagePreview(
    const String& message
)
{
    if (message.length() <= 14)
        return message;

    return message.substring(
               0,
               14
           ) +
           "...";
}

// ============================================================
// SHOW MESSAGE PAGE
// ============================================================

void ShowMessagePage()
{
    size_t messageCount =
        GetStoredMessageCount();

    if (messageCount == 0)
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

        SetInputPosition(
            8,
            3
        );

        return;
    }

    const size_t messagesPerPage = 2;

    size_t startIndex =
        static_cast<size_t>(
            checkMessagePage
        ) *
        messagesPerPage;

    // Safety check.
    if (startIndex >= messageCount)
    {
        size_t lastPage =
            (messageCount - 1) /
            messagesPerPage;

        checkMessagePage =
            static_cast<int>(
                lastPage
            );

        startIndex =
            static_cast<size_t>(
                checkMessagePage
            ) *
            messagesPerPage;
    }

    String message1;
    String message2;

    bool hasMessage1 =
        ReadStoredMessage(
            startIndex,
            message1
        );

    bool hasMessage2 = false;

    if (startIndex + 1 <
        messageCount)
    {
        hasMessage2 =
            ReadStoredMessage(
                startIndex + 1,
                message2
            );
    }

    if (displayMutex == NULL)
        return;

    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        display.clear();

        // ----------------------------------------------------
        // Message 1
        // ----------------------------------------------------

        if (hasMessage1)
        {
            display.setCursor(
                0,
                0
            );

            display.print(
                "1. "
            );

            display.print(
                GetMessagePreview(
                    message1
                )
            );
        }

        // ----------------------------------------------------
        // Message 2
        // ----------------------------------------------------

        if (hasMessage2)
        {
            display.setCursor(
                0,
                1
            );

            display.print(
                "2. "
            );

            display.print(
                GetMessagePreview(
                    message2
                )
            );
        }

        // ----------------------------------------------------
        // Navigation
        // ----------------------------------------------------

        display.setCursor(
            0,
            2
        );

        display.print(
            "0.Ex 00.Nxt 000.Bck"
        );

        // ----------------------------------------------------
        // Input
        // ----------------------------------------------------

        display.setCursor(
            0,
            3
        );

        display.print(
            "Option: "
        );

        xSemaphoreGive(
            displayMutex
        );
    }

    SetInputPosition(
        8,
        3
    );
}

// ============================================================
// RESET MESSAGE SCROLL
// ============================================================

void ResetMessageScroll()
{
    messageScrollActive = false;

    messageScrollPosition = 0;

    messageScrollTimer =
        millis();

    messageScrollState =
        MessageScrollState::WAIT_AT_START;
}

// ============================================================
// UPDATE MESSAGE SCROLL
// ============================================================

void UpdateMessageScroll()
{
    if (!messageScrollActive)
        return;

    if (selectedMessageIndex < 0)
        return;

    if (selectedMessageText.length() <= COL)
    {
        ResetMessageScroll();
        return;
    }

    unsigned long now =
        millis();

    // ========================================================
    // WAIT AT START
    // ========================================================

    if (messageScrollState ==
        MessageScrollState::WAIT_AT_START)
    {
        if (now -
                messageScrollTimer >=
            1000)
        {
            messageScrollState =
                MessageScrollState::SCROLLING;

            messageScrollTimer =
                now;
        }

        return;
    }

    // ========================================================
    // SCROLLING
    // ========================================================

    if (messageScrollState ==
        MessageScrollState::SCROLLING)
    {
        if (now -
                messageScrollTimer >=
            250)
        {
            messageScrollTimer =
                now;

            int maxPosition =
                selectedMessageText.length() -
                COL;

            messageScrollPosition++;

            // ------------------------------------------------
            // Reached final 20-character window.
            // ------------------------------------------------

            if (messageScrollPosition >=
                maxPosition)
            {
                messageScrollPosition =
                    maxPosition;

                if (xSemaphoreTake(
                        displayMutex,
                        pdMS_TO_TICKS(100)
                    ) == pdTRUE)
                {
                    display.setCursor(
                        0,
                        1
                    );

                    for (int i = 0;
                         i < COL;
                         i++)
                    {
                        display.print(' ');
                    }

                    display.setCursor(
                        0,
                        1
                    );

                    display.print(
                        selectedMessageText.substring(
                            messageScrollPosition,
                            messageScrollPosition + COL
                        )
                    );

                    xSemaphoreGive(
                        displayMutex
                    );
                }

                messageScrollState =
                    MessageScrollState::WAIT_AT_END;

                messageScrollTimer =
                    now;

                return;
            }

            // ------------------------------------------------
            // Display next scrolling position.
            // ------------------------------------------------

            if (xSemaphoreTake(
                    displayMutex,
                    pdMS_TO_TICKS(100)
                ) == pdTRUE)
            {
                display.setCursor(
                    0,
                    1
                );

                for (int i = 0;
                     i < COL;
                     i++)
                {
                    display.print(' ');
                }

                display.setCursor(
                    0,
                    1
                );

                display.print(
                    selectedMessageText.substring(
                        messageScrollPosition,
                        messageScrollPosition + COL
                    )
                );

                xSemaphoreGive(
                    displayMutex
                );
            }
        }

        return;
    }

    // ========================================================
    // WAIT AT END
    // ========================================================

    if (messageScrollState ==
        MessageScrollState::WAIT_AT_END)
    {
        if (now -
                messageScrollTimer >=
            1000)
        {
            messageScrollPosition =
                0;

            if (xSemaphoreTake(
                    displayMutex,
                    pdMS_TO_TICKS(100)
                ) == pdTRUE)
            {
                display.setCursor(
                    0,
                    1
                );

                for (int i = 0;
                     i < COL;
                     i++)
                {
                    display.print(' ');
                }

                display.setCursor(
                    0,
                    1
                );

                display.print(
                    selectedMessageText.substring(
                        0,
                        COL
                    )
                );

                xSemaphoreGive(
                    displayMutex
                );
            }

            messageScrollState =
                MessageScrollState::WAIT_AT_START;

            messageScrollTimer =
                now;
        }
    }
}

// ============================================================
// SHOW SELECTED MESSAGE
// ============================================================

void ShowSelectedMessage(
    int messageIndex
)
{
    String message;

    if (!ReadStoredMessage(
            static_cast<size_t>(
                messageIndex
            ),
            message
        ))
    {
        selectedMessageIndex = -1;
        selectedMessageText = "";

        LcdPrint(
            0,
            0,
            "Read failed",
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

        SetInputPosition(
            8,
            3
        );

        return;
    }

    selectedMessageIndex =
        messageIndex;

    selectedMessageText =
        message;

    ResetMessageScroll();

    if (xSemaphoreTake(
            displayMutex,
            pdMS_TO_TICKS(100)
        ) == pdTRUE)
    {
        display.clear();

        display.setCursor(
            0,
            0
        );

        display.print(
            "Displaying Message"
        );

        display.setCursor(
            0,
            1
        );

        display.print(
            selectedMessageText.substring(
                0,
                COL
            )
        );

        display.setCursor(
            0,
            2
        );

        display.print(
            "0)Back 00)Delete"
        );

        display.setCursor(
            0,
            3
        );

        display.print(
            "Option: "
        );

        xSemaphoreGive(
            displayMutex
        );
    }

    SetInputPosition(
        8,
        3
    );

    if (selectedMessageText.length() >
        COL)
    {
        messageScrollActive = true;

        messageScrollTimer =
            millis();

        messageScrollState =
            MessageScrollState::WAIT_AT_START;
    }
}

// ============================================================
// NEXT MESSAGE PAGE
// ============================================================

static bool HasNextMessagePage()
{
    size_t messageCount =
        GetStoredMessageCount();

    const size_t messagesPerPage = 2;

    size_t nextStartIndex =
        static_cast<size_t>(
            checkMessagePage + 1
        ) *
        messagesPerPage;

    return nextStartIndex <
           messageCount;
}

// ============================================================
// PREVIOUS MESSAGE PAGE
// ============================================================

static bool HasPreviousMessagePage()
{
    return checkMessagePage > 0;
}

// ============================================================
// HANDLE MENU INPUT
// ============================================================

void HandleMenuInput(
    const String& line
)
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
                appState =
                    AppState::CHECK_MESSAGE;

                checkMessagePage = 0;

                selectedMessageIndex = -1;

                selectedMessageText = "";

                ShowMessagePage();

                isThereNewMessage = false;
                prefs.begin("MsgNot", false);
                prefs.putBool("isNewMsg", false);
                prefs.end();
                digitalWrite(13, LOW);
            }

            else if (line == "2")
            {
                appState =
                    AppState::SEND_MESSAGE;

                currentInput = "";

                LcdPrint(
                    0,
                    0,
                    "Enter message:",
                    true
                );

                LcdPrint(0, 2, "Press Enter to Send");

                SetInputPosition(
                    0,
                    1
                );
            }

            else if (line == "3")
            {
                appState =
                    AppState::CONFIGURE;

                if (receiveTaskHandle != NULL)
                {
                    vTaskSuspend(
                        receiveTaskHandle
                    );
                }

                /*
                * Configuration codes
                */

                // LcdPrint(
                //     0,
                //     0,
                //     "Configuring...",
                //     true
                // );

                // LcdPrint(
                //     0,
                //     1,
                //     "Check Serial Monitor"
                // );

                // SetInputPosition(
                //     0,
                //     2
                // );

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
                // --------------------------------------------
                // 0 = Go Back
                // --------------------------------------------

                if (line == "0")
                {
                    selectedMessageIndex = -1;

                    selectedMessageText = "";

                    currentInput = "";

                    ShowMessagePage();

                    break;
                }

                // --------------------------------------------
                // 00 = DELETE
                // --------------------------------------------

                if (line == "00")
                {
                    int deletedIndex =
                        selectedMessageIndex;

                    if (DeleteStoredMessage(
                            static_cast<size_t>(
                                deletedIndex
                            )
                        ))
                    {
                        selectedMessageIndex = -1;

                        selectedMessageText = "";

                        currentInput = "";

                        // ------------------------------------------------
                        // Make sure the current page still exists.
                        // ------------------------------------------------

                        size_t messageCount =
                            GetStoredMessageCount();

                        if (messageCount == 0)
                        {
                            checkMessagePage = 0;
                        }
                        else
                        {
                            size_t maxPage =
                                (messageCount - 1) /
                                2;

                            if (static_cast<size_t>(
                                    checkMessagePage
                                ) > maxPage)
                            {
                                checkMessagePage =
                                    static_cast<int>(
                                        maxPage
                                    );
                            }
                        }

                        ShowMessagePage();
                    }
                    else
                    {
                        LcdPrint(
                            0,
                            0,
                            "Delete failed",
                            true
                        );

                        delay(800);

                        currentInput = "";

                        ShowSelectedMessage(
                            deletedIndex
                        );
                    }

                    break;
                }

                // --------------------------------------------
                // INVALID OPTION WHILE VIEWING MESSAGE
                // --------------------------------------------

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

                break;
            }

            // ------------------------------------------------
            // Message list
            // ------------------------------------------------

            // 0 = exit CHECK_MESSAGE

            if (line == "0")
            {
                appState =
                    AppState::MENU;

                checkMessagePage = 0;

                selectedMessageIndex = -1;

                selectedMessageText = "";

                currentInput = "";

                ShowMenu();

                break;
            }

            // ------------------------------------------------
            // Select message 1 or 2
            // ------------------------------------------------

            if (line == "1" ||
                line == "2")
            {
                const size_t messagesPerPage = 2;

                int messageNumber =
                    (line == "1")
                        ? 1
                        : 2;

                size_t messageIndex =
                    static_cast<size_t>(
                        checkMessagePage
                    ) *
                    messagesPerPage +
                    static_cast<size_t>(
                        messageNumber - 1
                    );

                size_t messageCount =
                    GetStoredMessageCount();

                if (messageIndex <
                    messageCount)
                {
                    currentInput = "";

                    ShowSelectedMessage(
                        static_cast<int>(
                            messageIndex
                        )
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
                sizeof(
                    ((LoRaPacket*)0)->payload
                );

            int totalLen =
                line.length();

            uint16_t totalPackets =
                (totalLen +
                 chunkSize -
                 1) /
                chunkSize;

            if (totalPackets == 0)
                totalPackets = 1;

            // ------------------------------------------------
            // FIX:
            //
            // The old code had:
            //
            // uint16_t messageID = messageID++;
            //
            // which was incorrect.
            // ------------------------------------------------

            uint16_t messageID =
                AllocateMessageID();

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
                // set the message ID
                packet.messageID =
                    messageID;

                // set the sender id 
                packet.senderID =
                    MY_DEVICE_ID;

                // set the receiver id, in this case, there is only 2 device. so no big issue
                packet.receiverID =
                    (MY_DEVICE_ID == 1)
                        ? 2
                        : 1;

                packet.totalPackets =
                    totalPackets;

                packet.packetIndex =
                    i;

                int start =
                    i * chunkSize;

                int len =
                    min(
                        chunkSize,
                        totalLen - start
                    );

                packet.payloadLength =
                    len;

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
                    reinterpret_cast<uint8_t*>(
                        &packet
                    ),
                    sizeof(packet)
                );

                #ifdef DEBUG_BUILD
                Serial.println("========================================");
                Serial.println("TX PACKET");

                Serial.print("Size = ");
                Serial.print(sizeof(LoRaPacket));
                Serial.println();

                Serial.print("TX RAW: ");

                const uint8_t *raw = reinterpret_cast<const uint8_t *>(&packet);

                for (size_t i = 0; i < sizeof(LoRaPacket); i++)
                {
                    if (raw[i] < 0x10)
                        Serial.print('0');

                    Serial.print(raw[i], HEX);
                    Serial.print(' ');

                    if ((i + 1) % 16 == 0)
                        Serial.println();
                }

                Serial.println();
                Serial.println("========================================");
                #else

                #endif

                LoRa.endPacket();

                if (i <
                    totalPackets - 1)
                {
                    delay(100);
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

            delay(800);

            appState =
                AppState::MENU;

            ShowMenu();

            break;
        }

        // ====================================================
        // CONFIGURE
        // ====================================================

        case AppState::CONFIGURE:
        {
            if (line == "done" || line == "0")
            {
                LcdPrint(
                    0,
                    0,
                    "Config saved",
                    true
                );

                delay(800);

                appState =
                    AppState::MENU;

                currentInput = "";

                if (receiveTaskHandle != NULL)
                {
                    vTaskResume(
                        receiveTaskHandle
                    );
                }

                ShowMenu();

                break;
            }

            if (line == "1")
            {
                currentInput = "";
                SetSpreadingFactor();
                break;
            }

            if (line == "2")
            {
                currentInput = "";
                SetBandwidth();
                break;
            }

            if (line == "3")
            {
                currentInput = "";
                SetTransmissionPower();
                break;
            }

            // ------------------------------------------------
            // INVALID OPTION
            // ------------------------------------------------

            LcdPrint(
                0,
                0,
                "Invalid Option",
                true
            );

            delay(800);

            currentInput = "";

            ConfigureLoRa();

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

static bool fragmentReceived[64] = {
    false
};

static bool messageConstructionActive =
    false;

// ============================================================
// RECEIVE TASK
// ============================================================

void ReceiveTask(
    void* pvParameters
)
{
    for (;;)
    {
        // ====================================================
        // Update notification timer
        // ====================================================

        UpdateMessageNotification();

        // ====================================================
        // Update selected-message scrolling
        // ====================================================

        UpdateMessageScroll();

        // ====================================================
        // Check incoming LoRa packet
        // ====================================================

        int packetSize =
            LoRa.parsePacket();

        if (packetSize > 0)
        {

            // debug code on why this is not receiving packet on higher SF
            DEBUG_PRINT("LoRa packet detected. Size = ");
            DEBUG_PRINT(packetSize);
            DEBUG_PRINT(" / Expected = ");
            DEBUG_PRINTLN(sizeof(LoRaPacket));
            // debug code ends here

            // ------------------------------------------------
            // Only accept our expected packet size.
            // ------------------------------------------------

            if (packetSize ==
                sizeof(LoRaPacket))
            {
                LoRaPacket packet{};

                int bytesRead =
                    LoRa.readBytes(
                        reinterpret_cast<uint8_t*>(
                            &packet
                        ),
                        sizeof(packet)
                    );
                DEBUG_PRINTF(
                "RX: ID=%u From=%u To=%u Fragment=%u/%u Len=%u RSSI=%d SNR=%.1f\n",
                    packet.messageID,
                    packet.senderID,
                    packet.receiverID,
                    packet.packetIndex + 1,
                    packet.totalPackets,
                    packet.payloadLength,
                    LoRa.packetRssi(),
                    LoRa.packetSnr()
                );
                DEBUG_PRINTF(
                    "Bytes read = %d / Expected = %d\n",
                    bytesRead,
                    sizeof(packet)
                );

                DEBUG_PRINT("RAW: ");

                for (int i = 0; i < 30; i++)
                {
                    DEBUG_PRINTF(
                        "%02X ",
                        reinterpret_cast<uint8_t*>(&packet)[i]
                    );
                }

                DEBUG_PRINTLN();

                if (bytesRead ==
                    sizeof(packet))
                {
                    // ============================================
                    // CHECK MAGIC
                    // ============================================

                    if (strncmp(
                            packet.magic,
                            "THE GREAT",
                            9
                        ) == 0)
                    {
                        // ========================================
                        // CHECK RECEIVER ID
                        // ========================================

                        if (packet.receiverID ==
                            MY_DEVICE_ID)
                        {
                            // ====================================
                            // VALIDATE PACKET
                            // ====================================

                            if (
                                packet.totalPackets > 0 &&
                                packet.totalPackets <= 64 &&
                                packet.packetIndex <
                                    packet.totalPackets &&
                                packet.payloadLength <=
                                    sizeof(packet.payload)
                            )
                            {
                                // =================================
                                // START NEW MESSAGE CONSTRUCTION
                                // =================================

                                if (!messageConstructionActive)
                                {
                                    expectedMessageID =
                                        packet.messageID;

                                    expectedTotalPackets =
                                        packet.totalPackets;

                                    fragmentsReceivedCount =
                                        0;

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

                                // =================================
                                // CHECK MESSAGE ID
                                // =================================

                                if (packet.messageID ==
                                    expectedMessageID)
                                {
                                    // =============================
                                    // CHECK TOTAL PACKET COUNT
                                    // =============================

                                    if (packet.totalPackets ==
                                        expectedTotalPackets)
                                    {
                                        // =========================
                                        // STORE FRAGMENT ONCE
                                        // =========================

                                        if (!fragmentReceived[
                                                packet.packetIndex])
                                        {
                                            String fragment =
                                                "";

                                            for (
                                                uint8_t i = 0;
                                                i <
                                                packet.payloadLength;
                                                i++
                                            )
                                            {
                                                fragment +=
                                                    packet.payload[i];
                                            }

                                            fragmentChunks[
                                                packet.packetIndex
                                            ] =
                                                fragment;

                                            fragmentReceived[
                                                packet.packetIndex
                                            ] =
                                                true;

                                            fragmentsReceivedCount++;
                                        }

                                        // =========================
                                        // CHECK COMPLETE MESSAGE
                                        // =========================

                                        if (
                                            fragmentsReceivedCount ==
                                            expectedTotalPackets
                                        )
                                        {
                                            String fullMessage =
                                                "";

                                            // ---------------------
                                            // Reconstruct in order.
                                            // ---------------------

                                            for (
                                                uint16_t i = 0;
                                                i <
                                                expectedTotalPackets;
                                                i++
                                            )
                                            {
                                                fullMessage +=
                                                    fragmentChunks[i];
                                            }

                                            // ---------------------
                                            // IMPORTANT:
                                            //
                                            // Save to LittleFS first.
                                            // Only show "Message
                                            // Received" if storage
                                            // succeeded.
                                            // ---------------------

                                            if (SaveReceivedMessage(
                                                    fullMessage
                                                ))
                                            {
                                                StartMessageNotification();
                                            }
                                            else
                                            {
                                                Serial.println(
                                                    "ERROR: Failed to save received message."
                                                );
                                            }

                                            isThereNewMessage = true;
                                            prefs.begin("MsgNot", false);
                                            prefs.putBool("isNewMsg", true);
                                            prefs.end();
                                            digitalWrite(13, HIGH);

                                            // =====================
                                            // RESET CONSTRUCTION
                                            // =====================

                                            messageConstructionActive =
                                                false;

                                            expectedMessageID =
                                                0;

                                            expectedTotalPackets =
                                                0;

                                            fragmentsReceivedCount =
                                                0;

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
                // debug code
                DEBUG_PRINTLN("ERROR: Packet size mismatch!");
                // debug code end

                // ------------------------------------------------
                // Wrong packet size.
                // Discard remaining bytes.
                // ------------------------------------------------

                while (LoRa.available())
                {
                    LoRa.read();
                }
            }
        }

        // ====================================================
        // Prevent continuous full-speed execution.
        // ====================================================

        vTaskDelay(
            pdMS_TO_TICKS(1)
        );
    }
}