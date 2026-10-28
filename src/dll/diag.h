#ifndef TOW_DIAG_H
#define TOW_DIAG_H
#include <windows.h>

enum { DIAG_CURSOR, DIAG_YESNO, DIAG_MASKS, DIAG_UI, DIAG_TRANSITIONS, DIAG_TIMERS, DIAG_COUNT };

typedef struct {
    LONG epoch;
    LONG serial;
    int state;
    const void *dib;
    unsigned menu_hash;
    unsigned ui_hash;
} diag_frame;

#ifdef __cplusplus
extern "C" {
#endif
/* Configured once before hooks are installed. */
extern int g_diag_yesno_enabled;
void diag_log(const char *format, ...);
void diag_configure(const char *dir);
int diag_enabled(unsigned kind);
void diag_line(unsigned kind, const char *format, ...);
unsigned diag_hash565(const WORD *pixels, int pitch, RECT rect);
void diag_transition(const char *phase, const diag_frame *frame, int composed, LONG status);
void diag_blit(HWND hwnd, HDC source, int x, int y, int w, int h, int sx, int sy);
void diag_yesno_window(HWND hwnd);
void diag_yesno_imports(HMODULE module, unsigned index);
void diag_yesno_gdi(const char *api, const void *caller, HDC dc, int x, int y, int w, int h, int sw, int sh,
                    DWORD rop);
void diag_ui_install(void);
void diag_ui_own(unsigned api, const void *caller, HWND hwnd, HWND after, UINT arg, int x, int y, int w,
                 int h);
void diag_ui_scaling(HWND hwnd, HWND before_after, UINT before, HWND after, UINT flags);
void diag_copy_size(int *width, int *height);
#ifdef __cplusplus
}
#endif
#endif
