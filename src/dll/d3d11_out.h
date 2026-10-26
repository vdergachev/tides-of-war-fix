#ifndef TOW_D3D11_OUT_H
#define TOW_D3D11_OUT_H

#include <windows.h>

void d3d11_out_configure(const char *dir);
int d3d11_out_enabled(void);
int d3d11_out_owns(HWND hwnd);
int d3d11_out_frame(HWND destination, HDC source, int factor);
void d3d11_out_hide(void);
void d3d11_out_window(HWND hwnd, UINT message);

#endif
