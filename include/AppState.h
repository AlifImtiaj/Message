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

extern int inputCol;
extern int inputRow;
extern int inputMinCol;

extern String currentInput;

extern bool messageNotificationActive;

void ShowMenu();

void HandleMenuInput(const String& line);

void ReceiveTask(void* pvParameters);

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

void StartMessageNotification();

void RestorePreviousScreen();

void UpdateMessageNotification();

void ShowSelectedMessage(int messageIndex);

void ShowMessagePage();

void UpdateMessageScroll();
void ResetMessageScroll();