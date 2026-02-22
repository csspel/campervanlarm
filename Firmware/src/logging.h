#pragma once
#include <Arduino.h>

void loggingInit();
void logSystem(const String &msg);
void loggingFlush(uint32_t budgetMs, uint16_t maxLines);
void loggingFlushIdle();
