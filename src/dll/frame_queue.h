#ifndef TOW_FRAME_QUEUE_H
#define TOW_FRAME_QUEUE_H
#include "scene.h"
#include "diag.h"
#define FRAME_WIDTH  640
#define FRAME_HEIGHT 480
#define FRAME_BYTES  (FRAME_WIDTH * FRAME_HEIGHT * 4)

/* UI produces; render thread consumes. The capture ring stays behind scene.c's C API.
 * Only this trivially destructible shared state survives worker shutdown. */
class FrameQueue
{
  public:
    static LONG atomic_read(LONG *value);
    static int screen_supported(void);
    static BYTE *source_dib(HDC source);
    int copy_pixels(HDC source, int smooth);
    FrameQueue() noexcept = default;
    FrameQueue(const FrameQueue &) = delete;
    FrameQueue &operator=(const FrameQueue &) = delete;
    void prepare();
    int take();
    int initialize();

    struct Snapshot {
        LONG serial;
        LONG epoch;
        int copied;
        int scene_ready;
    };

    Snapshot consume(LONG &snapshot_serial, LONG &snapshot_epoch, LONG scene_epoch, int scene_active);

  private:
    friend class OutputWindow;
    friend class Renderer;
    friend class SceneCompositor;
    SRWLOCK lock_;
    BYTE buffers_[2][FRAME_WIDTH * FRAME_HEIGHT * 2];
    BYTE *latest_;
    BYTE *capture_;
    WORD snapshot565_[FRAME_WIDTH * FRAME_HEIGHT];
    LONG epoch_;
    LONG serial_;
    LONG latest_epoch_;
    LONG received_;
    diag_frame diag_latest_;
    diag_frame diag_snapshot_;
    scene_record worker_record_;
    scene_record *input_;
};
#endif
