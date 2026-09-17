#pragma once

#include <Arduino.h>

enum class AppState {
    MENU,
    CHECK_MESSAGE,
    SEND_MESSAGE,
    CONFIGURE
};

extern AppState appState;

extern TaskHandle_t receiveTaskHandle;

extern SemaphoreHandle_t displayMutex;
extern SemaphoreHandle_t storageMutex;

extern int inputCol;
extern int inputRow;
extern int inputMinCol;

extern String currentInput;

extern bool messageNotificationActive;

// ============================================================
// UI
// ============================================================

void ShowMenu();

void HandleMenuInput(const String& line);

void LcdPrint(
    int col,
    int row,
    const String& text,
    bool clearFirst = false
);

void LcdEchoChar(char c);

void RedrawScrollingInput(
    const String& text,
    int row
);

void SetInputPosition(
    int col,
    int row
);

// ============================================================
// MESSAGE NOTIFICATION
// ============================================================

void StartMessageNotification();

void RestorePreviousScreen();

void UpdateMessageNotification();

// ============================================================
// MESSAGE UI
// ============================================================

void ShowSelectedMessage(int messageIndex);

void ShowMessagePage();

void UpdateMessageScroll();

void ResetMessageScroll();

// ============================================================
// LITTLEFS MESSAGE STORAGE
// ============================================================

bool InitializeMessageStorage();

size_t GetStoredMessageCount();

bool SaveReceivedMessage(const String& message);

bool ReadStoredMessage(
    size_t messageIndex,
    String& message
);

bool DeleteStoredMessage(
    size_t messageIndex
);

int FindStoredMessage(
    const String& message
);

// ============================================================
// MESSAGE ID
// ============================================================

uint16_t AllocateMessageID();

// ============================================================
// RECEIVE TASK
// ============================================================

void ReceiveTask(void* pvParameters);