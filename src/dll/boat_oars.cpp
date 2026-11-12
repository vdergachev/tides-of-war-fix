#include "boat_oars.h"
#include "boat_oars_vs.h"
#include "boat_oars_ps.h"
#include <math.h>
#include <string.h>

/* RTG deck/gunwale dimensions, model units. No texture is added to the hull. */
static const double OAR_PIVOT_X = .013;
static const double OAR_PIVOT_Y = .017167;
static const double OAR_ROW_SPAN = .060;
static const double OAR_SHAFT_WIDTH = .0016;
static const double OAR_BLADE_WIDTH = .004;
static const double OAR_BLADE_START = .70;
static const double OAR_SHADOW_DROP = .003;
static const double OAR_STOP_EPSILON = 1e-7;
static const double OAR_MAX_JUMP = 32;

OarVertex boat_project(double x, double y, double z, double heading, double anchor_x, double anchor_y,
                       float r, float g, float b, float a)
{
    double angle = heading * OAR_PI / 180;
    /* Match the captured hull: local +Z maps to (+sin(h), 0, +cos(h)).
     * Screen Y is inverted below; do not negate heading a second time. */
    double rotated_x = x * cos(angle) + z * sin(angle);
    double rotated_z = -x * sin(angle) + z * cos(angle);
    double screen_y = cos(65 * OAR_PI / 180) * y + sin(65 * OAR_PI / 180) * rotated_z;
    return {{(float)(anchor_x + 144 * rotated_x), (float)(anchor_y - 144 * screen_y), 0, 1}, {r, g, b, a}};
}

static void append_quad(OarVertex *out, double pivot_x, double pivot_z, double side, double angle,
                        double start, double end, double width, double y, double heading, double ax,
                        double ay, bool shadow)
{
    static const unsigned CORNERS[] = {0, 1, 2, 2, 1, 3};
    for (unsigned i = 0; i < 6; ++i) {
        unsigned corner = CORNERS[i];
        double distance = (corner & 2) ? end : start;
        double across = ((corner & 1) ? 1 : -1) * width / 2;
        double x = pivot_x + side * (cos(angle) * distance - sin(angle) * across);
        double z = pivot_z + sin(angle) * distance + cos(angle) * across;
        /* A darker edge supplies a small wood highlight without another texture. */
        float shade = (corner & 1) ? .82f : 1.f;
        out[i] =
            boat_project(x, y, z, heading, ax, ay, shadow ? .10f : .53f * shade, shadow ? .08f : .34f * shade,
                         shadow ? .06f : .17f * shade, shadow ? .28f : 1.f);
    }
}

unsigned boat_oar_vertices(OarVertex *out, const boat_oars_config &config, double heading, double phase,
                           double anchor_x, double anchor_y)
{
    if (!config.enabled || config.per_side < 1 || config.per_side > 8 || !isfinite(heading)) {
        return 0;
    }
    unsigned count = 0;
    double angle = config.amplitude_deg * OAR_PI / 180 * sin(phase);
    /* Shadow first, then both sides, so every blade remains above its shadow. */
    for (unsigned layer = 0; layer < 3; ++layer) {
        for (int row = 0; row < config.per_side; ++row) {
            double z = config.per_side == 1 ? 0 : OAR_ROW_SPAN * (row / (config.per_side - 1.) - .5);
            for (int side = -1; side <= 1; side += 2) {
                bool shadow = layer == 0;
                bool blade = layer == 2;
                append_quad(out + count, side * OAR_PIVOT_X, z, side, angle,
                            blade ? config.length * OAR_BLADE_START : 0, config.length,
                            blade ? OAR_BLADE_WIDTH : OAR_SHAFT_WIDTH,
                            OAR_PIVOT_Y - (shadow ? OAR_SHADOW_DROP : 0), heading, anchor_x, anchor_y,
                            shadow);
                count += 6;
            }
        }
    }
    return count;
}

bool BoatOars::init(ID3D11Device *device)
{
    if (vertices_) {
        return true;
    }
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(OarVertex) * OAR_VERTEX_LIMIT;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateVertexShader(tow_boat_oars_vs, sizeof(tow_boat_oars_vs), nullptr,
                                                vs_.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device->CreatePixelShader(tow_boat_oars_ps, sizeof(tow_boat_oars_ps), nullptr,
                                               ps_.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device->CreateBuffer(&desc, nullptr, vertices_.ReleaseAndGetAddressOf()));
}

bool BoatOars::draw(ID3D11DeviceContext *context, const OarVertex *vertices, unsigned count)
{
    if (!vertices_ || !count || count > OAR_VERTEX_LIMIT) {
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(vertices_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    memcpy(mapped.pData, vertices, count * sizeof(OarVertex));
    context->Unmap(vertices_.Get(), 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, vertices_.GetAddressOf());
    context->PSSetShader(ps_.Get(), nullptr, 0);
    context->Draw(count, 0);
    return true;
}

void BoatOars::reset()
{
    memset(motion_, 0, sizeof(motion_));
    serial_ = 0;
}

double BoatOars::phase(const scene_ship &ship, double x, double y, double stamp, bool snap,
                       const boat_oars_config &config)
{
    Motion *slot = &motion_[0];
    for (auto &motion : motion_) {
        if (motion.ptr == ship.ptr && motion.id == ship.id) {
            slot = &motion;
            break;
        }
        if (motion.serial < slot->serial) {
            slot = &motion;
        }
    }
    double distance = hypot(x - slot->x, y - slot->y);
    bool known = slot->ptr == ship.ptr && slot->id == ship.id;
    double phase = slot->phase;
    if (!known || snap || stamp < slot->stamp || stamp - slot->stamp > .5 || distance > OAR_MAX_JUMP) {
        phase = 0;
    } else if (stamp > slot->stamp) {
        phase = distance > OAR_STOP_EPSILON
                    ? fmod(phase + distance * config.strokes_per_unit * 2 * OAR_PI, 2 * OAR_PI)
                    : 0;
    }
    *slot = {ship.ptr, ship.id, x, y, stamp, phase, ++serial_};
    return phase;
}
