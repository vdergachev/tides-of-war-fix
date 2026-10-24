#ifndef TOW_GAME_H
#define TOW_GAME_H
#include <windows.h>

/* Supported image and imported MFC CString constructor. */
#define GAME_IMAGE_BASE      0x400000
#define CSTRING_CTOR_ADDRESS 0x49443c
int game_cstring_check(void);
void *game_cstring_construct(void *out, const char *text);
#endif
