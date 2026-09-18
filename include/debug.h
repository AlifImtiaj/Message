#pragma once

#define DEBUG_BUILD

#ifdef DEBUG_BUILD

#define DEBUG_PRINT(x)       Serial.print(x)
#define DEBUG_PRINTLN(x)     Serial.println(x)
#define DEBUG_PRINTF(...)    Serial.printf(__VA_ARGS__)

#else

#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTF(...)

#endif