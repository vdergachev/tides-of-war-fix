#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <d3d11.h>
#include "scene.h"
#include "scene_gpu.h"
#include "cloud_art.h"
#include <stdlib.h>
#include "scene_vs.h"
#include "scene_base_ps.h"
#define tow_d3d11_ps tow_scene_layer_ps
#include "d3d11_ps.h"
#undef tow_d3d11_ps
#include "diag.h"
#include "game_speed.h"
#include <wrl/client.h>
#include <utility>
#include <new>

using Microsoft::WRL::ComPtr;

/* Require stable capture before restoring the delayed timeline. */
static const unsigned SCENE_RECOVERY_RECORDS = 3;

double SceneCompositor::ticks(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart;
}

double SceneCompositor::clamp(double value, double low, double high)
{
    return max(low, min(value, high));
}

DWORD SceneCompositor::color(WORD value)
{
    unsigned r = (value >> 11) & 31;
    unsigned g = (value >> 5) & 63;
    unsigned b = value & 31;
    return 0xff000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | (b << 3) | (b >> 2);
}

void SceneCompositor::release_entry(SceneEntry *entry)
{
    for (unsigned i = 0; i < REGION_COUNT; ++i) {
        entry->regions[i].view.Reset();
        entry->regions[i].texture.Reset();
    }
    for (unsigned i = 0; i < SCENE_DRAWS; ++i) {
        entry->draws[i].view.Reset();
        entry->draws[i].texture.Reset();
    }
}

void SceneCompositor::reset_timeline(void)
{
    if (wake_) {
        wake_->reset();
    }
    if (!initialized_) {
        return;
    }
    for (unsigned i = 0; i < count_; ++i) {
        release_entry(&resources_.entries[i]);
    }
    for (unsigned i = 0; i < SCENE_RING; ++i) {
        auto bottom = std::move(resources_.entries[i].wake_bottom);
        resources_.entries[i] = {};
        resources_.entries[i].wake_bottom = std::move(bottom);
    }
    count_ = 0;
    time_ = 0;
    now_ = 0;
    interval_ = 0;
    follow_ = 0;
    changed_pair_ = 0;
    motion_ = 0;
    valid_ = 0;
    complete_records_ = 0;
    stalled_ = false;
    stall_qpc_ = 0;
}

void SceneCompositor::check_stall(void)
{
    double latest = count_ ? resources_.entries[count_ - 1].qpc : stall_qpc_;
    if (!latest || !frequency_) {
        return;
    }
    double age = max(0, (ticks() - latest) * 1000.0 / frequency_);
    if (age > 2.0 * game_speed_step_delay()) {
        stall_max_ms_ = max(stall_max_ms_, age);
        if (!stalled_ || count_) {
            /* Drop the frozen timeline; smoothing resumes after K fresh complete records. */
            reset_timeline();
            stalled_ = true;
            stall_qpc_ = latest;
        }
    }
}

void SceneCompositor::release(void)
{
    if (!initialized_) {
        return;
    }
    reset_timeline();
    art_.release();
    wake_.reset();
    resources_.raster.Reset();
    resources_.blend.Reset();
    resources_.placement.Reset();
    resources_.vs.Reset();
    resources_.base_constants.Reset();
    resources_.base_ps.Reset();
    resources_.layer_ps.Reset();
    free(diagnostics_);
    diagnostics_ = nullptr;
    initialized_ = false;
}

int SceneCompositor::init(ID3D11Device *device)
{
    if (diag_enabled(DIAG_MASKS) || diag_enabled(DIAG_UI) || diag_enabled(DIAG_TRANSITIONS)) {
        diagnostics_ = static_cast<SceneDiagnostic *>(calloc(SCENE_RING + 1, sizeof(*diagnostics_)));
        if (!diagnostics_) {
            return 0;
        }
    }
    initialized_ = true;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    frequency_ = (double)frequency.QuadPart;
    HRESULT hr = device->CreateVertexShader(tow_scene_vs, sizeof(tow_scene_vs), NULL, &resources_.vs);
    D3D11_BUFFER_DESC buffer = {};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) {
        hr = device->CreateBuffer(&buffer, NULL, &resources_.placement);
    }
    buffer.ByteWidth = 96;
    if (SUCCEEDED(hr)) {
        hr = device->CreateBuffer(&buffer, NULL, &resources_.base_constants);
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(tow_scene_base_ps, sizeof(tow_scene_base_ps), NULL,
                                       &resources_.base_ps);
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(tow_scene_layer_ps, sizeof(tow_scene_layer_ps), NULL,
                                       &resources_.layer_ps);
    }
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) {
        hr = device->CreateBlendState(&blend, &resources_.blend);
    }
    D3D11_RASTERIZER_DESC raster = {D3D11_FILL_SOLID, D3D11_CULL_NONE};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE;
    raster.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) {
        hr = device->CreateRasterizerState(&raster, &resources_.raster);
    }
    if (FAILED(hr)) {
        release();
        diag_log("scene: GPU setup failed hr=0x%08lx; plain frames only", (DWORD)hr);
        return 0;
    }
    art_.load(device, uploads_, upload_bytes_);
    if (wake_settings().enabled) {
        wake_.reset(new (std::nothrow) WakeLayer);
        if (!wake_ || !wake_->init(device)) {
            wake_.reset();
            diag_log("wake: setup failed; wake disabled");
        }
    }
    if (wake_) {
        for (auto &entry : resources_.entries) {
            entry.wake_bottom.reset(new (std::nothrow) WORD[SCENE_PIXELS]);
        }
    }
    return 1;
}

/* Replay proves completeness without guessing an owner from screen geometry.
 * Each source-key pixel remains transparent, even if its RGB equals the sea. */
int SceneCompositor::assign_masks(const scene_record *record)
{
    if (record->count > SCENE_SHIPS || record->draw_count > SCENE_DRAWS) {
        return 0;
    }
    int diagnostic = diag_enabled(DIAG_MASKS);
    unsigned overlaps[2] = {};
    unsigned draws[2] = {};
    if (diagnostic) {
        memset(diagnostics_[SCENE_RING].pairs, 0, sizeof(diagnostics_[SCENE_RING].pairs));
    }
    if (diagnostic) {
        memset(owner_, -2, sizeof(owner_));
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
        unsigned base = pass ? SCENE_A : SCENE_A0;
        unsigned final = pass ? SCENE_B : SCENE_A1;
        memcpy(replay_, record->pixels[base], sizeof(replay_));
        for (unsigned i = 0; i < record->draw_count; ++i) {
            const scene_draw *draw = &record->draws[i];
            if ((draw->ship >= record->count && draw->ship != SCENE_SHIPS) || draw->pass > 1 ||
                draw->width < 1 || draw->width > SCENE_SPRITE_SIZE || draw->height < 1 ||
                draw->height > SCENE_SPRITE_SIZE || draw->box.left < 0 || draw->box.top < 0 ||
                draw->box.right > SCENE_WIDTH || draw->box.bottom > SCENE_HEIGHT ||
                draw->box.right - draw->box.left != draw->width ||
                draw->box.bottom - draw->box.top != draw->height) {
                return 0;
            }
            if (draw->pass != pass) {
                continue;
            }
            ++draws[pass];
            for (int y = 0; y < draw->height; ++y) {
                for (int x = 0; x < draw->width; ++x) {
                    WORD value = draw->pixels[y * draw->width + x];
                    if (value == draw->key) {
                        continue;
                    }
                    int p = (draw->box.top + y) * SCENE_WIDTH + draw->box.left + x;
                    if (diagnostic) {
                        if (owner_[pass][p] >= 0 && owner_[pass][p] != (int)draw->ship) {
                            ++overlaps[pass];
                        }
                        owner_[pass][p] = (signed char)draw->ship;
                    }
                    replay_[p] = value;
                    ++mask_pixels_[pass ? 2 : 0];
                }
            }
        }
        for (int y = record->view.top; y < record->view.bottom; ++y) {
            for (int x = record->view.left; x < record->view.right; ++x) {
                int p = y * SCENE_WIDTH + x;
                if (replay_[p] != record->pixels[final][p]) {
                    diag_line(DIAG_MASKS, "sprite_incomplete step=%u pass=%u x=%d y=%d; plain frame",
                              record->step, pass, x, y);
                    return 0;
                }
                if (diagnostic) {
                    int owner = owner_[pass][p];
                    if (owner >= 0 && owner < SCENE_SHIPS) {
                        ++diagnostics_[SCENE_RING].pairs[pass][owner][owner];
                    }
                }
            }
        }
    }
    if (diagnostic) {
        DWORD now = GetTickCount();
        int overlap = overlaps[0] || overlaps[1];
        if (overlap != diagnostic_overlap_ || (DWORD)(now - diagnostic_mask_time_) >= 1000) {
            diagnostic_mask_time_ = now;
            diag_line(DIAG_MASKS,
                      "overlap_mask_pixels step=%u hull=%u masts=%u exact=1 hull_draws=%u mast_draws=%u",
                      record->step, overlaps[0], overlaps[1], draws[0], draws[1]);
        }
        diagnostic_overlap_ = overlap;
    }
    return 1;
}

int SceneCompositor::region_pixel(const scene_record *record, unsigned region, int p)
{
    if (region == REGION_BOTTOM || region == REGION_FINAL || region == REGION_EDGE) {
        return 1;
    }
    if (region == REGION_MID) {
        return record->pixels[SCENE_A][p] != record->pixels[SCENE_A1][p];
    }
    if (region == REGION_TOP) {
        return record->pixels[SCENE_C][p] != record->pixels[split_clouds_ ? SCENE_E : SCENE_B][p];
    }
    if (region == REGION_PRECLOUD) {
        return split_clouds_ && record->pixels[SCENE_D][p] != record->pixels[SCENE_B][p];
    }
    return 0;
}

unsigned SceneCompositor::source_layer(unsigned region)
{
    if (region == REGION_BOTTOM) {
        return SCENE_A0;
    }
    if (region == REGION_FINAL || region == REGION_TOP) {
        return SCENE_C;
    }
    if (region == REGION_PRECLOUD) {
        return SCENE_D;
    }
    if (region == REGION_MID) {
        return SCENE_A;
    }
    return SCENE_A0;
}

void SceneCompositor::include_pixel(unsigned index, int x, int y)
{
    RECT *box = &bounds_[index];
    box->left = min(box->left, x);
    box->top = min(box->top, y);
    box->right = max(box->right, x + 1);
    box->bottom = max(box->bottom, y + 1);
}

void SceneCompositor::find_bounds(const scene_record *record)
{
    for (unsigned i = 0; i < REGION_COUNT; ++i) {
        SetRect(&bounds_[i], SCENE_WIDTH, SCENE_HEIGHT, 0, 0);
    }
    SetRect(&bounds_[REGION_BOTTOM], 0, 0, SCENE_WIDTH, SCENE_HEIGHT);
    bounds_[REGION_FINAL] = bounds_[REGION_BOTTOM];
    bounds_[REGION_EDGE] = bounds_[REGION_BOTTOM];
    for (int y = record->view.top; y < record->view.bottom; ++y) {
        for (int x = record->view.left; x < record->view.right; ++x) {
            int p = y * SCENE_WIDTH + x;
            if (region_pixel(record, REGION_MID, p)) {
                include_pixel(REGION_MID, x, y);
                ++mask_pixels_[1];
            }
            if (region_pixel(record, REGION_PRECLOUD, p)) {
                include_pixel(REGION_PRECLOUD, x, y);
            }
            if (region_pixel(record, REGION_TOP, p)) {
                include_pixel(REGION_TOP, x, y);
                ++mask_pixels_[3];
            }
        }
    }
}

int SceneCompositor::build_region(ID3D11Device *device, SceneEntry *entry, const scene_record *record,
                                  unsigned index)
{
    /* Most records have no mid layer; share A0 instead of uploading it twice. */
    if (index == REGION_EDGE && IsRectEmpty(&bounds_[REGION_MID])) {
        entry->regions[index] = entry->regions[REGION_BOTTOM];
        return 1;
    }
    RECT bounds = bounds_[index];
    unsigned layer = source_layer(index);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return 1;
    }
    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int p = (y + bounds.top) * SCENE_WIDTH + x + bounds.left;
            unsigned pixel_layer =
                index == REGION_EDGE
                    ? (record->pixels[SCENE_A][p] != record->pixels[SCENE_A1][p] ? SCENE_A : SCENE_A0)
                    : layer;
            pixels_[y * width + x] =
                region_pixel(record, index, p) ? color(record->pixels[pixel_layer][p]) : 0;
        }
    }
    return upload_pixels(device, &entry->regions[index], bounds);
}

int SceneCompositor::build_draw(ID3D11Device *device, SceneEntry *entry, const scene_record *record,
                                unsigned index)
{
    const scene_draw *draw = &record->draws[index];
    entry->draw_ship[index] = draw->ship;
    entry->draw_pass[index] = draw->pass;
    for (int p = 0; p < draw->width * draw->height; ++p) {
        pixels_[p] = draw->pixels[p] == draw->key ? 0 : color(draw->pixels[p]);
    }
    return upload_pixels(device, &entry->draws[index], draw->box);
}

int SceneCompositor::upload_pixels(ID3D11Device *device, Texture2D *out, RECT bounds)
{
    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = pixels_;
    data.SysMemPitch = (UINT)width * 4;
    out->box = bounds;
    ++uploads_;
    upload_bytes_ += (unsigned long long)width * height * 4;
    HRESULT hr = device->CreateTexture2D(&desc, &data, &out->texture);
    if (SUCCEEDED(hr)) {
        hr = device->CreateShaderResourceView(out->texture.Get(), NULL, &out->view);
    }
    return SUCCEEDED(hr);
}

void SceneCompositor::discard_first(void)
{
    auto bottom = std::move(resources_.entries[0].wake_bottom);
    release_entry(&resources_.entries[0]);
    --count_;
    for (unsigned i = 0; i < count_; ++i) {
        resources_.entries[i] = std::move(resources_.entries[i + 1]);
        if (diagnostics_) {
            diagnostics_[i] = diagnostics_[i + 1];
        }
    }
    resources_.entries[count_] = {};
    resources_.entries[count_].wake_bottom = std::move(bottom);
}

void SceneCompositor::update(ID3D11Device *device, ID3D11DeviceContext *context, LONG epoch)
{
    (void)context;
    if (!initialized_) {
        return;
    }
    if (epoch_ != epoch) {
        reset_timeline();
        epoch_ = epoch;
    }
    check_stall();
    while (queue_.take()) {
        if (queue_.input_->epoch != epoch) {
            continue;
        }
        if (!queue_.input_->valid) {
            reset_timeline();
            continue;
        }
        if (!resources_.vs) {
            continue;
        }
        if (count_) {
            SceneEntry *last = &resources_.entries[count_ - 1];
            if (queue_.input_->step <= last->step || (double)queue_.input_->qpc <= last->qpc) {
                ++rejected_;
                complete_records_ = 0;
                continue;
            }
            if (queue_.input_->step != last->step + 1) {
                complete_records_ = 0;
            }
            double sample = ((double)queue_.input_->qpc - last->qpc) / (queue_.input_->step - last->step);
            interval_ = interval_ ? interval_ + .1 * (sample - interval_) : sample;
        }
        if (count_ == SCENE_RING) {
            discard_first();
        }
        SceneEntry *out = &resources_.entries[count_];
        out->step = queue_.input_->step;
        out->count = queue_.input_->count;
        out->follow = queue_.input_->follow;
        out->camera = queue_.input_->camera;
        out->view = queue_.input_->view;
        out->qpc = (double)queue_.input_->qpc;
        memcpy(out->ships, queue_.input_->ships, sizeof(out->ships));
        if (wake_) {
            if (!out->wake_bottom) {
                out->wake_bottom.reset(new (std::nothrow) WORD[SCENE_PIXELS]);
            }
            if (out->wake_bottom) {
                memcpy(out->wake_bottom.get(), queue_.input_->pixels[SCENE_A0], SCENE_PIXELS * sizeof(WORD));
            }
        }
        out->cloud_flags = queue_.input_->cloud_flags;
        memcpy(out->clouds, queue_.input_->clouds, sizeof(out->clouds));
        split_clouds_ = art_.ready(queue_.input_);
        out->split_clouds = split_clouds_;
        if (!assign_masks(queue_.input_)) {
            reset_timeline();
            continue;
        }
        out->draw_count = queue_.input_->draw_count;
        if (diag_enabled(DIAG_MASKS)) {
            memcpy(diagnostics_[count_].pairs, diagnostics_[SCENE_RING].pairs,
                   sizeof(diagnostics_[SCENE_RING].pairs));
        }
        if (diag_enabled(DIAG_UI) || diag_enabled(DIAG_TRANSITIONS)) {
            diagnostics_[count_].frame = *scene_diagnostic(queue_.input_);
        }
        find_bounds(queue_.input_);
        int ok = 1;
        for (unsigned i = 0; i < REGION_COUNT && ok; ++i) {
            ok = build_region(device, out, queue_.input_, i);
        }
        for (unsigned i = 0; i < out->draw_count && ok; ++i) {
            ok = build_draw(device, out, queue_.input_, i);
        }
        if (!ok) {
            release_entry(out);
            reset_timeline();
            diag_log("scene: texture creation failed; plain frame");
            continue;
        }
        ++count_;
        if (stalled_) {
            stall_qpc_ = out->qpc;
        }
        ++sets_;
        if (complete_records_ < SCENE_RECOVERY_RECORDS) {
            ++complete_records_;
        }
        if (!valid_) {
            valid_ = complete_records_ >= SCENE_RECOVERY_RECORDS;
            if (valid_) {
                stalled_ = false;
            }
            if (!valid_) {
                ++held_records_;
            }
        }
    }
    check_stall();
}

const scene_ship *SceneCompositor::find_ship(const SceneEntry *entry, uintptr_t ptr)
{
    for (unsigned i = 0; i < entry->count; ++i) {
        if (entry->ships[i].ptr == ptr) {
            return &entry->ships[i];
        }
    }
    return NULL;
}

void SceneCompositor::camera_fraction(const SceneEntry *entry, uintptr_t follow, double *camera)
{
    camera[0] = entry->camera.x;
    camera[1] = entry->camera.y;
    const scene_ship *ship = find_ship(entry, follow);
    if (ship) {
        double fx = ship->x - ship->point.x;
        double fy = ship->y - ship->point.y;
        camera[0] += fabs(fx) <= .500001 ? fx : 0;
        camera[1] += fabs(fy) <= .500001 ? fy : 0;
    }
}

int SceneCompositor::classify(const SceneEntry *before, const SceneEntry *entry, double *cam0, double *cam1)
{
    if (!before) {
        return SNAP_PREV;
    }
    if (entry->step != before->step + 1 || entry->qpc <= before->qpc ||
        memcmp(&entry->view, &before->view, sizeof(RECT)) != 0) {
        return SNAP_GAP;
    }
    uintptr_t follow = 0;
    double nearest = 1e30;
    for (unsigned i = 0; i < entry->count; ++i) {
        const scene_ship *ship = &entry->ships[i];
        const scene_ship *old = find_ship(before, ship->ptr);
        if (!old || (entry->follow != 0xffff && entry->follow != ship->id) ||
            ship->point.x - entry->camera.x != old->point.x - before->camera.x ||
            ship->point.y - entry->camera.y != old->point.y - before->camera.y) {
            continue;
        }
        double dx = ship->point.x - entry->camera.x - (entry->view.right - entry->view.left) * .5;
        double dy = ship->point.y - entry->camera.y - (entry->view.bottom - entry->view.top) * .5;
        double distance = ship->ptr == follow_ ? -1 : dx * dx + dy * dy;
        if (distance < nearest) {
            nearest = distance;
            follow = ship->ptr;
        }
    }
    if (!follow) {
        return SNAP_FOLLOW;
    }
    camera_fraction(before, follow, cam0);
    camera_fraction(entry, follow, cam1);
    int changed = follow_ && follow != follow_;
    follow_ = follow;
    if (changed || (before->follow != entry->follow)) {
        changed_pair_ = entry->qpc;
    }
    if (changed_pair_ == entry->qpc) {
        return SNAP_CHANGE;
    }
    if (abs(entry->camera.x - before->camera.x) > 16 || abs(entry->camera.y - before->camera.y) > 16 ||
        fabs(cam1[0] - cam0[0]) > 12 || fabs(cam1[1] - cam0[1]) > 12) {
        return SNAP_JUMP;
    }
    return SNAP_NONE;
}

void SceneCompositor::draw_sprite(ID3D11DeviceContext *context, const Texture2D *region, double dx, double dy,
                                  RECT clip, UINT width, UINT height)
{
    if (!region->view) {
        return;
    }
    FLOAT placement[4] = {(FLOAT)((region->box.left + dx) / SCENE_WIDTH),
                          (FLOAT)((region->box.top + dy) / SCENE_HEIGHT),
                          (FLOAT)(region->box.right - region->box.left) / SCENE_WIDTH,
                          (FLOAT)(region->box.bottom - region->box.top) / SCENE_HEIGHT};
    RECT physical = {(LONG)floor((double)clip.left * width / SCENE_WIDTH + .5),
                     (LONG)floor((double)clip.top * height / SCENE_HEIGHT + .5),
                     (LONG)floor((double)clip.right * width / SCENE_WIDTH + .5),
                     (LONG)floor((double)clip.bottom * height / SCENE_HEIGHT + .5)};
    context->RSSetScissorRects(1, &physical);
    context->UpdateSubresource(resources_.placement.Get(), 0, NULL, placement, 0, 0);
    context->PSSetShaderResources(0, 1, region->view.GetAddressOf());
    context->Draw(4, 0);
    ++draws_;
}

void SceneCompositor::draw_region(ID3D11DeviceContext *context, const SceneEntry *entry, unsigned index,
                                  double dx, double dy, RECT clip, UINT width, UINT height)
{
    draw_sprite(context, &entry->regions[index], dx, dy, clip, width, height);
}

int SceneCompositor::cloud_snap(const SceneEntry *before, const SceneEntry *entry, unsigned i)
{
    const scene_cloud *cloud = &entry->clouds[i];
    if (!before || !before->split_clouds || before->cloud_flags != entry->cloud_flags) {
        return 1;
    }
    const scene_cloud *old = &before->clouds[i];
    return !old->valid || !old->ticks || cloud->ticks != old->ticks - 1 || cloud->image != old->image ||
           cloud->width != old->width || cloud->height != old->height || cloud->vx != old->vx ||
           cloud->vy != old->vy || fabs(cloud->x - old->x - cloud->vx) > 1e-6 ||
           fabs(cloud->y - old->y - cloud->vy) > 1e-6;
}

void SceneCompositor::draw_clouds(ID3D11DeviceContext *context, const SceneEntry *before,
                                  const SceneEntry *entry, int reason, double alpha, const double *shift,
                                  UINT width, UINT height)
{
    if (!(entry->cloud_flags & 8)) {
        return;
    }
    if (!entry->split_clouds) {
        ++cloud_fallback_;
        return;
    }
    draw_region(context, entry, REGION_PRECLOUD, 0, 0, entry->view, width, height);
    for (unsigned i = 0; i < SCENE_CLOUDS; ++i) {
        const scene_cloud *cloud = &entry->clouds[i];
        int snap = reason || cloud_snap(before, entry, i);
        double dx = reason ? 0 : shift[0];
        double dy = reason ? 0 : shift[1];
        if (!snap) {
            const scene_cloud *old = &before->clouds[i];
            double x = old->x + (cloud->x - old->x) * alpha;
            double y = old->y + (cloud->y - old->y) * alpha;
            /* Preserve the game's render/world camera offset and rounding. */
            dx += x - (cloud->x < 0 ? ceil(cloud->x - .5) : floor(cloud->x + .5));
            dy += y - (cloud->y < 0 ? ceil(cloud->y - .5) : floor(cloud->y + .5));
        }
        const Texture2D *art = &art_.image(cloud->image + ((entry->cloud_flags & 2) ? 18 : 0));
        draw_sprite(context, art, cloud->dest.x + dx, cloud->dest.y + dy, entry->view, width, height);
        ++cloud_drawn_;
        if (snap) {
            ++cloud_snapped_;
        } else {
            ++cloud_interpolated_;
        }
    }
}

void SceneCompositor::observe_motion(const SceneEntry *before, const SceneEntry *entry, const double *camera,
                                     const double *cam0, const double *cam1, double positions[SCENE_SHIPS][2])
{
    if (motion_) {
        double dot = (camera[0] - last_camera_[0]) * (cam1[0] - cam0[0]) +
                     (camera[1] - last_camera_[1]) * (cam1[1] - cam0[1]);
        if (dot < -1e-6) {
            ++backward_;
        }
    }
    for (unsigned i = 0; i < entry->count; ++i) {
        const scene_ship *ship = &entry->ships[i];
        const scene_ship *old = find_ship(before, ship->ptr);
        if (motion_ && old) {
            for (unsigned j = 0; j < SCENE_SHIPS; ++j) {
                if (last_ptr_[j] != ship->ptr) {
                    continue;
                }
                double dot = (positions[i][0] - last_ship_[j][0]) * (ship->x - old->x - cam1[0] + cam0[0]) +
                             (positions[i][1] - last_ship_[j][1]) * (ship->y - old->y - cam1[1] + cam0[1]);
                if (dot < -1e-6) {
                    ++backward_;
                }
            }
        }
    }
    memset(last_ptr_, 0, sizeof(last_ptr_));
    for (unsigned i = 0; i < entry->count; ++i) {
        last_ptr_[i] = entry->ships[i].ptr;
        memcpy(last_ship_[i], positions[i], sizeof(last_ship_[i]));
    }
    memcpy(last_camera_, camera, sizeof(last_camera_));
    motion_ = 1;
}

void SceneCompositor::draw_base(ID3D11DeviceContext *context, const SceneEntry *entry, const SceneEntry *edge,
                                const double *camera, const double *shift, UINT width, UINT height)
{
    /* Current bottom over previous bottom/mid, C outside view.
     * Physical rasterization preserves fractional motion at every scale. */

    FLOAT constants[24] = {(FLOAT)entry->view.left,
                           (FLOAT)entry->view.top,
                           (FLOAT)entry->view.right,
                           (FLOAT)entry->view.bottom,
                           (FLOAT)shift[0],
                           (FLOAT)shift[1],
                           (FLOAT)(edge->camera.x - camera[0]),
                           (FLOAT)(edge->camera.y - camera[1]),
                           (FLOAT)SCENE_WIDTH / (FLOAT)width,
                           (FLOAT)SCENE_HEIGHT / (FLOAT)height};
    if (wake_ready_) {
        wake_->bind(context, constants + 16);
        constants[20] = (FLOAT)entry->camera.x;
        constants[21] = (FLOAT)entry->camera.y;
    }
    RECT physical = {0, 0, (LONG)width, (LONG)height};
    ID3D11ShaderResourceView *base_views[] = {
        entry->regions[REGION_BOTTOM].view.Get(), edge->regions[REGION_EDGE].view.Get(),
        entry->regions[REGION_FINAL].view.Get(), entry->regions[REGION_EDGE].view.Get()};
    FLOAT placement[4] = {0, 0, 1, 1};
    context->UpdateSubresource(resources_.placement.Get(), 0, NULL, placement, 0, 0);
    context->UpdateSubresource(resources_.base_constants.Get(), 0, NULL, constants, 0, 0);
    context->PSSetConstantBuffers(0, 1, resources_.base_constants.GetAddressOf());
    context->RSSetScissorRects(1, &physical);
    context->OMSetBlendState(NULL, NULL, 0xffffffff);
    context->PSSetShader(wake_ready_ ? wake_->shader() : resources_.base_ps.Get(), NULL, 0);
    context->PSSetShaderResources(0, 4, base_views);
    context->Draw(4, 0);
    ++draws_;
    ID3D11ShaderResourceView *empty[6] = {};
    context->PSSetShaderResources(0, 6, empty);
    context->PSSetShader(resources_.layer_ps.Get(), NULL, 0);
    context->OMSetBlendState(resources_.blend.Get(), NULL, 0xffffffff);
}

int SceneCompositor::draw(ID3D11DeviceContext *context, UINT width, UINT height, LONG epoch)
{
    if (!count_ || !valid_ || epoch != epoch_) {
        motion_ = 0;
        return 0;
    }
    double start = ticks();
    double elapsed = now_ ? max(0, start - now_) : 0;
    now_ = start;
    if (!time_) {
        time_ = resources_.entries[0].qpc;
    }
    double interval = interval_ ? interval_ : frequency_ * .078;
    double target = start - interval;
    double rate = clamp(1 + (target - time_) / (4 * interval), .75, 1.25);
    time_ = max(time_, min(resources_.entries[count_ - 1].qpc, time_ + elapsed * rate));
    while (count_ > 2 && resources_.entries[1].qpc <= time_) {
        discard_first();
    }
    unsigned current = count_ - 1;
    for (unsigned i = 1; i < count_; ++i) {
        if (resources_.entries[i].qpc >= time_) {
            current = i;
            break;
        }
    }
    const SceneEntry *entry = &resources_.entries[current];
    if (diag_enabled(DIAG_UI) || diag_enabled(DIAG_TRANSITIONS)) {
        diagnostics_[SCENE_RING].frame = diagnostics_[entry - resources_.entries].frame;
    }
    if (diag_enabled(DIAG_UI) && (DWORD)(GetTickCount() - diagnostic_ui_time_) >= 500) {
        diagnostic_ui_time_ = GetTickCount();
        diag_line(DIAG_UI,
                  "ui_selected_C ui_serial=%ld C_serial=%ld C_step=%u ui_hash=%08x view=(%ld,%ld,%ld,%ld) "
                  "output=%ux%u",
                  queue_.diag_snapshot_.serial, diagnostics_[entry - resources_.entries].frame.serial,
                  entry->step, diagnostics_[entry - resources_.entries].frame.ui_hash, entry->view.left,
                  entry->view.top, entry->view.right, entry->view.bottom, width, height);
    }
    const SceneEntry *before = current ? &resources_.entries[current - 1] : NULL;
    double cam0[2] = {};
    double cam1[2] = {};
    int reason = classify(before, entry, cam0, cam1);
    /* classify returns SNAP_PREV for null before; the analyzer loses that relation. */
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    double alpha = reason ? 1 : clamp((time_ - before->qpc) / (entry->qpc - before->qpc), 0, 1);
    double camera[2] = {cam0[0] + (cam1[0] - cam0[0]) * alpha, cam0[1] + (cam1[1] - cam0[1]) * alpha};
    double shift[2] = {entry->camera.x - camera[0], entry->camera.y - camera[1]};
    ++frames_;
    ++snaps_[reason];
    if (reason) {
        shift[0] = 0;
        shift[1] = 0;
        camera[0] = entry->camera.x;
        camera[1] = entry->camera.y;
        motion_ = 0;
    }
    context->VSSetShader(resources_.vs.Get(), NULL, 0);
    context->VSSetConstantBuffers(0, 1, resources_.placement.GetAddressOf());
    context->OMSetBlendState(resources_.blend.Get(), NULL, 0xffffffff);
    context->RSSetState(resources_.raster.Get());
    wake_ready_ = false;
    if (wake_ && entry->wake_bottom) {
        WakePose poses[SCENE_SHIPS] = {};
        for (unsigned i = 0; i < entry->count; ++i) {
            const scene_ship &ship = entry->ships[i];
            const scene_ship *old = before ? find_ship(before, ship.ptr) : nullptr;
            double dx = 0;
            double dy = 0;
            if (!reason && old && ship.ptr != follow_) {
                dx = old->x + (ship.x - old->x) * alpha - camera[0] - (ship.point.x - entry->camera.x);
                dy = old->y + (ship.y - old->y) * alpha - camera[1] - (ship.point.y - entry->camera.y);
            }
            poses[i] = WakeLayer::placed_pose(ship, dx, dy, entry->camera, camera);
        }
        wake_ready_ = wake_->update(context, entry->view, entry->camera, camera, poses, entry->count,
                                    entry->follow, entry->wake_bottom.get(), time_ / frequency_, reason != 0);
    }
    draw_base(context, entry, reason ? entry : before, camera, shift, width, height);
    double positions[SCENE_SHIPS][2] = {{0}};
    DWORD diagnostic_now = diag_enabled(DIAG_MASKS) ? GetTickCount() : 0;
    int diagnostic_owners =
        diag_enabled(DIAG_MASKS) && (DWORD)(diagnostic_now - diagnostic_owner_time_) >= 2000;
    if (diagnostic_owners) {
        diagnostic_owner_time_ = diagnostic_now;
    }
    double offsets[SCENE_SHIPS][2] = {};
    for (unsigned pass = 0; pass < 2; ++pass) {
        if (pass) {
            draw_region(context, entry, REGION_MID, shift[0], shift[1], entry->view, width, height);
        }
        for (unsigned i = 0; i < entry->count; ++i) {
            const scene_ship *ship = &entry->ships[i];
            const scene_ship *old = before ? find_ship(before, ship->ptr) : NULL;
            double dx = 0;
            double dy = 0;
            if (!reason && old && ship->ptr != follow_) {
                dx = old->x + (ship->x - old->x) * alpha - camera[0] - (ship->point.x - entry->camera.x);
                dy = old->y + (ship->y - old->y) * alpha - camera[1] - (ship->point.y - entry->camera.y);
            }
            positions[i][0] = ship->point.x - entry->camera.x + dx;
            positions[i][1] = ship->point.y - entry->camera.y + dy;
            if (diagnostic_owners) {
                for (unsigned drawing = 0; drawing <= SCENE_SHIPS; ++drawing) {
                    unsigned pixels = diagnostics_[entry - resources_.entries].pairs[pass][i][drawing];
                    if (pixels) {
                        int drawing_id = drawing < entry->count ? (int)entry->ships[drawing].id : -1;
                        diag_line(DIAG_MASKS,
                                  "mask_owner step=%u pass=%u drawing_id=%d assigned_id=%u pixels=%u dx=%.3f "
                                  "dy=%.3f",
                                  entry->step, pass, drawing_id, ship->id, pixels, dx, dy);
                    }
                }
            }
            offsets[i][0] = dx;
            offsets[i][1] = dy;
        }
        for (unsigned i = 0; i < entry->draw_count; ++i) {
            if (entry->draw_pass[i] == pass) {
                unsigned ship = entry->draw_ship[i];
                double dx = ship == SCENE_SHIPS ? shift[0] : offsets[ship][0];
                double dy = ship == SCENE_SHIPS ? shift[1] : offsets[ship][1];
                draw_sprite(context, &entry->draws[i], dx, dy, entry->view, width, height);
            }
        }
    }
    draw_clouds(context, before, entry, reason, alpha, shift, width, height);
    draw_region(context, entry, REGION_TOP, 0, 0, entry->view, width, height);
    if (!reason) {
        observe_motion(before, entry, camera, cam0, cam1, positions);
    }
    context->OMSetBlendState(NULL, NULL, 0xffffffff);
    context->RSSetState(NULL);
    double cost = (ticks() - start) * 1000 / frequency_;
    cost_ += cost;
    max_cost_ = max(max_cost_, cost);
    return 1;
}

int SceneCompositor::active(void)
{
    return valid_;
}

void SceneCompositor::counters(unsigned *draws, unsigned *uploads, unsigned long long *bytes)
{
    *draws = draws_;
    *uploads = uploads_;
    *bytes = upload_bytes_;
}

void SceneCompositor::log(void)
{
    if (wake_) {
        wake_->log();
    }
    diag_log("scene: stall_plain_interval=%u stall_max_ms=%.1f", stall_plain_, stall_max_ms_);
    stall_plain_ = 0;
    stall_max_ms_ = 0;
    diag_log("scene: recovery_held_records=%u recovery_complete_records=%u recovery_required=%u",
             held_records_, complete_records_, SCENE_RECOVERY_RECORDS);
    diag_log("scene: clouds_drawn=%u clouds_interpolated=%u clouds_snapped=%u cloud_art_loaded=%u "
             "cloud_fallback_frames=%u",
             cloud_drawn_, cloud_interpolated_, cloud_snapped_, art_.loaded, cloud_fallback_);
    unsigned uploads = uploads_ - last_uploads_;
    unsigned long long bytes = upload_bytes_ - last_bytes_;
    diag_log("scene: texture_sets_interval=%u texture_uploads_interval=%u uploads_record=%.2f "
             "upload_bytes_record=%.0f",
             sets_, uploads, sets_ ? (double)uploads / sets_ : 0, sets_ ? (double)bytes / sets_ : 0);
    last_uploads_ = uploads_;
    last_bytes_ = upload_bytes_;
    sets_ = 0;
    diag_log("scene: interpolated=%u rejected=%u backward_motion=%u render_ms_mean=%.3f render_ms_max=%.3f "
             "mask_pixels_hull=%u mid=%u masts=%u top=%u",
             snaps_[SNAP_NONE], rejected_, backward_, frames_ ? cost_ / frames_ : 0, max_cost_,
             mask_pixels_[0], mask_pixels_[1], mask_pixels_[2], mask_pixels_[3]);
    for (unsigned i = 1; i < SNAP_COUNT; ++i) {
        diag_log("scene: snapped reason=%s frames=%u", SNAP_NAMES[i], snaps_[i]);
    }
    diag_log("scene: render_frame_ms_mean=%.3f render_frame_ms_max=%.3f frames=%u (before vsync)",
             frame_count_ ? frame_cost_ / frame_count_ : 0, frame_max_, frame_count_);
}

void SceneCompositor::frame_begin(void)
{
    frame_start_ = ticks();
}

void SceneCompositor::frame_end(void)
{
    double cost = (ticks() - frame_start_) * 1000 / frequency_;
    frame_cost_ += cost;
    frame_max_ = max(frame_max_, cost);
    ++frame_count_;
}
