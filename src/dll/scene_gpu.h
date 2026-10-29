#ifndef TOW_SCENE_GPU_H
#define TOW_SCENE_GPU_H
#include "frame_queue.h"
#include "cloud_art_gpu.h"
#include "texture2d.h"
#include "wake_layer.h"
#include "ship_gpu.h"
using Microsoft::WRL::ComPtr;
#define REGION_PRECLOUD 5
#define REGION_COUNT    6
#define REGION_BOTTOM   0
#define REGION_MID      1
#define REGION_TOP      2
#define REGION_FINAL    3
#define REGION_EDGE     4

struct SceneDiagnostic {
    diag_frame frame;
    unsigned pairs[2][SCENE_SHIPS][SCENE_SHIPS + 1];
};

struct SceneEntry {
    unsigned step;
    unsigned count;
    unsigned follow;
    POINT camera;
    RECT view;
    double qpc;
    scene_ship ships[SCENE_SHIPS];
    unsigned cloud_flags;
    int split_clouds;
    scene_cloud clouds[SCENE_CLOUDS];
    Texture2D regions[REGION_COUNT];
    unsigned draw_count;
    unsigned draw_ship[SCENE_DRAWS];
    unsigned draw_pass[SCENE_DRAWS];
    unsigned draw_additive[SCENE_DRAWS];
    Texture2D draws[SCENE_DRAWS];
    Texture2D hd_draws[SCENE_DRAWS];
    hd_pass hd_passes[SCENE_DRAWS];
    std::unique_ptr<WORD[]> wake_bottom;
};

enum { SNAP_NONE, SNAP_PREV, SNAP_GAP, SNAP_FOLLOW, SNAP_CHANGE, SNAP_JUMP, SNAP_COUNT };

inline constexpr const char *SNAP_NAMES[] = {"none", "no_prev", "gap", "no_follow", "follow_changed", "jump"};

struct SceneResources {
    SceneEntry entries[SCENE_RING];
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11Buffer> placement;
    ComPtr<ID3D11Buffer> base_constants;
    ComPtr<ID3D11PixelShader> base_ps;
    ComPtr<ID3D11PixelShader> layer_ps;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;

    SceneResources() noexcept
        : entries{}, vs(nullptr), placement(nullptr), base_constants(nullptr), base_ps(nullptr),
          layer_ps(nullptr), blend(nullptr), raster(nullptr)
    {
    }
};

/* Render-thread only. Renderer owns this member and explicitly releases GPU state. */
class SceneCompositor
{
  public:
    explicit SceneCompositor(FrameQueue &queue) noexcept : queue_(queue)
    {
    }

    SceneCompositor(const SceneCompositor &) = delete;
    SceneCompositor &operator=(const SceneCompositor &) = delete;

    void release(void);
    int init(ID3D11Device *device);
    void update(ID3D11Device *device, ID3D11DeviceContext *context, LONG epoch, UINT width, UINT height);
    int draw(ID3D11DeviceContext *context, UINT width, UINT height, LONG epoch);
    int active(void);
    void counters(unsigned *draws, unsigned *uploads, unsigned long long *bytes);
    void log(void);
    void frame_begin(void);
    void frame_end(void);

    void plain_frame(void)
    {
        if (stalled_) {
            ++stall_plain_;
        }
    }

    const diag_frame &diagnostic(void) const noexcept
    {
        return diagnostics_[SCENE_RING].frame;
    }

  private:
    double ticks(void);
    double clamp(double value, double low, double high);
    DWORD color(WORD value);
    void release_entry(SceneEntry *entry);
    void reset_timeline(void);
    void check_stall(void);
    int assign_masks(const scene_record *record);
    int build_draw(ID3D11Device *device, SceneEntry *entry, const scene_record *record, unsigned index);
    int upload_pixels(ID3D11Device *device, Texture2D *out, RECT bounds);
    int region_pixel(const scene_record *record, unsigned region, int p);
    unsigned source_layer(unsigned region);
    void include_pixel(unsigned index, int x, int y);
    void find_bounds(const scene_record *record);
    int build_region(ID3D11Device *device, SceneEntry *entry, const scene_record *record, unsigned index);
    void discard_first(void);
    const scene_ship *find_ship(const SceneEntry *entry, uintptr_t ptr);
    void camera_fraction(const SceneEntry *entry, uintptr_t follow, double *camera);
    int classify(const SceneEntry *before, const SceneEntry *entry, double *cam0, double *cam1);
    void draw_sprite(ID3D11DeviceContext *context, const Texture2D *region, double dx, double dy, RECT clip,
                     UINT width, UINT height);
    void draw_region(ID3D11DeviceContext *context, const SceneEntry *entry, unsigned index, double dx,
                     double dy, RECT clip, UINT width, UINT height);
    int cloud_snap(const SceneEntry *before, const SceneEntry *entry, unsigned i);
    void draw_clouds(ID3D11DeviceContext *context, const SceneEntry *before, const SceneEntry *entry,
                     int reason, double alpha, const double *shift, UINT width, UINT height);
    void observe_motion(const SceneEntry *before, const SceneEntry *entry, const double *camera,
                        const double *cam0, const double *cam1, double positions[SCENE_SHIPS][2]);
    void draw_base(ID3D11DeviceContext *context, const SceneEntry *entry, const SceneEntry *edge,
                   const double *camera, const double *shift, UINT width, UINT height);

    void initialize_hd(ID3D11Device *device, ID3D11DeviceContext *context);
    bool build_hd_draw(ID3D11DeviceContext *context, SceneEntry *entry, const scene_record *record,
                       unsigned index, unsigned side, unsigned &triangles);
    void build_hd(ID3D11Device *device, ID3D11DeviceContext *context, SceneEntry *entry,
                  const scene_record *record, UINT width, UINT height);
    bool draw_hd(ID3D11DeviceContext *context, const SceneEntry *entry, unsigned index, double dx, double dy,
                 UINT width, UINT height);
    std::unique_ptr<ShipGpu> hd_;
    ComPtr<ID3D11PixelShader> hd_transfer_;
    ComPtr<ID3D11Buffer> hd_constants_;
    ComPtr<ID3D11BlendState> hd_add_;
    unsigned hd_render_scale_ = 0;
    unsigned ships_hd_ = 0;
    unsigned ships_fallback_ = 0;
    unsigned triangles_hd_ = 0;
    unsigned hd_last_uploads_ = 0;
    double hd_ms_ = 0;
    FrameQueue &queue_;
    SceneDiagnostic *diagnostics_ = nullptr;
    DWORD diagnostic_owner_time_ = {};
    DWORD diagnostic_mask_time_ = {};
    DWORD diagnostic_ui_time_ = {};
    int diagnostic_overlap_ = {};
    CloudArt art_;
    std::unique_ptr<WakeLayer> wake_;
    bool wake_ready_ = false;
    SceneResources resources_;
    bool initialized_ = false;
    unsigned cloud_drawn_ = {};
    unsigned cloud_interpolated_ = {};
    unsigned cloud_snapped_ = {};
    unsigned cloud_fallback_ = {};
    int split_clouds_ = {};
    unsigned count_ = {};
    unsigned draws_ = {};
    unsigned uploads_ = {};
    unsigned long long upload_bytes_ = {};
    RECT bounds_[REGION_COUNT] = {};
    DWORD pixels_[SCENE_PIXELS] = {};
    WORD replay_[SCENE_PIXELS] = {};
    unsigned sets_ = {};
    signed char owner_[2][SCENE_PIXELS] = {};
    LONG epoch_ = {};
    int valid_ = {};
    unsigned complete_records_ = {};
    unsigned held_records_ = {};
    bool stalled_ = false;
    double stall_qpc_ = {};
    unsigned stall_plain_ = {};
    double stall_max_ms_ = {};
    double frequency_ = {};
    double time_ = {};
    double now_ = {};
    double interval_ = {};
    uintptr_t follow_ = {};
    double changed_pair_ = {};
    unsigned rejected_ = {};
    unsigned frames_ = {};
    unsigned snaps_[SNAP_COUNT] = {};
    unsigned backward_ = {};
    unsigned mask_pixels_[4] = {};
    double cost_ = {};
    double max_cost_ = {};
    double last_camera_[2] = {};
    double last_ship_[SCENE_SHIPS][2] = {};
    uintptr_t last_ptr_[SCENE_SHIPS] = {};
    int motion_ = {};
    double frame_start_ = {};
    double frame_cost_ = {};
    double frame_max_ = {};
    unsigned frame_count_ = {};

    unsigned last_uploads_ = {};
    unsigned long long last_bytes_ = {};
};
#endif
