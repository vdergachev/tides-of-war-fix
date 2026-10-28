#include "wake_layer.h"
#include "diag.h"
#include "game_speed.h"
#include "scene_wake_ps.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

/* numpy.rint / Python round use ties-to-even, independently of the CRT mode. */
static int round_even(double value)
{
    double low = floor(value);
    double fraction = value - low;
    return (int)(low + (fraction > .5 || (fraction == .5 && fmod(low, 2) != 0)));
}

WakePose WakeLayer::placed_pose(const scene_ship &ship, double dx, double dy, POINT captured_camera,
                                const double *camera)
{
    /* Invert the exact interpolated hull placement, including the followed-ship anchor.
     * The captured rear hull support excludes transparent padding and sails. */
    return {ship.ptr,
            ship.id,
            ship.point.x + dx + camera[0] - captured_camera.x,
            ship.point.y + dy + camera[1] - captured_camera.y,
            ship.course,
            ship.wake_stern};
}

double WakeLayer::ticks()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart;
}

bool WakeLayer::texture(ID3D11Device *device, Texture2D &out, UINT width, UINT height, DXGI_FORMAT format)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    out.view.Reset();
    out.texture.Reset();
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &out.texture);
    if (SUCCEEDED(hr)) {
        hr = device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.view);
    }
    return SUCCEEDED(hr);
}

bool WakeLayer::init(ID3D11Device *device)
{
    settings_ = wake_settings();
    if (!settings_.enabled) {
        return false;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    frequency_ = (double)frequency.QuadPart;
    capacity_ = (unsigned)settings_.budget * (settings_.shared ? 1 : SCENE_SHIPS);
    particles_.reset(new (std::nothrow) Particle[capacity_]);
    owners_.reset(new (std::nothrow) Owner[capacity_]);
    HRESULT hr = device->CreatePixelShader(tow_scene_wake_ps, sizeof(tow_scene_wake_ps), nullptr, &shader_);
    return particles_ && owners_ && SUCCEEDED(hr) &&
           texture(device, foam_, SCENE_WIDTH, SCENE_HEIGHT, DXGI_FORMAT_R8_UNORM);
}

void WakeLayer::reset()
{
    alive_ = 0;
    start_ = 0;
    count_ = 0;
    tick_ = -1;
    stamp_ = -1;
    field_.clear();
}

const WakeLayer::History *WakeLayer::find(const WakePose &pose)
{
    for (unsigned i = 0; i < count_; ++i) {
        if (history_[i].pose.ptr == pose.ptr && history_[i].pose.id == pose.id) {
            return &history_[i];
        }
    }
    return nullptr;
}

void WakeLayer::emit(const WakePose &pose, const History *old, double stamp, bool snap, History &next)
{
    next = {pose, stamp, 0};
    if (!old) {
        return;
    }
    double dt = stamp - old->stamp;
    double dx = pose.x - old->pose.x;
    double dy = pose.y - old->pose.y;
    double distance = hypot(dx, dy);
    if (dt <= 0) {
        next = *old;
        return;
    }
    if (snap || dt > .5 || distance > 32) {
        return;
    }
    next.remainder = old->remainder;
    if (distance < 1e-6 || distance / dt < .25) {
        return;
    }
    double ux = sin(pose.course * 3.141592653589793 / 180);
    double uy = -cos(pose.course * 3.141592653589793 / 180);
    double intensity = min(1., distance / dt / 30);
    double remaining = 1.25 - old->remainder;
    unsigned emissions = remaining <= distance ? (unsigned)floor((distance - remaining) / 1.25) + 1 : 0;
    for (unsigned emission = 0; emission < emissions; ++emission) {
        double fraction = remaining / distance;
        double born = old->stamp + dt * fraction;
        double sx = old->pose.x + dx * fraction - ux * pose.stern;
        double sy = old->pose.y + dy * fraction - uy * pose.stern;
        for (int lane = 0; lane < 7; ++lane) {
            double side = (lane - 3) * (8. / 9) * (5. / 6);
            double back = ((lane * 3) % 7) / 7. * 1.25;
            unsigned index = (start_ + alive_) % capacity_;
            particles_[index] = {
                sx - ux * back - uy * side, sy - uy * back + ux * side, born, intensity, pose.ptr, pose.id};
            if (alive_ < capacity_) {
                ++alive_;
            } else {
                start_ = (start_ + 1) % capacity_;
            }
        }
        remaining += 1.25;
    }
    next.remainder = fmod(old->remainder + distance, 1.25);
}

bool WakeLayer::sea(WORD pixel)
{
    int r = pixel >> 11;
    int g = (pixel >> 5) & 63;
    int b = pixel & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return b > r + 8 && b > g * 1.08;
}

void WakeLayer::bubbles(RECT view, POINT camera, const WORD *bottom, double stamp)
{
    memset(foam_pixels_, 0, sizeof(foam_pixels_));
    /* Reverse traversal retains the newest budget particles for each identity.
     * Compact backwards in the ring without touching unread particles. */
    unsigned kept = 0;

    unsigned owner_count = 0;
    for (unsigned n = alive_; n > 0; --n) {
        Particle p = particles_[(start_ + n - 1) % capacity_];
        double age = (stamp - p.born) / 1.33;
        if (!settings_.shared) {
            unsigned owner = 0;
            while (owner < owner_count && (owners_[owner].ptr != p.ptr || owners_[owner].id != p.id)) {
                ++owner;
            }
            if (owner == owner_count) {
                owners_[owner_count++] = {p.ptr, p.id, 0};
            }
            if (++owners_[owner].count > (unsigned)settings_.budget) {
                continue;
            }
        }
        particles_[(start_ + alive_ - 1 - kept) % capacity_] = p;
        ++kept;
        double bx = p.x - camera.x + view.left;
        double by = p.y - camera.y + view.top;
        int ix = (int)floor(bx);
        int iy = (int)floor(by);
        if (age < 0 || age >= 1 || ix < view.left || ix >= view.right || iy < view.top || iy >= view.bottom ||
            (settings_.land && !sea(bottom[iy * SCENE_WIDTH + ix]))) {
            continue;
        }
        int x = round_even(bx);
        int y = round_even(by);
        int alpha = round_even(100 * p.strength * pow(1 - age, 3));
        static const int OFFSETS[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (unsigned j = 0; j < 5; ++j) {
            int xx = x + OFFSETS[j][0];
            int yy = y + OFFSETS[j][1];
            if (xx < 0 || xx >= SCENE_WIDTH || yy < 0 || yy >= SCENE_HEIGHT) {
                continue;
            }
            int index = yy * SCENE_WIDTH + xx;
            if (settings_.land && !sea(bottom[index])) {
                continue;
            }
            int aa = j ? (int)(alpha * .4) : alpha;
            foam_pixels_[index] = (BYTE)max(foam_pixels_[index], aa);
        }
    }
    start_ = (start_ + alive_ - kept) % capacity_;
    alive_ = kept;
}

bool WakeLayer::update(ID3D11DeviceContext *context, RECT view, POINT captured_camera, const double *camera,
                       const WakePose *poses, unsigned count, unsigned follow, const WORD *bottom,
                       double stamp, bool snap)
{
    double started = ticks();
    if (!simulate(view, captured_camera, camera, poses, count, follow, bottom, stamp, snap) ||
        !upload(context)) {
        return false;
    }
    frame_ms_ += (ticks() - started) * 1000 / frequency_;
    ++frames_;
    return true;
}

bool WakeLayer::simulate(RECT view, POINT captured_camera, const double *camera, const WakePose *poses,
                         unsigned count, unsigned follow, const WORD *bottom, double stamp, bool snap)
{
    /* Match deque expiry before emission, including its per-ship insertion order. */
    while (alive_ && stamp - particles_[start_].born >= 1.33) {
        start_ = (start_ + 1) % capacity_;
        --alive_;
    }
    if (!field_.align(view, camera, settings_.resolution)) {
        return false;
    }
    step_ = game_speed_step_delay() / 1000.;
    bool reset = tick_ < 0 || stamp < stamp_ || stamp - stamp_ > .5 || snap;
    if (reset) {
        tick_ = stamp;
        field_.clear();
    } else {
        while (tick_ + step_ <= stamp + 1e-9) {
            double begin = ticks();
            tick_ += step_;
            double fraction = max(0., min(1., (tick_ - stamp_) / max(1e-9, stamp - stamp_)));
            for (unsigned i = 0; i < count; ++i) {
                const WakePose &pose = poses[i];
                const History *old = find(pose);
                if (pose.id == follow || !old || hypot(pose.x - old->pose.x, pose.y - old->pose.y) > 32) {
                    continue;
                }
                double x = old->pose.x + fraction * (pose.x - old->pose.x);
                double y = old->pose.y + fraction * (pose.y - old->pose.y);
                double ux = sin(pose.course * 3.141592653589793 / 180);
                double uy = -cos(pose.course * 3.141592653589793 / 180);
                field_.inject(x, y, settings_.strength, settings_.resolution);
                field_.inject(x - ux * pose.stern, y - uy * pose.stern, settings_.strength,
                              settings_.resolution);
            }
            field_.step(settings_.damping);
            step_ms_ += (ticks() - begin) * 1000 / frequency_;
            ++steps_;
        }
    }
    History next[SCENE_SHIPS] = {};
    for (unsigned i = 0; i < count; ++i) {
        emit(poses[i], find(poses[i]), stamp, snap, next[i]);
    }
    memcpy(history_, next, sizeof(history_));
    count_ = count;
    stamp_ = stamp;
    fraction_ = max(0., min(1., (stamp - tick_) / step_));
    bubbles(view, captured_camera, bottom, stamp);

    return true;
}

bool WakeLayer::upload(ID3D11DeviceContext *context)
{
    D3D11_TEXTURE2D_DESC desc = {};
    if (ripple_.texture) {
        ripple_.texture->GetDesc(&desc);
    }
    if (!ripple_.view || desc.Width != (UINT)field_.width() || desc.Height != (UINT)field_.height()) {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!texture(device.Get(), ripple_, field_.width(), field_.height(), DXGI_FORMAT_R16G16_SINT)) {
            return false;
        }
    }
    context->UpdateSubresource(ripple_.texture.Get(), 0, nullptr, field_.shades(), field_.width() * 4, 0);
    context->UpdateSubresource(foam_.texture.Get(), 0, nullptr, foam_pixels_, SCENE_WIDTH, 0);
    return true;
}

void WakeLayer::bind(ID3D11DeviceContext *context, float *constants)
{
    constants[0] = (float)field_.x();
    constants[1] = (float)field_.y();
    constants[2] = (float)settings_.resolution;
    constants[3] = (float)fraction_;
    ID3D11ShaderResourceView *views[] = {ripple_.view.Get(), foam_.view.Get()};
    context->PSSetShaderResources(4, 2, views);
}

void WakeLayer::log()
{
    diag_log("wake: ships=%u particles=%u steps=%u step_ms=%.3f frame_ms=%.3f upload_bytes_frame=%u own=game",
             count_, alive_, steps_, steps_ ? step_ms_ / steps_ : 0, frames_ ? frame_ms_ / frames_ : 0,
             field_.width() * field_.height() * 4 + SCENE_PIXELS);
    steps_ = 0;
    frames_ = 0;
    step_ms_ = 0;
    frame_ms_ = 0;
}
