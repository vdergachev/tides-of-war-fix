#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "log.h"

static FILE *g_log;
static SRWLOCK g_log_lock = SRWLOCK_INIT;

int log_open(const char *path)
{
    return fopen_s(&g_log, path, "w") == 0;
}

void log_line(const char *format, ...)
{
    if (!g_log) {
        return;
    }
    AcquireSRWLockExclusive(&g_log_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
    ReleaseSRWLockExclusive(&g_log_lock);
}
