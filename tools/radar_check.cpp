/* Synthetic captures and the real D3D11 radar pass, without a game process. */
#include "../src/dll/frame_queue.h"
#include "../src/dll/cloud_art_gpu.h"
#include "../src/dll/texture2d.h"
#include "../src/dll/wake_layer.h"
#include "../src/dll/ship_gpu.h"
#include "../src/dll/boat_oars.h"
#include "../src/dll/scene_timeline.h"
#define private public
#include "../src/dll/scene_gpu.h"
#undef private
#include "../src/dll/scene_gpu.cpp"
#include <cassert>
#include <cstdio>
#include <new>
#include <cstdlib>
#undef assert
#define assert(condition)                                                                                    \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (0)

/* Analytic world stripes provide an independent, spatially registered oracle. */
static WORD terrain(int world_x, int world_y)
{
    return ((world_x / 3 + world_y / 6) % 7) ? 0x001f : 0xffff;
}

static void capture(radar_capture &out, double left, double top)
{
    out = {};
    out.valid = 1;
    out.left = left;
    out.top = top;
    out.tile = {(LONG)floor(left / 64), (LONG)floor(top / 64)};
    out.follow = 123;
    out.follow_id = 7;
    for (int y = 0; y < RADAR_SIDE; ++y) {
        for (int x = 0; x < RADAR_SIDE; ++x) {
            unsigned p = y * RADAR_SIDE + x;
            out.terrain[p] = terrain(out.tile.x * 3 + x - 1, out.tile.y * 3 + y - 1);
            /* Deliberately asymmetric aperture, not a radius-48 circle. */
            out.overlay_mask[p] = x < 8 || x >= 120 || y < 20 || y >= 110;
            out.final[p] = 0x07e0;
            out.overlay[p] = out.final[p];
        }
    }
    out.count = 2;
    for (unsigned i = 0; i < out.count; ++i) {
        radar_marker &m = out.markers[i];
        m.ptr = 100 + i;
        m.id = 12 + i;
        m.kind = i;
        m.x = 2400 + i * 420 + left * (i ? 0 : .25);
        m.y = 2400 + top * (i ? 0 : .25);
        m.projected = {(LONG)((m.x - left) / 21), (LONG)((m.y - top) / 21)};
        LONG ax = i ? -1 : -4;
        LONG ay = i ? -8 : -4;
        m.dest = {m.projected.x + ax, m.projected.y + ay, m.projected.x + ax + 8, m.projected.y + ay + 8};
        m.key = 0;
        for (WORD &pixel : m.pixels) {
            pixel = i ? 0xf800 : 0xffe0;
        }
    }
}

static void check_geometry()
{
    auto old = std::unique_ptr<radar_capture>(new radar_capture{});
    auto now = std::unique_ptr<radar_capture>(new radar_capture{});
    double previous = 1e30;
    for (unsigned step = 0; step < 260; ++step) {
        double origin = 1020 + step * .5;
        capture(*old, origin, origin);
        capture(*now, origin + .5, origin + .5);
        assert(radar_pair_valid(old.get(), now.get()));
        for (unsigned phase = 0; phase < 4; ++phase) {
            double alpha = phase / 4.0;
            double left = origin + .5 * alpha;
            // Position of one world tile feature, independent of capture origin.
            double feature = 1 + (60 - now->tile.x * 3) + radar_terrain_shift(now->tile.x, left);
            if (previous < 1e20) {
                assert(fabs(feature - previous + .125 * 3 / 64) < 1e-9);
            }
            previous = feature;
            double x;
            double y;
            radar_marker_position(old.get(), now.get(), 0, alpha, &x, &y);
            assert(fabs(x - ((2400 + left * .25 - left) / 21 - 4)) < 1e-9);
            assert(fabs(y - x) < 1e-9);
            assert(radar_coverage(old.get(), now.get(), alpha));
        }
    }
    now->terrain[64 * RADAR_SIDE + 64] ^= 1;
    assert(!radar_pair_valid(old.get(), now.get()));
    assert(radar_pair_reason(old.get(), now.get()) == RADAR_SEAM);
    capture(*now, old->left + 1, old->top + 1);
    now->valid = 0;
    assert(!radar_pair_valid(old.get(), now.get()));
    assert(radar_pair_reason(old.get(), now.get()) == RADAR_INCOMPLETE);
    now->valid = 1;
    ++now->follow;
    assert(!radar_pair_valid(old.get(), now.get()));
    assert(radar_pair_reason(old.get(), now.get()) == RADAR_IDENTITY);
    now->follow = old->follow;
    double x;
    double y;
    now->markers[0].id += 1;
    radar_marker_position(old.get(), now.get(), 0, .5, &x, &y);
    assert(fabs(x - ((now->markers[0].x - (old->left + now->left) * .5) / 21 - 4)) < 1e-9);
    now->count = 0;
    assert(radar_pair_valid(old.get(), now.get())); // disappearance is discrete
    now->overlay_mask[RADAR_PIXELS - 1] = 0;
    assert(!radar_coverage(old.get(), now.get(), .5));
    puts("radar geometry: two tile boundaries, continuous terrain/markers, seam/incomplete/coverage OK");
}

static void check_speed(double speed, int direction)
{
    auto old = std::unique_ptr<radar_capture>(new radar_capture{});
    auto now = std::unique_ptr<radar_capture>(new radar_capture{});
    double previous = 1e30;
    /* Both signs, diagonal tile crossings and fractional origins. The followed
     * marker moves at full viewport speed, including beyond the old 256 limit. */
    for (unsigned step = 0; step < 64; ++step) {
        double origin = 30000.75 + direction * speed * step;
        double next = origin + direction * speed;
        capture(*old, origin, origin);
        capture(*now, next, next);
        old->markers[0].x = old->markers[0].y = origin + 1344;
        now->markers[0].x = now->markers[0].y = next + 1344;
        now->markers[0].projected = {64, 64};
        now->markers[0].dest = {60, 60, 68, 68};
        assert(radar_find(old.get(), &now->markers[0]));
        assert(radar_pair_reason(old.get(), now.get()) == RADAR_OK);
        for (unsigned phase = 0; phase < 16; ++phase) {
            double alpha = phase / 16.0;
            double left = origin + direction * speed * alpha;
            double feature = 1 + (1500 - now->tile.x * 3) + radar_terrain_shift(now->tile.x, left);
            if (previous < 1e20) {
                assert(fabs(feature - previous + direction * speed / 16 * 3 / 64) < 1e-9);
            }
            previous = feature;
            double x;
            double y;
            radar_marker_position(old.get(), now.get(), 0, alpha, &x, &y);
            assert(fabs(x - 60) < 1e-9 && fabs(y - 60) < 1e-9);
            assert(radar_coverage(old.get(), now.get(), alpha));
        }
    }
    capture(*now, old->left + RADAR_TELEPORT + 1, old->top);
    assert(radar_pair_reason(old.get(), now.get()) == RADAR_SNAP);
    assert(!radar_find(old.get(), &now->markers[0]));
    printf("radar speed %.2f direction %d: 1024 phases, continuous terrain/ship, no fallback OK\n", speed,
           direction);
}

static void check_skipped_partial()
{
    auto last = std::unique_ptr<radar_capture>(new radar_capture{});
    auto now = std::unique_ptr<radar_capture>(new radar_capture{});
    auto old = std::unique_ptr<radar_capture>(new radar_capture{});
    assert(!radar_reuse(now.get(), last.get())); // no complete seed yet
    capture(*last, 1023, 1023);
    *old = *last;
    double x0;
    double y0;
    radar_marker_position(old.get(), old.get(), 0, 0, &x0, &y0);
    for (unsigned step = 1; step <= 12; ++step) {
        // A terrain-only/compass-only capture is deliberately unusable.
        *now = {};
        if (step % 2) {
            now->terrain[64 * RADAR_SIDE + 64] = 0xdead;
            now->count = 1;
            now->markers[0].id = 999;
        }
        assert(radar_reuse(now.get(), last.get()));
        now->left += .5;
        now->top += .5;
        radar_update_marker(now.get(), now->markers[0].ptr, now->markers[0].id, last->markers[0].x + 2,
                            last->markers[0].y + 2);
        // Wrong identity and unavailable object positions preserve the last pose.
        double port_x = last->markers[1].x;
        radar_update_marker(now.get(), now->markers[0].ptr, 999, 1, 1);
        assert(now->markers[1].x == port_x);
        assert(now->failure == RADAR_OK && radar_pair_valid(old.get(), now.get()));
        assert(now->tile.x == 15 && now->tile.y == 15);
        assert(now->terrain[64 * RADAR_SIDE + 64] == old->terrain[64 * RADAR_SIDE + 64]);
        for (unsigned phase = 0; phase < 4; ++phase) {
            double x;
            double y;
            double alpha = phase / 4.0;
            radar_marker_position(old.get(), now.get(), 0, alpha, &x, &y);
            double progress = step - 1 + alpha;
            assert(fabs(x - x0 - progress * 1.5 / 21) < 1e-9);
            assert(fabs(y - y0 - progress * 1.5 / 21) < 1e-9);
            assert(radar_coverage(old.get(), now.get(), alpha));
        }
        *last = *now;
        *old = *now;
    }
    puts("radar skipped/partial: retained complete layers, moving markers, stable identity, 48 phases OK");
}

static void check_marker_changes()
{
    auto old = std::unique_ptr<radar_capture>(new radar_capture{});
    auto hidden = std::unique_ptr<radar_capture>(new radar_capture{});
    auto now = std::unique_ptr<radar_capture>(new radar_capture{});
    capture(*old, 1023, 1023);
    capture(*now, 1024, 1024);
    now->markers[0].source += 104;
    now->markers[0].pixels[0] ^= 0xffff;
    now->markers[0].source_rect.left += 1;
    assert(radar_find(old.get(), &now->markers[0]) == &old->markers[0]);
    // Hidden for one full draw; its position keeps updating, art is not drawn.
    capture(*hidden, 1023.5, 1023.5);
    hidden->count = 0;
    radar_remember(hidden.get(), old.get());
    assert(hidden->history_count == old->count);
    hidden->history[0].x = 2400 + 1023.5 * .25;
    hidden->history[0].y = hidden->history[0].x;
    for (unsigned phase = 0; phase <= 16; ++phase) {
        double alpha = phase / 16.0;
        double x;
        double y;
        radar_marker_position(hidden.get(), now.get(), 0, alpha, &x, &y);
        double origin = 1023.5 + .5 * alpha;
        assert(fabs(x - ((2400 + origin * .25 - origin) / 21 - 4)) < 1e-9);
        assert(fabs(x - y) < 1e-9);
    }
    ++now->markers[0].id;
    assert(!radar_find(hidden.get(), &now->markers[0]));
    --now->markers[0].id;
    now->markers[0].x += RADAR_TELEPORT + 1;
    assert(!radar_find(hidden.get(), &now->markers[0]));
    now->count = 0;
    radar_remember(now.get(), hidden.get());
    assert(!now->history_count); // a genuinely absent object expires
    puts("radar markers: art/relation identity, visibility flicker, ID reuse/teleport/expiry OK");
}

static void pointer_fixture(radar_pointer &out, double angle, double radius, WORD ink)
{
    out = {};
    out.present = 1;
    out.valid = 1;
    out.angle = angle;
    out.radius = radius;
    out.centre = {64, 64};
    out.follow = 123;
    out.follow_id = 7;
    double radians = angle * 3.14159265358979323846 / 180;
    LONG x = 64 + (LONG)round(radius * sin(radians)) - 6;
    LONG y = 64 - (LONG)round(radius * cos(radians)) - 6;
    out.dest = {x, y, x + 13, y + 13};
    for (unsigned p = 0; p < 13 * 13; ++p) {
        out.pixels[p] = p % 7 ? ink : 0x07e0;
    }
}

static void check_pointer_geometry()
{
    radar_pointer old;
    radar_pointer now;
    pointer_fixture(old, 359, 48, 0xffff);
    pointer_fixture(now, 1, 48, 0xffff);
    double x;
    double y;
    radar_pointer_position(&old, &now, .5, &x, &y);
    assert(fabs(x - 58) < 1e-9 && fabs(y - 10) < 1e-9);
    assert(radar_angle_delta(15 * 22.5, 0) == 22.5);
    pointer_fixture(old, 15 * 22.5, 48, 0xffff);
    pointer_fixture(now, 0, 48, 0xffff);
    radar_pointer_position(&old, &now, .5, &x, &y);
    double sector_midpoint = 348.75 * 3.14159265358979323846 / 180;
    assert(fabs(x - (58 + 48 * sin(sector_midpoint))) < 1e-9);
    assert(fabs(y - (58 - 48 * cos(sector_midpoint))) < 1e-9);
    pointer_fixture(old, 337.5, 48, 0xffff);
    pointer_fixture(now, 338, 48, 0xffff);
    double last_x;
    double last_y;
    radar_pointer_position(&old, &now, 0, &last_x, &last_y);
    for (unsigned phase = 1; phase <= 16; ++phase) {
        radar_pointer_position(&old, &now, phase / 16.0, &x, &y);
        double physical_step = hypot(x - last_x, y - last_y) * 4;
        assert(physical_step > .1 && physical_step < .11);
        last_x = x;
        last_y = y;
    }
    for (unsigned reason = 0; reason < 5; ++reason) {
        pointer_fixture(old, 0, 48, 0xffff);
        pointer_fixture(now, reason == 4 ? 91 : 20, 48, 0xffff);
        if (reason == 0) {
            ++old.follow;
        } else if (reason == 1) {
            ++old.follow_id;
        } else if (reason == 2) {
            old.valid = 0;
        } else if (reason == 3) {
            now.radius = 35;
        }
        radar_pointer_position(&old, &now, .25, &x, &y);
        double sx;
        double sy;
        radar_pointer_position(nullptr, &now, 1, &sx, &sy);
        assert(fabs(x - sx) < 1e-9 && fabs(y - sy) < 1e-9);
    }
    pointer_fixture(now, 0, 35, 0xffff);
    radar_pointer_position(nullptr, &now, 1, &x, &y);
    assert(x == 58 && y == 23 && radar_pointer_rect_valid(&now));
    now.dest.left += 2;
    now.dest.right += 2;
    assert(!radar_pointer_rect_valid(&now));
    puts("radar geometry: 359/1, sector 15/0, continuous half-degree x4, follow/invalid/jump/night snap OK");
}

static void check_warp(unsigned scale, double distance, bool skipped = false, bool switching = false,
                       bool occluded = false, unsigned pointers = 0)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, &context)));
    auto queue = std::unique_ptr<FrameQueue>(new FrameQueue{});
    auto gpu = std::unique_ptr<SceneCompositor>(new SceneCompositor(*queue));
    assert(gpu->init(device.Get()));
    auto old = std::unique_ptr<radar_capture>(new radar_capture{});
    auto now = std::unique_ptr<radar_capture>(new radar_capture{});
    capture(*old, 1023, 1023);
    capture(*now, 1023 + distance, 1023 + distance);
    if (switching) {
        old->history[0] = old->markers[0];
        old->history_count = 1;
        old->count = 0;
        now->markers[0].source += 104;
        old->history[0].pixels[0] ^= 0xffff;
        for (int y = 0; y < RADAR_SIDE; ++y) {
            for (int x = 0; x < RADAR_SIDE; ++x) {
                if (x < 10) {
                    now->overlay_mask[y * RADAR_SIDE + x] = 1;
                }
                now->final[y * RADAR_SIDE + x] = 0x07ff;
                now->overlay[y * RADAR_SIDE + x] = 0x07ff;
            }
        }
    }
    if (skipped) {
        double left = now->left;
        double top = now->top;
        radar_marker mobile = now->markers[0];
        assert(radar_reuse(now.get(), old.get()));
        now->left = left;
        now->top = top;
        radar_update_marker(now.get(), mobile.ptr, mobile.id, mobile.x, mobile.y);
    }
    if (pointers) {
        for (unsigned i = 0; i < RADAR_POINTERS; ++i) {
            pointer_fixture(old->pointers[i], 80, pointers == 3 ? 35 : 48, i ? 0xffe0 : 0x07ff);
            pointer_fixture(now->pointers[i], 90.5, pointers == 3 ? 35 : 48, i ? 0xffff : 0xf800);
            now->pointers[i].frame = 4;
        }
        if (pointers == 2) {
            now->pointers[RADAR_HEADING].valid = 0;
            now->pointers[RADAR_HEADING].dest = {109, 63, 122, 76};
        }
        /* Populate fallback bits through the same helper as a partial capture. */
        radar_reuse_pointers(now.get(), now.get());
        for (radar_capture *sample : {old.get(), now.get()}) {
            /* A frame extension crosses both the captured and moving sprite. */
            for (unsigned y = 45; y < 85; ++y) {
                for (unsigned x = 116; x < 120; ++x) {
                    unsigned p = y * RADAR_SIDE + x;
                    sample->overlay_mask[p] |= RADAR_FRAME_MASK;
                    sample->overlay[p] = sample->final[p] = 0x07e0;
                }
            }
            for (const radar_pointer &pointer : sample->pointers) {
                for (unsigned y = 0; y < 13; ++y) {
                    for (unsigned x = 0; x < 13; ++x) {
                        WORD ink = pointer.pixels[y * 13 + x];
                        if (ink != 0x07e0) {
                            unsigned p = (pointer.dest.top + y) * RADAR_SIDE + pointer.dest.left + x;
                            sample->final[p] = ink;
                        }
                    }
                }
            }
        }
    }
    SceneEntry &a = gpu->resources_.entries[0];
    SceneEntry &b = gpu->resources_.entries[1];
    auto clean = std::unique_ptr<radar_capture>(new radar_capture(*now));
    if (occluded) {
        WORD final[RADAR_PIXELS];
        memcpy(final, clean->final, sizeof(final));
        for (unsigned y = 30; y < 40; ++y) {
            for (unsigned x = 30; x < 40; ++x) {
                final[y * RADAR_SIDE + x] = 0x07ff;
            }
        }
        radar_mask_occlusion(now.get(), clean->final, final, RADAR_SIDE);
        assert(now->valid && radar_pair_valid(old.get(), now.get()));
    }
    gpu->build_radar(device.Get(), &a, old.get());
    gpu->build_radar(device.Get(), &b, now.get());
    assert(a.radar && b.radar);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 640 * scale;
    desc.Height = 480 * scale;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)));
    assert(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    assert(SUCCEEDED(device->CreateSamplerState(&sd, &sampler)));
    for (unsigned phase = 0; phase < (occluded ? 6u : 5u); ++phase) {
        unsigned mode = occluded && phase ? phase - 1 : phase;
        bool covered = occluded && !phase;
        if (occluded && phase == 1) {
            gpu->build_radar(device.Get(), &b, clean.get());
            assert(!b.radar->overlay_mask[30 * RADAR_SIDE + 30]);
        }
        FLOAT clear[4] = {1, 0, 1, 1};
        context->ClearRenderTargetView(rtv.Get(), clear);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp = {0, 0, (FLOAT)(640 * scale), (FLOAT)(480 * scale), 0, 1};
        context->RSSetViewports(1, &vp);
        context->RSSetState(gpu->resources_.raster.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(gpu->resources_.vs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, gpu->resources_.placement.GetAddressOf());
        context->OMSetBlendState(gpu->resources_.blend.Get(), nullptr, UINT_MAX);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        b.radar->valid = mode != 2;
        b.radar->overlay_mask[(RADAR_SIDE - 1) * RADAR_SIDE] = mode != 3;
        if (mode == 4) {
            b.radar.reset();
        }
        bool drawn = gpu->draw_radar(context.Get(), &a, &b, mode == 1 ? SNAP_GAP : SNAP_NONE, .5, 640 * scale,
                                     480 * scale);
        assert(drawn == (mode == 0));
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};
        assert(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        for (unsigned y = 0; y < 128 * scale; ++y) {
            const DWORD *row = (const DWORD *)((const BYTE *)map.pData + y * map.RowPitch);
            for (unsigned x = 0; x < 128 * scale; ++x) {
                if (mode) {
                    assert(row[x] == 0xffff00ff); // fallback performs zero writes
                } else {
                    assert(row[x] != 0xffff00ff); // entire region replaced
                    if (pointers && x >= 80 * scale && y >= 45 * scale && y < 85 * scale) {
                        double lx = (x + .5) / scale;
                        double ly = (y + .5) / scale;
                        double origin = 1023 + distance * .5;
                        int wx = (int)floor(origin * 3 / 64 + lx - 1);
                        int wy = (int)floor(origin * 3 / 64 + ly - 1);
                        DWORD expected = x >= 116 * scale            ? 0x00ff00
                                         : terrain(wx, wy) == 0xffff ? 0xffffff
                                                                     : 0x0000ff;
                        double mobile = (2400 + origin * .25 - origin) / 21 - 4;
                        if (lx >= mobile && lx < mobile + 8 && ly >= mobile && ly < mobile + 8) {
                            expected = 0xffff00;
                        }
                        double mx = (2820 - origin) / 21 - 1;
                        double my = (2400 - origin) / 21 - 8;
                        if (lx >= mx && lx < mx + 8 && ly >= my && ly < my + 8) {
                            expected = 0xff0000;
                        }
                        for (unsigned i = 0; i < RADAR_POINTERS; ++i) {
                            const radar_pointer &pointer = now->pointers[i];
                            /* Independent oracle: halfway angle 85.25 degrees. */
                            double radians = 85.25 * 3.14159265358979323846 / 180;
                            double px =
                                pointer.valid ? 58 + pointer.radius * sin(radians) : pointer.dest.left;
                            double py = pointer.valid ? 58 - pointer.radius * cos(radians) : pointer.dest.top;
                            int ax = (int)floor(lx - px);
                            int ay = (int)floor(ly - py);
                            if (ax >= 0 && ax < 13 && ay >= 0 && ay < 13 && (ay * 13 + ax) % 7) {
                                expected = i ? 0xffffff : 0xff0000;
                            }
                        }
                        assert((row[x] & 0xffffff) == expected);
                    }
                    if (x < 8 * scale || y < 20 * scale) {
                        assert(row[x] == (switching ? 0xff00ffff : 0xff00ff00)); // current frame art
                    }
                    if (x >= 10 * scale && x < 60 * scale && y >= 22 * scale && y < 60 * scale) {
                        double origin = 1023 + distance * .5;
                        int wx = (int)floor(origin * 3 / 64 + (x + .5) / scale - 1);
                        int wy = (int)floor(origin * 3 / 64 + (y + .5) / scale - 1);
                        DWORD expected = terrain(wx, wy) == 0xffff ? 0xffffff : 0x0000ff;
                        if (covered && x >= 30 * scale && x < 40 * scale && y >= 30 * scale &&
                            y < 40 * scale) {
                            expected = 0x00ffff;
                        }
                        assert((row[x] & 0xffffff) == expected);
                    }
                }
            }
            assert(row[128 * scale] == 0xffff00ff); // no writes outside radar
        }
        // Independent world-to-screen marker positions at alpha=.5.
        if (!mode) {
            double origin = 1023 + distance * .5;
            unsigned x = (unsigned)(((2400 + origin * .25 - origin) / 21) * scale);
            unsigned y = x;
            const DWORD *row = (const DWORD *)((const BYTE *)map.pData + y * map.RowPitch);
            assert((row[x] & 0xffffff) == 0xffff00);
            unsigned port_x = (unsigned)(((2820 - origin) / 21 + 2) * scale);
            unsigned port_y = (unsigned)(((2400 - origin) / 21 - 4) * scale);
            row = (const DWORD *)((const BYTE *)map.pData + port_y * map.RowPitch);
            assert((row[port_x] & 0xffffff) == 0xff0000);
        }
        context->Unmap(staging.Get(), 0);
    }
    gpu->release();
    assert(gpu->radar_drawn_ == (occluded ? 2u : 1u) && gpu->radar_fallback_ == 4);
    assert(gpu->radar_reasons_[RADAR_SNAP] == 1);
    assert(gpu->radar_reasons_[RADAR_INCOMPLETE] == 1);
    assert(gpu->radar_reasons_[RADAR_GPU] == 1);
    assert(gpu->radar_reasons_[RADAR_COVERAGE] == 1);
    if (occluded) {
        assert(gpu->radar_reasons_[RADAR_OCCLUDED] == 0);
        printf("radar WARP x%u: brief UI occlusion, moving markers, clean recovery without fallback OK\n",
               scale);
    }
    if (pointers) {
        printf("radar WARP x%u mode=%u: fractional pointers, green key, frame/order, no old pixels/fallback "
               "double draw OK\n",
               scale, pointers);
    }
    printf("radar WARP x%u step=%.2f: full replacement, frame, ship/port positions and no-write fallback OK\n",
           scale, distance);
}

int main()
{
    check_geometry();
    check_pointer_geometry();
    check_skipped_partial();
    check_marker_changes();
    for (double speed : {.5, 192.25, 256.5, 319.75}) {
        check_speed(speed, 1);
        check_speed(speed, -1);
    }
    for (unsigned scale : {1u, 2u, 4u}) {
        check_warp(scale, 2);
        check_warp(scale, 320);
        check_warp(scale, 2, true);
        check_warp(scale, 2, false, true);
        check_warp(scale, 2, true, false, true);
        for (unsigned mode = 1; mode <= 3; ++mode) {
            check_warp(scale, 2, false, false, false, mode);
        }
    }
}
