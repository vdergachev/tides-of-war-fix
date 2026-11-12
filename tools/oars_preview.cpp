#include "../src/dll/boat_oars.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <memory>
#include <new>

using Microsoft::WRL::ComPtr;
static const unsigned WIDTH = 2560;
static const unsigned HEIGHT = 1920;
static const double CLOSEUP = 6;
static const boat_oars_config CONFIG = {1, 3, .035, 30, .08};

/* Independent hull fixture: row-vector model matrix, then captured camera.
 * Do not call boat_project here: that hid the opposite oar rotation. */
static OarVertex hull_project(double x, double y, double z, double heading, double ax, double ay)
{
    double angle = heading * OAR_PI / 180;
    double matrix[3][3] = {{cos(angle), 0, -sin(angle)}, {0, 1, 0}, {sin(angle), 0, cos(angle)}};
    double local[3] = {x, y, z};
    double world[3] = {};
    for (unsigned column = 0; column < 3; ++column) {
        for (unsigned row = 0; row < 3; ++row) {
            world[column] += local[row] * matrix[row][column];
        }
    }
    return {{(float)(ax + 144 * world[0]),
             (float)(ay - 144 * (.42261826174069944 * world[1] + .9063077870366499 * world[2])), 0, 1},
            {.48f, .29f, .13f, 1}};
}

static double direction_error(const OarVertex &a, const OarVertex &b, const OarVertex &c, const OarVertex &d)
{
    double first = atan2(b.position[1] - a.position[1], b.position[0] - a.position[0]);
    double second = atan2(d.position[1] - c.position[1], d.position[0] - c.position[0]);
    return fabs(remainder(first - second, 2 * OAR_PI)) * 180 / OAR_PI;
}

/* Midpoint of the shaft's two start corners is its gunwale pivot. */
static OarVertex pivot(const OarVertex *vertices, unsigned row, unsigned side)
{
    unsigned offset = CONFIG.per_side * 2 * 6 + (row * 2 + side) * 6;
    OarVertex point = vertices[offset];
    for (unsigned i = 0; i < 2; ++i) {
        point.position[i] = (vertices[offset].position[i] + vertices[offset + 1].position[i]) / 2;
    }
    return point;
}

static bool verify_alignment()
{
    double maximum = 0;
    unsigned frames = 0;
    /* Forward/backward travel is independent of model heading; course is deliberately
     * different. Translation is interpolated, hull rotation is held per step. */
    for (int reverse = 0; reverse < 2; ++reverse) {
        for (int step = 0; step <= 720; ++step) {
            scene_ship ship = {};
            ship.heading = step <= 360 ? step : 720 - step;
            ship.course = reverse ? ship.heading + 180 : ship.heading + 37;
            for (unsigned fraction = 0; fraction <= 4; ++fraction) {
                double alpha = fraction / 4.;
                double ax = (reverse ? -1 : 1) * (step + alpha) / 10;
                double ay = 30 + alpha;
                OarVertex vertices[OAR_VERTEX_LIMIT];
                if (!boat_oar_vertices(vertices, CONFIG, ship.heading, step * .17 + alpha, ax, ay)) {
                    return false;
                }
                OarVertex aft = hull_project(0, .017167, -.03, ship.heading, ax, ay);
                OarVertex bow = hull_project(0, .017167, .03, ship.heading, ax, ay);
                for (unsigned side = 0; side < 2; ++side) {
                    double error =
                        direction_error(aft, bow, pivot(vertices, 0, side), pivot(vertices, 2, side));
                    maximum = fmax(maximum, error);
                    if (!(error < 3)) {
                        fprintf(stderr, "Hull/oar angle error %.6f at heading %.1f\n", error, ship.heading);
                        return false;
                    }
                    for (unsigned row = 0; row < 3; ++row) {
                        OarVertex expected = hull_project(side ? .013 : -.013, .017167, -.03 + row * .03,
                                                          ship.heading, ax, ay);
                        OarVertex actual = pivot(vertices, row, side);
                        if (hypot(actual.position[0] - expected.position[0],
                                  actual.position[1] - expected.position[1]) > .0001) {
                            return false;
                        }
                    }
                }
                ++frames;
            }
        }
    }
    /* Explicit wrap endpoints: no independent angular lerp while hull is held. */
    for (double heading : {359., 0., 1., 361., -1.}) {
        OarVertex actual = boat_project(0, 0, 1, heading, 0, 0, 1, 1, 1);
        OarVertex expected = hull_project(0, 0, 1, heading, 0, 0);
        if (hypot(actual.position[0] - expected.position[0], actual.position[1] - expected.position[1]) >
            .0001) {
            return false;
        }
    }
    printf("Hull/oar alignment: %u frames, max error %.6f deg (<3); reverse/sides/wrap OK\n", frames,
           maximum);
    return true;
}

static void closeup(OarVertex *vertices, unsigned count, double x, double y)
{
    for (unsigned i = 0; i < count; ++i) {
        vertices[i].position[0] = (float)(x + (vertices[i].position[0] - x) * CLOSEUP);
        vertices[i].position[1] = (float)(y + (vertices[i].position[1] - y) * CLOSEUP);
    }
}

/* RTG is read-only. The preview uses the original polygons with flat wood
 * colours; live rendering keeps the game's textured hull unchanged. */
static bool hull(BoatOars &gpu, ID3D11DeviceContext *context, const char *path, double heading, double ax,
                 double ay)
{
    FILE *file = nullptr;
    if (fopen_s(&file, path, "r") || !file) {
        return false;
    }
    double positions[64][3] = {};
    unsigned count = 0;
    unsigned triangles = 0;
    bool reading = false;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, "VERTEX local", 12)) {
            reading = true;
            count = 0;
            continue;
        }
        if (!strncmp(line, "TEXCOORD", 8)) {
            reading = false;
        }
        if (reading) {
            double x;
            double y;
            double z;
            if (sscanf_s(line, "%lf %lf %lf", &x, &y, &z) == 3 && count < 64) {
                positions[count][0] = x;
                positions[count][1] = y;
                positions[count++][2] = z;
            }
        }
        unsigned a;
        unsigned b;
        unsigned c;
        if (sscanf_s(line, "v %u %u %u", &a, &b, &c) == 3) {
            if (a >= count || b >= count || c >= count) {
                fclose(file);
                return false;
            }
            unsigned indices[] = {a, b, c};
            OarVertex triangle[3];
            for (unsigned i = 0; i < 3; ++i) {
                const double *p = positions[indices[i]];
                float shade = p[1] > .01 ? 1.f : .65f;
                triangle[i] = hull_project(p[0], p[1], p[2], heading, ax, ay);
                for (unsigned channel = 0; channel < 3; ++channel) {
                    triangle[i].color[channel] *= shade;
                }
            }
            closeup(triangle, 3, ax, ay);
            if (!gpu.draw(context, triangle, 3)) {
                fclose(file);
                return false;
            }
            ++triangles;
        }
    }
    fclose(file);
    return triangles > 0;
}

static bool verify_motion()
{
    BoatOars gpu;
    scene_ship ship = {};
    ship.ptr = 1;
    ship.id = 7;
    if (gpu.phase(ship, 0, 0, 1, false, CONFIG) != 0) {
        return false;
    }
    double phase = gpu.phase(ship, 1, 0, 1.1, false, CONFIG);
    if (fabs(phase - .08 * 2 * OAR_PI) > 1e-9 || gpu.phase(ship, 1, 0, 1.1, false, CONFIG) != phase ||
        gpu.phase(ship, 1, 0, 1.2, false, CONFIG) != 0 || gpu.phase(ship, 100, 0, 1.3, false, CONFIG) != 0) {
        return false;
    }
    ship.id = 8;
    if (gpu.phase(ship, 101, 0, 1.4, false, CONFIG) != 0) {
        return false;
    }
    OarVertex vertices[OAR_VERTEX_LIMIT];
    boat_oars_config config = CONFIG;
    config.per_side = 8;
    if (boat_oar_vertices(vertices, config, 0, 0, 0, 0) != OAR_VERTEX_LIMIT) {
        return false;
    }
    config.enabled = 0;
    return boat_oar_vertices(vertices, config, 0, 0, 0, 0) == 0;
}

static bool save_bmp(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *target,
                     const char *path)
{
    D3D11_TEXTURE2D_DESC desc;
    target->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        return false;
    }
    context->CopyResource(staging.Get(), target);
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        return false;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = WIDTH;
    info.bmiHeader.biHeight = -(LONG)HEIGHT;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biSizeImage = WIDTH * HEIGHT * 4;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        context->Unmap(staging.Get(), 0);
        return false;
    }
    for (unsigned y = 0; y < HEIGHT; ++y) {
        memcpy((BYTE *)bits + y * WIDTH * 4, (BYTE *)map.pData + y * map.RowPitch, WIDTH * 4);
    }
    context->Unmap(staging.Get(), 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bitmap);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    SetTextColor(dc, RGB(240, 240, 240));
    SetBkMode(dc, TRANSPARENT);
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            char label[128];
            sprintf_s(label, "%s heading=%u phase=%u deg | 6x closeup", row < 2 ? "Launch" : "Canoe",
                      row % 2 ? 115 : 25, column * 90);
            TextOutA(dc, column * (WIDTH / 4) + 30, row * (HEIGHT / 4) + 30, label, (int)strlen(label));
        }
    }
    GdiFlush();
    SelectObject(dc, old);
    DeleteDC(dc);
    BITMAPFILEHEADER header = {};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + info.bmiHeader.biSizeImage;
    FILE *file = nullptr;
    bool ok = !fopen_s(&file, path, "wb") && file;
    if (ok) {
        ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
             fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file) == 1 &&
             fwrite(bits, info.bmiHeader.biSizeImage, 1, file) == 1;
        ok = fclose(file) == 0 && ok;
    }
    DeleteObject(bitmap);
    return ok;
}

int main(int argc, char **argv)
{
    if (!verify_motion() || !verify_alignment()) {
        fprintf(stderr, "Oar verification failed\n");
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--verify-only")) {
        return 0;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, &context))) {
        return 1;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = WIDTH;
    desc.Height = HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &view))) {
        return 1;
    }
    context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
    D3D11_VIEWPORT viewport = {0, 0, (FLOAT)WIDTH, (FLOAT)HEIGHT, 0, 1};
    context->RSSetViewports(1, &viewport);
    D3D11_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> raster;
    D3D11_BLEND_DESC blend_desc = {};
    auto &blend_target = blend_desc.RenderTarget[0];
    blend_target.BlendEnable = TRUE;
    blend_target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_target.BlendOp = D3D11_BLEND_OP_ADD;
    blend_target.SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_target.DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    if (FAILED(device->CreateRasterizerState(&raster_desc, &raster)) ||
        FAILED(device->CreateBlendState(&blend_desc, &blend))) {
        return 1;
    }
    context->RSSetState(raster.Get());
    context->OMSetBlendState(blend.Get(), nullptr, UINT32_MAX);
    FLOAT background[] = {.12f, .29f, .38f, 1};
    context->ClearRenderTargetView(view.Get(), background);
    BoatOars gpu;
    if (!gpu.init(device.Get())) {
        return 1;
    }
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            double ax = 80 + column * 160.;
            double ay = 60 + row * 120.;
            double heading = row % 2 ? 115 : 25;
            const char *path = row < 2 ? "game-test/Baggage/MobileModels/M/launch.rtg"
                                       : "game-test/Baggage/MobileModels/M/canoe.rtg";
            if (!hull(gpu, context.Get(), path, heading, ax, ay)) {
                return 1;
            }
            OarVertex vertices[OAR_VERTEX_LIMIT];
            unsigned count = boat_oar_vertices(vertices, CONFIG, heading, column * OAR_PI / 2, ax, ay);
            closeup(vertices, count, ax, ay);
            if (!gpu.draw(context.Get(), vertices, count)) {
                return 1;
            }
        }
    }
    if (!save_bmp(device.Get(), context.Get(), target.Get(), "tow_fix/build/oars_preview.bmp")) {
        return 1;
    }
    printf("WARP preview: tow_fix/build/oars_preview.bmp; motion/limits OK\n");
    return 0;
}
