#include "frame_queue.h"
#include <string.h>
#include <stdint.h>
#include <cstdlib>
/* Supported image, Draw_CDibLayout and CGameMenuWnd (IDA 0x42a37e/0x433340). */
#define FRAME_DIB_ADDRESS   0x4cde34
#define DRAW_STATE_ADDRESS  0x4cde48
#define MENU_WINDOW_ADDRESS 0x4ce064
#define MENU_DIB_OFFSET     0x48
#define DIB_WIDTH_OFFSET    8
#define DIB_HEIGHT_OFFSET   0x0c
#define DIB_RAW_OFFSET      0x1c
#define DIB_HDC_OFFSET      0x24
#define DIB_BPP_OFFSET      0x42
#define DIB_ROW_OFFSET      0x60
#define DIB_STRIDE_OFFSET   0x64

LONG FrameQueue::atomic_read(LONG *value)
{
    return InterlockedCompareExchange(value, 0, 0);
}

int FrameQueue::screen_supported(void)
{
    int state = *(const int *)(uintptr_t)DRAW_STATE_ADDRESS;
    return (state >= 1 && state <= 4) || state == 8 || state == 9;
}

BYTE *FrameQueue::source_dib(HDC source)
{
    BYTE *dib = *(BYTE **)(uintptr_t)FRAME_DIB_ADDRESS;
    if (dib && *(HDC *)(dib + DIB_HDC_OFFSET) == source) {
        return dib;
    }
    BYTE *menu = *(BYTE **)(uintptr_t)MENU_WINDOW_ADDRESS;
    if (menu && *(int *)(uintptr_t)DRAW_STATE_ADDRESS == 1) {
        dib = menu + MENU_DIB_OFFSET;
        if (*(HDC *)(dib + DIB_HDC_OFFSET) == source) {
            return dib;
        }
    }
    return NULL;
}

int FrameQueue::copy_pixels(HDC source, int smooth)
{
    int result = 0;
    __try {
        BYTE *dib = source_dib(source);
        if (!dib || *(int *)(dib + DIB_WIDTH_OFFSET) != FRAME_WIDTH ||
            *(int *)(dib + DIB_HEIGHT_OFFSET) != FRAME_HEIGHT || *(WORD *)(dib + DIB_BPP_OFFSET) != 16 ||
            !*(BYTE **)(dib + DIB_RAW_OFFSET)) {
            __leave;
        }
        const BYTE *row = *(const BYTE **)(dib + DIB_ROW_OFFSET);
        int stride = *(int *)(dib + DIB_STRIDE_OFFSET);
        if (!row || (stride != FRAME_WIDTH * 2 && stride != -FRAME_WIDTH * 2)) {
            __leave;
        }
        /* Complete GDI writes to this thread's DIB before reading its CPU pixels. */
        GdiFlush();
        for (int y = 0; y < FRAME_HEIGHT; ++y) {
            memcpy(capture_ + y * FRAME_WIDTH * 2, row + y * stride, FRAME_WIDTH * 2);
        }
        if (smooth) {
            scene_present(source, atomic_read(&epoch_), serial_ + 1);
        }
        /* Only the producer exchanges these pointers; the worker reads under lock_.
         * Identical menu/map paints need neither a new upload nor another Present.
         * Mission records must still advance even when their final pixels match. */
        int state = *(const int *)(uintptr_t)DRAW_STATE_ADDRESS;
        if (state != 2 && state != 3 && serial_ && latest_epoch_ == atomic_read(&epoch_) &&
            memcmp(capture_, latest_, sizeof(snapshot565_)) == 0) {
            InterlockedIncrement(&received_);
            return 1;
        }
        diag_frame diagnostic;
        if (diag_enabled(DIAG_TRANSITIONS) || diag_enabled(DIAG_UI)) {
            diagnostic.state = state;
            diagnostic.dib = dib;
            RECT menu = {160, 32, 480, 112};
            RECT panel = {0, 128, 128, 480};
            diagnostic.menu_hash = diag_hash565((const WORD *)capture_, FRAME_WIDTH * 2, menu);
            diagnostic.ui_hash = diag_hash565((const WORD *)capture_, FRAME_WIDTH * 2, panel);
        }
        /* Publish only a complete capture, including after a guarded read fails.
         * Pixel copying itself never holds the consumer's lock. */
        AcquireSRWLockExclusive(&lock_);
        BYTE *previous = latest_;
        latest_ = capture_;
        capture_ = previous;
        latest_epoch_ = atomic_read(&epoch_);
        ++serial_;
        if (diag_enabled(DIAG_TRANSITIONS) || diag_enabled(DIAG_UI)) {
            diagnostic.epoch = latest_epoch_;
            diagnostic.serial = serial_;
            diag_latest_ = diagnostic;
        }
        ReleaseSRWLockExclusive(&lock_);
        if (diag_enabled(DIAG_TRANSITIONS)) {
            diag_transition("capture", &diagnostic, -1, S_OK);
        }
        InterlockedIncrement(&received_);
        result = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
    }
    return result;
}

int FrameQueue::initialize()
{
    if (ship_hd_render_scale() && !worker_record_.hd) {
        worker_record_.hd = static_cast<hd_live_record *>(calloc(1, sizeof(hd_live_record)));
    }
    return scene_init(&lock_);
}

int FrameQueue::take()
{
    return scene_take(&input_);
}

FrameQueue::Snapshot FrameQueue::consume(LONG &snapshot_serial, LONG &snapshot_epoch, LONG scene_epoch,
                                         int scene_active)
{
    AcquireSRWLockShared(&lock_);
    Snapshot result = {serial_, latest_epoch_, 0, scene_active && scene_epoch == latest_epoch_};
    if (result.epoch == atomic_read(&epoch_) && result.serial != snapshot_serial) {
        memcpy(snapshot565_, latest_, sizeof(snapshot565_));
        if (diag_enabled(DIAG_TRANSITIONS) || diag_enabled(DIAG_UI)) {
            diag_snapshot_ = diag_latest_;
        }
        snapshot_serial = result.serial;
        snapshot_epoch = result.epoch;
        result.copied = 1;
    }
    ReleaseSRWLockShared(&lock_);
    return result;
}

void FrameQueue::prepare()
{
    if (latest_) {
        return;
    }
    latest_ = buffers_[0];
    capture_ = buffers_[1];
    input_ = &worker_record_;
    epoch_ = 1;
}
