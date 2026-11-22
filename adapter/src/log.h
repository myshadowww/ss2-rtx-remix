// ss2rtx adapter log. Thread-safe, line-buffered, written next to the game executable.
#pragma once
#include <windows.h>

namespace ss2rtx {

void log_open();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
double now_ms();

}  // namespace ss2rtx
