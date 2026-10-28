#ifndef TOW_WAKE_LAYER_H
#define TOW_WAKE_LAYER_H
#include "scene.h"
#include "texture2d.h"
#include "wake_config.h"
#include "wake_field.h"
#include <memory>
using Microsoft::WRL::ComPtr;

struct WakePose {
    uintptr_t ptr;
    unsigned id;
    double x;
    double y;
    double course;
    double stern;
};

/* SceneCompositor owns this optional render-thread-only layer. */
class WakeLayer
{
  public:
    static WakePose placed_pose(const scene_ship &ship, double dx, double dy, POINT captured_camera,
                                const double *camera);
    bool init(ID3D11Device *device);
    void reset();
    bool update(ID3D11DeviceContext *context, RECT view, POINT captured_camera, const double *camera,
                const WakePose *poses, unsigned count, unsigned follow, const WORD *bottom, double stamp,
                bool snap);
    void bind(ID3D11DeviceContext *context, float *constants);

    ID3D11PixelShader *shader() const
    {
        return shader_.Get();
    }

    void log();

  private:
    struct History {
        WakePose pose;
        double stamp;
        double remainder;
    };

    struct Particle {
        double x;
        double y;
        double born;
        double strength;
        uintptr_t ptr;
        unsigned id;
    };

    struct Owner {
        uintptr_t ptr;
        unsigned id;
        unsigned count;
    };

    bool simulate(RECT view, POINT captured_camera, const double *camera, const WakePose *poses,
                  unsigned count, unsigned follow, const WORD *bottom, double stamp, bool snap);
    bool upload(ID3D11DeviceContext *context);
    bool texture(ID3D11Device *device, Texture2D &out, UINT width, UINT height, DXGI_FORMAT format);
    const History *find(const WakePose &pose);
    void emit(const WakePose &pose, const History *old, double stamp, bool snap, History &next);
    void bubbles(RECT view, POINT camera, const WORD *bottom, double stamp);
    bool sea(WORD pixel);
    double ticks();

    wake_config settings_ = {};
    WakeField field_;
    Texture2D ripple_;
    Texture2D foam_;
    ComPtr<ID3D11PixelShader> shader_;
    std::unique_ptr<Particle[]> particles_;
    std::unique_ptr<Owner[]> owners_;
    unsigned capacity_ = 0;
    unsigned start_ = 0;
    unsigned alive_ = 0;
    History history_[SCENE_SHIPS] = {};
    unsigned count_ = 0;
    BYTE foam_pixels_[SCENE_PIXELS] = {};
    double tick_ = -1;
    double stamp_ = -1;
    double fraction_ = 0;
    double step_ = .078;
    double frequency_ = 0;
    double step_ms_ = 0;
    double frame_ms_ = 0;
    unsigned steps_ = 0;
    unsigned frames_ = 0;
};
#endif
