#ifndef TOW_IAT_H
#define TOW_IAT_H
#include <windows.h>

/* Distinct failures preserve the registry guard diagnostics. */
enum { IAT_OK = 1, IAT_UNKNOWN_STATE = 0, IAT_NOT_WRITABLE = -1, IAT_NOT_RESTORED = -2 };

DWORD *iat_find_slot_in(HMODULE module, const char *dll_name, const char *function_name);
DWORD *iat_find_slot(const char *dll_name, const char *function_name);
int iat_replace(DWORD *slot, void *expected, void *replacement);
#endif
