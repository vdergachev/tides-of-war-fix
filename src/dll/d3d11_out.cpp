#include "d3d11_out.h"
#include "output_window.h"
#include <type_traits>

/* Trivial UI/mailbox state only. The RAII root lives on the render worker. */
static_assert(std::is_trivially_destructible<OutputWindow>::value, "No static render destructors");
static_assert(std::is_trivially_default_constructible<OutputWindow>::value,
              "Shared state must stay in zero-initialized storage");
static OutputWindow output;

void d3d11_out_hide(void)
{
    output.hide();
}

int d3d11_out_begin_gdi_refresh(void)
{
    return output.begin_gdi_refresh(0);
}

void d3d11_out_end_gdi_refresh(int ok)
{
    output.end_gdi_refresh(ok);
}

int d3d11_out_frame(HWND destination, HDC source, int factor)
{
    return output.frame(destination, source, factor);
}

void d3d11_out_window(HWND hwnd, UINT message)
{
    output.window(hwnd, message);
}

int d3d11_out_owns(HWND hwnd)
{
    return output.owns(hwnd);
}

int d3d11_out_enabled(void)
{
    return output.enabled();
}

void d3d11_out_configure(const char *dir)
{
    output.configure(dir);
}
