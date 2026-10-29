#include "ship_gpu.h"
#include "ship_gpu_vs.h"
#include "ship_gpu_ps.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <new>
using Microsoft::WRL::ComPtr;

namespace
{
constexpr unsigned max_vertices = 196608;
constexpr unsigned rs_texture = 1;
constexpr unsigned rs_shade = 9;
constexpr unsigned rs_min = 18;
constexpr unsigned rs_mag = 17;
constexpr unsigned rs_src = 19;
constexpr unsigned rs_dst = 20;
constexpr unsigned rs_blend = 21;
constexpr unsigned rs_cull = 22;
constexpr unsigned rs_alpha_ref = 24;
constexpr unsigned rs_alpha_func = 25;
constexpr unsigned rs_alpha_enable = 27;
constexpr unsigned rs_alpha_test = 15;
constexpr unsigned rs_address = 3;
constexpr unsigned rs_address_u = 44;
constexpr unsigned rs_address_v = 45;
constexpr unsigned rs_colorkey = 41;
constexpr unsigned rs_specular = 29;
constexpr unsigned rs_fog = 28;

struct Vertex {
    float x;
    float y;
    float u;
    float v;
    float color[4];
    float specular[4];
};

struct Constants {
    unsigned textured;
    unsigned alpha_test;
    unsigned alpha_func;
    float alpha_ref;
    unsigned specular;
    unsigned colorkey;
    unsigned modulate_alpha;
    unsigned padding;
};

unsigned expand(unsigned value, unsigned mask)
{
    if (!mask) {
        return 0;
    }
    while (!(mask & 1)) {
        mask >>= 1;
        value >>= 1;
    }
    return ((value & mask) * 255u + mask / 2) / mask;
}

uint32_t rgb565(unsigned value)
{
    return expand(value, 0xf800) | (expand(value, 0x7e0) << 8) | (expand(value, 31) << 16) | 0xff000000u;
}

void transform(float value[4], const float matrix[16])
{
    float result[4] = {};
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned r = 0; r < 4; ++r) {
            result[c] += value[r] * matrix[r * 4 + c];
        }
    }
    std::memcpy(value, result, sizeof(result));
}

bool screen_point(const hd_vertex &v, const hd_draw &d, float &x, float &y)
{
    if (d.transform_hr[0] < 0 || d.transform_hr[1] < 0 || d.transform_hr[2] < 0 || d.viewport_hr < 0 ||
        d.viewport[0] != sizeof(d.viewport) || !d.viewport[3] || !d.viewport[4]) {
        return false;
    }
    float value[4] = {v.x, v.y, v.z, 1};
    for (unsigned i = 0; i < 3; ++i) {
        for (float component : d.transforms[i]) {
            if (!std::isfinite(component)) {
                return false;
            }
        }
        transform(value, d.transforms[i]);
    }
    float viewport[11];
    std::memcpy(viewport, d.viewport, sizeof(viewport));
    if (!std::isfinite(value[3]) || value[3] == 0 || !std::isfinite(viewport[7]) ||
        !std::isfinite(viewport[8]) || viewport[7] == 0 || viewport[8] == 0) {
        return false;
    }
    x = static_cast<float>(d.viewport[1]) +
        (value[0] / value[3] - viewport[5]) * static_cast<float>(d.viewport[3]) / viewport[7];
    y = static_cast<float>(d.viewport[2]) +
        (viewport[6] - value[1] / value[3]) * static_cast<float>(d.viewport[4]) / viewport[8];
    return std::isfinite(x) && std::isfinite(y);
}

} // namespace

class ShipGpu::Impl
{
  public:
    bool initialize(ID3D11Device *device, ID3D11DeviceContext *context)
    {
        device_ = device;
        context_ = context;
        if (FAILED(
                device_->CreateVertexShader(tow_ship_vs, sizeof(tow_ship_vs), nullptr, vs_.GetAddressOf())) ||
            FAILED(
                device_->CreatePixelShader(tow_ship_ps, sizeof(tow_ship_ps), nullptr, ps_.GetAddressOf()))) {
            return false;
        }
        D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        if (FAILED(device_->CreateInputLayout(elements, 4, tow_ship_vs, sizeof(tow_ship_vs),
                                              layout_.GetAddressOf()))) {
            return false;
        }
        D3D11_BUFFER_DESC buffer = {};
        buffer.ByteWidth = max_vertices * sizeof(Vertex);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, vertices_.GetAddressOf()))) {
            return false;
        }
        buffer.ByteWidth = sizeof(Constants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, constants_.GetAddressOf()))) {
            return false;
        }
        D3D11_DEPTH_STENCIL_DESC depth = {};
        return SUCCEEDED(device_->CreateDepthStencilState(&depth, depth_.GetAddressOf()));
    }

    bool render(const HdGpuView &dump, unsigned pass, unsigned side, Texture2D &out)
    {
        side_ = side;
        if (!side || side > HD_SIZE * 16 || pass >= dump.pass_count) {
            return false;
        }
        const hd_pass &p = dump.passes[pass];
        if (!p.reference_valid || p.first_draw > dump.draw_count ||
            p.draw_count > dump.draw_count - p.first_draw) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = side;
        desc.Height = side;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        out = {};
        rtv_.Reset();
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, out.texture.GetAddressOf())) ||
            FAILED(device_->CreateShaderResourceView(out.texture.Get(), nullptr, out.view.GetAddressOf())) ||
            FAILED(device_->CreateRenderTargetView(out.texture.Get(), nullptr, rtv_.GetAddressOf()))) {
            return false;
        }
        unsigned background = rgb565(p.key);
        float clear[4] = {static_cast<float>(background & 255) / 255,
                          static_cast<float>((background >> 8) & 255) / 255,
                          static_cast<float>((background >> 16) & 255) / 255, 1};
        context_->ClearRenderTargetView(rtv_.Get(), clear);
        context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(side_), static_cast<float>(side_), 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->IASetInputLayout(layout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        for (unsigned i = p.first_draw; i < p.first_draw + p.draw_count; ++i) {
            const hd_draw &d = dump.draws[i];
            if (d.result < 0) {
                continue;
            }
            if (!draw(dump, d)) {
                std::fprintf(stderr, "Unsupported or invalid draw %u (pass %u)\n", i, pass);
                return false;
            }
        }
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
    }

    void begin_record()
    {
        std::memset(hash_valid_, 0, sizeof(hash_valid_));
    }

    unsigned uploads() const
    {
        return uploads_;
    }

  private:
    unsigned side_ = 0;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> vertices_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    std::unique_ptr<Vertex[]> scratch_;
    unsigned scratch_count_ = 0;
    ComPtr<ID3D11DepthStencilState> depth_;
    ComPtr<ID3D11RasterizerState> raster_[3];
    ComPtr<ID3D11BlendState> blend_[12][12];
    ComPtr<ID3D11SamplerState> sampler_[4][4][4];
    uint64_t hashes_[HD_TEXTURES] = {};
    bool hash_valid_[HD_TEXTURES] = {};

    struct CachedTexture {
        uint64_t hash = 0;
        hd_texture info = {};
        ComPtr<ID3D11ShaderResourceView> view;
    };

    CachedTexture cache_[HD_TEXTURES];
    unsigned next_texture_ = 0;
    unsigned uploads_ = 0;

    bool texture(const HdGpuView &dump, const hd_draw &d, ComPtr<ID3D11ShaderResourceView> &srv)
    {
        if (d.texture == UINT32_MAX) {
            return d.rs[rs_texture] == 0;
        }
        if (d.texture >= dump.texture_count || d.texture >= HD_TEXTURES) {
            return false;
        }
        const hd_texture &t = dump.textures[d.texture];
        unsigned bytes = t.bpp / 8;
        if (!t.width || t.width > 2048 || !t.height || t.height > 2048 ||
            (t.bpp != 8 && t.bpp != 16 && t.bpp != 24 && t.bpp != 32) || t.row_bytes != t.width * bytes ||
            t.bytes != t.row_bytes * t.height || (t.bpp == 8 && !t.palette_valid)) {
            return false;
        }
        unsigned offset = dump.texture_offsets[d.texture];
        if (offset > dump.pixel_bytes || t.bytes > dump.pixel_bytes - offset) {
            return false;
        }
        const BYTE *raw = dump.pixels + offset;
        uint64_t hash = texture_hash(t, raw, d.texture) ^ (d.rs[rs_colorkey] != 0);
        for (auto &entry : cache_) {
            if (entry.view && entry.hash == hash && !std::memcmp(&entry.info, &t, sizeof(t))) {
                srv = entry.view;
                return true;
            }
        }
        if (!upload_texture(t, raw, d.rs[rs_colorkey] != 0, srv)) {
            return false;
        }
        /* Replace changed contents of the same game texture immediately. */
        unsigned slot = next_texture_++ % HD_TEXTURES;
        for (unsigned i = 0; i < HD_TEXTURES; ++i) {
            const hd_texture &old = cache_[i].info;
            if (cache_[i].view && old.device == t.device && old.handle == t.handle) {
                slot = i;
                break;
            }
        }
        auto &entry = cache_[slot];
        entry.hash = hash;
        entry.info = t;
        entry.view = srv;
        ++uploads_;
        return true;
    }

    uint64_t texture_hash(const hd_texture &t, const BYTE *raw, unsigned index)
    {
        if (!hash_valid_[index]) {
            uint64_t hash = 14695981039346656037ull;
            const BYTE *info = reinterpret_cast<const BYTE *>(&t);
            for (unsigned i = 0; i < sizeof(t); ++i) {
                hash = (hash ^ info[i]) * 1099511628211ull;
            }
            for (unsigned i = 0; i < t.bytes; ++i) {
                hash = (hash ^ raw[i]) * 1099511628211ull;
            }
            hashes_[index] = hash;
            hash_valid_[index] = true;
        }
        return hashes_[index];
    }

    uint32_t texture_color(const hd_texture &t, unsigned value, bool colorkey)
    {
        unsigned color;
        if (t.bpp == 8) {
            const BYTE *p = t.palette + value * 4;
            color = p[0] | (p[1] << 8) | (p[2] << 16) | 0xff000000u;
        } else {
            color = expand(value, t.rmask) | (expand(value, t.gmask) << 8) | (expand(value, t.bmask) << 16) |
                    ((t.amask ? expand(value, t.amask) : 255) << 24);
        }
        if (colorkey && t.key_valid && value >= t.key_low && value <= t.key_high) {
            color &= 0xffffff;
        }
        return color;
    }

    bool upload_texture(const hd_texture &t, const BYTE *raw, bool colorkey,
                        ComPtr<ID3D11ShaderResourceView> &srv)
    {
        unsigned bytes = t.bpp / 8;
        std::unique_ptr<uint32_t[]> rgba{new (std::nothrow) uint32_t[t.width * t.height]};
        if (!rgba) {
            return false;
        }
        for (unsigned i = 0; i < t.width * t.height; ++i) {
            unsigned v = 0;
            std::memcpy(&v, raw + i * bytes, bytes);
            rgba.get()[i] = texture_color(t, v, colorkey);
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = t.width;
        desc.Height = t.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial = {rgba.get(), t.width * 4, 0};
        ComPtr<ID3D11Texture2D> image;
        if (FAILED(device_->CreateTexture2D(&desc, &initial, image.GetAddressOf())) ||
            FAILED(device_->CreateShaderResourceView(image.Get(), nullptr, srv.GetAddressOf()))) {
            return false;
        }
        return true;
    }

    bool valid_states(const hd_draw &d)
    {
        static const unsigned required[] = {rs_texture,      rs_shade,      rs_fog,      rs_cull,
                                            rs_alpha_enable, rs_alpha_test, rs_specular, rs_colorkey};
        for (unsigned state : required) {
            if (d.rs_hr[state] < 0) {
                return false;
            }
        }
        if (d.rs[rs_shade] != 1 || d.rs[rs_fog] || d.rs[rs_cull] < 1 || d.rs[rs_cull] > 3 ||
            d.rs[rs_alpha_enable] > 1 || d.rs[rs_alpha_test] > 1 || d.rs[rs_specular] > 1 ||
            d.rs[rs_colorkey] > 1) {
            return false;
        }
        if (d.rs[rs_texture] && (d.rs_hr[rs_blend] < 0 || (d.rs[rs_blend] != 2 && d.rs[rs_blend] != 4))) {
            return false;
        }
        if (d.rs[rs_alpha_test] &&
            (d.rs_hr[rs_alpha_ref] < 0 || d.rs[rs_alpha_ref] > 255 || d.rs_hr[rs_alpha_func] < 0 ||
             d.rs[rs_alpha_func] < 1 || d.rs[rs_alpha_func] > 8)) {
            return false;
        }
        return true;
    }

    bool raster_state(const hd_draw &d)
    {
        auto &state = raster_[d.rs[rs_cull] - 1];
        if (!state) {
            D3D11_RASTERIZER_DESC desc = {};
            desc.FillMode = D3D11_FILL_SOLID;
            desc.CullMode = d.rs[rs_cull] == 1 ? D3D11_CULL_NONE
                                               : (d.rs[rs_cull] == 2 ? D3D11_CULL_FRONT : D3D11_CULL_BACK);
            if (FAILED(device_->CreateRasterizerState(&desc, state.GetAddressOf()))) {
                return false;
            }
        }
        context_->RSSetState(state.Get());
        context_->OMSetDepthStencilState(depth_.Get(), 0);
        return true;
    }

    bool blend_state(const hd_draw &d)
    {
        bool enabled = d.rs[rs_alpha_enable] != 0;
        unsigned src = enabled ? d.rs[rs_src] : 0;
        unsigned dst = enabled ? d.rs[rs_dst] : 0;
        if (enabled &&
            (d.rs_hr[rs_src] < 0 || d.rs_hr[rs_dst] < 0 || src < 1 || src > 11 || dst < 1 || dst > 11)) {
            return false;
        }
        auto &state = blend_[src][dst];
        if (!state) {
            D3D11_BLEND_DESC desc = {};
            auto &rt = desc.RenderTarget[0];
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            rt.BlendEnable = enabled;
            rt.SrcBlend = enabled ? static_cast<D3D11_BLEND>(src) : D3D11_BLEND_ONE;
            rt.DestBlend = enabled ? static_cast<D3D11_BLEND>(dst) : D3D11_BLEND_ZERO;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            if (FAILED(device_->CreateBlendState(&desc, state.GetAddressOf()))) {
                return false;
            }
        }
        context_->OMSetBlendState(state.Get(), nullptr, UINT32_MAX);
        return true;
    }

    bool sampler_state(const hd_draw &d)
    {
        unsigned ustate = d.rs_hr[rs_address_u] >= 0 ? rs_address_u : rs_address;
        unsigned vstate = d.rs_hr[rs_address_v] >= 0 ? rs_address_v : rs_address;
        unsigned u = d.rs[ustate];
        unsigned v = d.rs[vstate];
        if (d.rs_hr[ustate] < 0 || d.rs_hr[vstate] < 0 || d.rs_hr[rs_min] < 0 || d.rs_hr[rs_mag] < 0 ||
            u < 1 || u > 4 || v < 1 || v > 4 || d.rs[rs_min] < 1 || d.rs[rs_min] > 2 || d.rs[rs_mag] < 1 ||
            d.rs[rs_mag] > 2) {
            return false;
        }
        unsigned filter = (d.rs[rs_min] == 2 ? 2 : 0) | (d.rs[rs_mag] == 2 ? 1 : 0);
        auto &state = sampler_[u - 1][v - 1][filter];
        if (!state) {
            D3D11_SAMPLER_DESC desc = {};
            desc.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(u);
            desc.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(v);
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.Filter = static_cast<D3D11_FILTER>((filter & 2 ? 0x10 : 0) | (filter & 1 ? 4 : 0));
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device_->CreateSamplerState(&desc, state.GetAddressOf()))) {
                return false;
            }
        }
        context_->PSSetSamplers(0, 1, state.GetAddressOf());
        return true;
    }

    bool states(const hd_draw &d)
    {
        return valid_states(d) && raster_state(d) && blend_state(d) &&
               (!d.rs[rs_texture] || sampler_state(d));
    }

    bool geometry(const HdGpuView &dump, const hd_draw &d, unsigned count)
    {
        if (count > scratch_count_) {
            scratch_.reset(new (std::nothrow) Vertex[count]);
            scratch_count_ = scratch_ ? count : 0;
            if (!scratch_) {
                return false;
            }
        }
        const BYTE *base = dump.geometry;
        if (d.primitive != 4 || d.vertex_count > 65536 || d.vertices > dump.geometry_bytes ||
            d.vertex_count > (dump.geometry_bytes - d.vertices) / sizeof(hd_vertex) ||
            d.indices > dump.geometry_bytes || d.index_count > (dump.geometry_bytes - d.indices) / 2) {
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            unsigned index = i;
            unsigned first = i - i % 3;
            if (d.index_count) {
                WORD value;
                std::memcpy(&value, base + d.indices + i * 2, 2);
                index = value;
                std::memcpy(&value, base + d.indices + first * 2, 2);
                first = value;
            }
            if (index >= d.vertex_count || first >= d.vertex_count) {
                return false;
            }
            hd_vertex source;
            hd_vertex flat;
            std::memcpy(&source, base + d.vertices + index * sizeof(source), sizeof(source));
            std::memcpy(&flat, base + d.vertices + first * sizeof(flat), sizeof(flat));
            Vertex &v = scratch_.get()[i];
            float x;
            float y;
            if (!screen_point(source, d, x, y) || !std::isfinite(source.u) || !std::isfinite(source.v)) {
                return false;
            }
            /* D3D5 integer pixel centres -> D3D11 half-integer pixel centres. */
            v.x = 2 * (x + 0.5f) / HD_SIZE - 1;
            v.y = 1 - 2 * (y + 0.5f) / HD_SIZE;
            v.u = source.u;
            v.v = source.v;
            for (unsigned c = 0; c < 4; ++c) {
                unsigned shift = c == 0 ? 16 : (c == 2 ? 0 : c * 8);
                v.color[c] = static_cast<float>((flat.color >> shift) & 255) / 255;
                v.specular[c] = static_cast<float>((flat.specular >> shift) & 255) / 255;
            }
        }
        return true;
    }

    void draw_constants(const hd_draw &d)
    {
        Constants constants = {};
        constants.textured = d.texture != UINT32_MAX;
        constants.alpha_test = d.rs[rs_alpha_test];
        constants.alpha_func = d.rs[rs_alpha_func];
        constants.alpha_ref = static_cast<float>(d.rs[rs_alpha_ref] & 255) / 255;
        constants.specular = d.rs[rs_specular];
        constants.colorkey = d.rs[rs_colorkey];
        constants.modulate_alpha = d.rs[rs_blend] == 4;
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
    }

    bool draw(const HdGpuView &dump, const hd_draw &d)
    {
        unsigned count = d.index_count ? d.index_count : d.vertex_count;
        if (count > max_vertices || count % 3 || !states(d)) {
            return false;
        }
        if (!geometry(dump, d, count)) {
            return false;
        }
        if (!count) {
            return true;
        }
        ComPtr<ID3D11ShaderResourceView> srv;
        if (!texture(dump, d, srv)) {
            return false;
        }
        context_->PSSetShaderResources(0, 1, srv.GetAddressOf());
        draw_constants(d);
        D3D11_BOX box = {0, 0, 0, count * sizeof(Vertex), 1, 1};
        context_->UpdateSubresource(vertices_.Get(), 0, &box, scratch_.get(), 0, 0);
        unsigned stride = sizeof(Vertex);
        unsigned offset = 0;
        context_->IASetVertexBuffers(0, 1, vertices_.GetAddressOf(), &stride, &offset);
        context_->Draw(count, 0);
        return true;
    }
};

ShipGpu::ShipGpu() noexcept = default;
ShipGpu::~ShipGpu() noexcept = default;

bool ShipGpu::initialize(ID3D11Device *device, ID3D11DeviceContext *context)
{
    impl_.reset(new (std::nothrow) Impl);
    return impl_ && impl_->initialize(device, context);
}

bool ShipGpu::render(const HdGpuView &view, unsigned pass, unsigned side, Texture2D &out)
{
    return impl_ && impl_->render(view, pass, side, out);
}

unsigned ShipGpu::uploads() const
{
    return impl_ ? impl_->uploads() : 0;
}

void ShipGpu::begin_record()
{
    if (impl_) {
        impl_->begin_record();
    }
}
