#ifndef TOW_SCALE_H
#define TOW_SCALE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scaling owns factor, target state and mouse mapping in scaling.c. */
void scaling_apply(const char *dir);
int scaling_active(void);
void scaling_set_borderless(int enabled);
/* UI-thread backing-surface refresh, without child invalidation. */
int scaling_refresh_gdi(HWND hwnd, const void *pixels, int factor);

#ifdef __cplusplus
}
#endif

#endif
