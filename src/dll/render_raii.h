#ifndef TOW_RENDER_RAII_H
#define TOW_RENDER_RAII_H

#include <windows.h>

/* Noncopyable owners; no allocation and no exceptions. Explicit reset preserves
 * the UI shutdown order: join worker, close thread, then close its events. */
class unique_handle
{
  public:
    unique_handle() noexcept = default;

    explicit unique_handle(HANDLE value) noexcept : value_(value)
    {
    }

    ~unique_handle() noexcept
    {
        reset();
    }

    unique_handle(const unique_handle &) = delete;
    unique_handle &operator=(const unique_handle &) = delete;

    HANDLE get() const noexcept
    {
        return value_;
    }

    void reset(HANDLE value = nullptr) noexcept
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

  private:
    HANDLE value_ = nullptr;
};

class paint_scope
{
  public:
    explicit paint_scope(HWND window) noexcept : window_(window)
    {
        BeginPaint(window_, &paint_);
    }

    ~paint_scope() noexcept
    {
        EndPaint(window_, &paint_);
    }

    paint_scope(const paint_scope &) = delete;
    paint_scope &operator=(const paint_scope &) = delete;

  private:
    HWND window_;
    PAINTSTRUCT paint_ = {};
};

#endif
