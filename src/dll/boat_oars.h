#ifndef TOW_BOAT_OARS_H
#define TOW_BOAT_OARS_H
#include "scene.h"
#include "wake_config.h"
#include <d3d11.h>
#include <wrl/client.h>

/* Eight pairs, shaft/blade/shadow, two triangles each. */
inline constexpr unsigned OAR_VERTEX_LIMIT = 8 * 2 * 3 * 6;
inline constexpr double OAR_PI = 3.141592653589793;

struct OarVertex {
    float position[4];
    float color[4];
};

/* Same model projection for the live overlay and the offline preview. */
OarVertex boat_project(double x, double y, double z, double heading, double anchor_x, double anchor_y,
                       float r, float g, float b, float a = 1);
unsigned boat_oar_vertices(OarVertex *out, const boat_oars_config &config, double heading, double phase,
                           double anchor_x, double anchor_y);

class BoatOars
{
  public:
    bool init(ID3D11Device *device);
    bool draw(ID3D11DeviceContext *context, const OarVertex *vertices, unsigned count);
    double phase(const scene_ship &ship, double x, double y, double stamp, bool snap,
                 const boat_oars_config &config);
    void reset();

  private:
    struct Motion {
        uintptr_t ptr;
        unsigned id;
        double x;
        double y;
        double stamp;
        double phase;
        unsigned serial;
    };

    Motion motion_[SCENE_SHIPS] = {};
    unsigned serial_ = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertices_;
};
#endif
