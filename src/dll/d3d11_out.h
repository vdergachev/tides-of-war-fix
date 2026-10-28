#ifndef TOW_D3D11_OUT_H
#define TOW_D3D11_OUT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void d3d11_out_configure(const char *dir);
int d3d11_out_enabled(void);
int d3d11_out_owns(HWND hwnd);
int d3d11_out_frame(HWND destination, HDC source, int factor);
void d3d11_out_hide(void);
int d3d11_out_begin_gdi_refresh(void);
void d3d11_out_end_gdi_refresh(int ok);
void d3d11_out_window(HWND hwnd, UINT message);

#ifdef __cplusplus
}
#endif

#endif
