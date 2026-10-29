#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include "../src/dll/ship_dump.h"
#include "../src/dll/ship_gpu.h"

using Microsoft::WRL::ComPtr;

namespace
{

constexpr unsigned max_side = HD_SIZE * 4;
constexpr unsigned max_vertices = 196608;
constexpr unsigned rs_texture = 1;
constexpr unsigned rs_shade = 9;
constexpr unsigned rs_min = 18;
constexpr unsigned rs_mag = 17;
constexpr unsigned rs_blend = 21;
constexpr unsigned rs_cull = 22;
constexpr unsigned rs_address = 3;
constexpr unsigned rs_address_u = 44;
constexpr unsigned rs_address_v = 45;

template <class T> using Memory = std::unique_ptr<T, decltype(&std::free)>;

template <class T> Memory<T> allocate(size_t count)
{
    return Memory<T>(static_cast<T *>(std::calloc(count, sizeof(T))), &std::free);
}

bool file_read(const char *path, void *dest, size_t bytes)
{
    FILE *file = nullptr;
    if (fopen_s(&file, path, "rb") || !file) {
        return false;
    }
    bool ok = std::fread(dest, 1, bytes, file) == bytes && std::fgetc(file) == EOF;
    std::fclose(file);
    return ok;
}

bool file_write(const char *path, const void *data, size_t bytes)
{
    FILE *file = nullptr;
    if (fopen_s(&file, path, "wb") || !file) {
        return false;
    }
    bool ok = std::fwrite(data, 1, bytes, file) == bytes;
    return std::fclose(file) == 0 && ok;
}

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

bool bmp_write(const char *path, const uint32_t *pixels, unsigned width, unsigned height)
{
    BITMAPFILEHEADER file_header = {};
    BITMAPINFOHEADER info = {};
    file_header.bfType = 0x4d42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info);
    file_header.bfSize = file_header.bfOffBits + width * height * 4;
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    auto bgra = allocate<uint32_t>(width * height);
    if (!bgra) {
        return false;
    }
    for (unsigned i = 0; i < width * height; ++i) {
        unsigned p = pixels[i];
        bgra.get()[i] = (p & 0xff00ff00) | ((p & 255) << 16) | ((p >> 16) & 255);
    }
    FILE *file = nullptr;
    if (fopen_s(&file, path, "wb") || !file) {
        return false;
    }
    bool ok = std::fwrite(&file_header, sizeof(file_header), 1, file) == 1 &&
              std::fwrite(&info, sizeof(info), 1, file) == 1 &&
              std::fwrite(bgra.get(), 4, width * height, file) == width * height;
    return std::fclose(file) == 0 && ok;
}

class Dump
{
  public:
    hd_header header = {};
    Memory<hd_pass> passes = allocate<hd_pass>(HD_PASSES);
    Memory<hd_draw> draws = allocate<hd_draw>(HD_DRAWS);
    Memory<hd_texture> textures = allocate<hd_texture>(HD_TEXTURES);
    Memory<BYTE> geometry = allocate<BYTE>(HD_GEOMETRY);

    bool load(const char *dir)
    {
        if (!passes || !draws || !textures || !geometry) {
            return false;
        }
        char path[MAX_PATH];
        if (sprintf_s(path, "%s\\manifest.json", dir) < 0 ||
            GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES ||
            sprintf_s(path, "%s\\geometry.bin", dir) < 0) {
            return false;
        }
        FILE *file = nullptr;
        if (fopen_s(&file, path, "rb") || !file) {
            return false;
        }
        bool ok = std::fread(&header, sizeof(header), 1, file) == 1 && header.magic == HD_MAGIC &&
                  header.version == HD_VERSION && header.passes > 0 && header.passes <= HD_PASSES &&
                  header.draws <= HD_DRAWS && header.textures <= HD_TEXTURES &&
                  header.geometry_bytes <= HD_GEOMETRY && !header.incomplete;
        ok = ok && std::fread(passes.get(), sizeof(hd_pass), header.passes, file) == header.passes &&
             std::fread(draws.get(), sizeof(hd_draw), header.draws, file) == header.draws &&
             std::fread(textures.get(), sizeof(hd_texture), header.textures, file) == header.textures &&
             std::fread(geometry.get(), 1, header.geometry_bytes, file) == header.geometry_bytes &&
             std::fgetc(file) == EOF;
        std::fclose(file);
        if (!ok) {
            return false;
        }
        for (unsigned i = 0; i < header.passes; ++i) {
            const hd_pass &p = passes.get()[i];
            if (!p.reference_valid || p.first_draw > header.draws ||
                p.draw_count > header.draws - p.first_draw) {
                return false;
            }
        }
        for (unsigned i = 0; i < header.draws; ++i) {
            const hd_draw &d = draws.get()[i];
            if (d.pass >= header.passes || d.primitive != 4 || d.vertex_count > 65536 ||
                d.index_count > max_vertices || !range(d.vertices, d.vertex_count, sizeof(hd_vertex)) ||
                !range(d.indices, d.index_count, 2) ||
                (d.texture != UINT32_MAX && d.texture >= header.textures)) {
                return false;
            }
            const hd_pass &p = passes.get()[d.pass];
            if (i < p.first_draw || i - p.first_draw >= p.draw_count) {
                return false;
            }
        }
        return true;
    }

  private:
    bool range(unsigned offset, unsigned count, unsigned stride) const
    {
        return offset <= header.geometry_bytes && count <= (header.geometry_bytes - offset) / stride;
    }
};

/* This wrapper only loads files and reads pixels; all raster work is DLL code. */
class Replay
{
  public:
    bool initialize(unsigned side)
    {
        side_ = side;
        D3D_FEATURE_LEVEL level;
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, device_.GetAddressOf(), &level,
                                           context_.GetAddressOf())) &&
               gpu_.initialize(device_.Get(), context_.Get());
    }

    bool render(const Dump &dump, const char *dir, unsigned pass, uint32_t *pixels, bool verify)
    {
        auto raw = allocate<BYTE>(HD_PIXELS);
        unsigned offsets[HD_TEXTURES] = {};
        unsigned bytes = 0;
        if (!raw) {
            return false;
        }
        for (unsigned i = 0; i < dump.header.textures; ++i) {
            const hd_texture &t = dump.textures.get()[i];
            char path[MAX_PATH];
            offsets[i] = bytes;
            if (t.bytes > HD_PIXELS - bytes || sprintf_s(path, "%s\\textures\\%u.bin", dir, i) < 0 ||
                !file_read(path, raw.get() + bytes, t.bytes)) {
                return false;
            }
            bytes += t.bytes;
        }
        HdGpuView view = {dump.passes.get(),
                          dump.header.passes,
                          dump.draws.get(),
                          dump.header.draws,
                          dump.textures.get(),
                          dump.header.textures,
                          dump.geometry.get(),
                          dump.header.geometry_bytes,
                          raw.get(),
                          bytes,
                          offsets};
        Texture2D target;
        gpu_.begin_record();
        if (!gpu_.render(view, pass, side_, target)) {
            return false;
        }
        if (verify) {
            unsigned uploads = gpu_.uploads();
            Texture2D repeated;
            gpu_.begin_record();
            if (!gpu_.render(view, pass, side_, repeated) || gpu_.uploads() != uploads) {
                std::fprintf(stderr, "Texture cache failed to reuse identical content\n");
                return false;
            }
            const hd_pass &p = view.passes[pass];
            for (unsigned i = p.first_draw; i < p.first_draw + p.draw_count; ++i) {
                unsigned texture = view.draws[i].texture;
                if (view.draws[i].result < 0 || texture == UINT32_MAX) {
                    continue;
                }
                raw.get()[offsets[texture]] ^= 1;
                gpu_.begin_record();
                bool changed = gpu_.render(view, pass, side_, repeated) && gpu_.uploads() == uploads + 1;
                raw.get()[offsets[texture]] ^= 1;
                gpu_.begin_record();
                if (!changed || !gpu_.render(view, pass, side_, repeated) || gpu_.uploads() != uploads + 2) {
                    std::fprintf(stderr, "Texture cache failed content invalidation\n");
                    return false;
                }
                break;
            }
        }
        D3D11_TEXTURE2D_DESC desc;
        target.texture->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
            return false;
        }
        context_->CopyResource(staging.Get(), target.texture.Get());
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            return false;
        }
        for (unsigned y = 0; y < side_; ++y) {
            std::memcpy(pixels + y * side_, static_cast<BYTE *>(map.pData) + y * map.RowPitch, side_ * 4);
        }
        context_->Unmap(staging.Get(), 0);
        return true;
    }

  private:
    unsigned side_ = 0;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ShipGpu gpu_;
};

bool synthetic(const char *dir)
{
    if (!CreateDirectoryA(dir, nullptr)) {
        std::fprintf(stderr, "Synthetic directory must be new\n");
        return false;
    }
    char path[MAX_PATH];
    sprintf_s(path, "%s\\reference", dir);
    if (!CreateDirectoryA(path, nullptr)) {
        return false;
    }
    sprintf_s(path, "%s\\textures", dir);
    if (!CreateDirectoryA(path, nullptr)) {
        return false;
    }
    hd_vertex vertices[6] = {};
    const float points[6][2] = {{20, 20}, {124, 20}, {72, 124}, {48, 40}, {112, 100}, {32, 100}};
    for (unsigned i = 0; i < 6; ++i) {
        vertices[i].x = (points[i][0] - 72) / 144;
        vertices[i].y = (72 - points[i][1]) / (144 * 0.4226182617f);
        vertices[i].color = i < 3 ? 0xffff0000u : 0xffffffffu;
        vertices[i].u = 0.25f;
        vertices[i].v = 0.25f;
    }
    hd_header header = {};
    header.magic = HD_MAGIC;
    header.version = HD_VERSION;
    header.step = 1;
    header.passes = 1;
    header.draws = 2;
    header.textures = 1;
    const WORD indices[3] = {0, 1, 2};
    header.geometry_bytes = sizeof(vertices) + sizeof(indices);
    hd_pass pass = {};
    pass.id = 7;
    pass.pass = 1;
    pass.key = 0x7e0;
    pass.draw_count = 2;
    pass.reference_valid = 1;
    hd_draw draw = {};
    draw.primitive = 4;
    draw.vertex_count = 3;
    draw.texture = UINT32_MAX;
    draw.rs[rs_shade] = 1;
    draw.rs[rs_cull] = 1;
    draw.rs[rs_address] = 1;
    draw.rs[rs_address_u] = 1;
    draw.rs[rs_address_v] = 1;
    draw.rs[rs_min] = 1;
    draw.rs[rs_mag] = 1;
    for (unsigned m = 0; m < 3; ++m) {
        for (unsigned i = 0; i < 4; ++i) {
            draw.transforms[m][i * 4 + i] = 1;
        }
    }
    draw.transforms[1][5] = 0.4226182617f;
    draw.transforms[1][12] = -0.5f;
    draw.transforms[1][13] = 0.5f;
    draw.transforms[2][10] = 0;
    draw.viewport[0] = 44;
    draw.viewport[3] = HD_SIZE;
    draw.viewport[4] = HD_SIZE;
    const float clip[6] = {-1, 1, 1, 1, -1, 1};
    std::memcpy(draw.viewport + 5, clip, sizeof(clip));
    hd_draw indexed = draw;
    indexed.vertices = 3 * sizeof(hd_vertex);
    indexed.index_count = 3;
    indexed.indices = sizeof(vertices);
    indexed.texture = 0;
    indexed.rs[rs_texture] = 42;
    indexed.rs[rs_blend] = 2;
    hd_texture texture = {};
    texture.width = 2;
    texture.height = 2;
    texture.pitch = 2;
    texture.row_bytes = 2;
    texture.bytes = 4;
    texture.bpp = 8;
    texture.flags = 0x20;
    texture.palette_valid = 1;
    texture.palette[6] = 255;
    const BYTE texels[4] = {1, 1, 1, 1};
    auto reference = allocate<WORD>(HD_SIZE * HD_SIZE);
    if (!reference) {
        return false;
    }
    /* Independent integer-centre CPU reference; second triangle paints over first. */
    for (unsigned y = 0; y < HD_SIZE; ++y) {
        for (unsigned x = 0; x < HD_SIZE; ++x) {
            WORD color = 0x7e0;
            for (unsigned t = 0; t < 2; ++t) {
                bool inside = true;
                for (unsigned edge = 0; edge < 3; ++edge) {
                    const float *a = points[t * 3 + edge];
                    const float *b = points[t * 3 + (edge + 1) % 3];
                    inside = inside && ((b[0] - a[0]) * (static_cast<float>(y) - a[1]) -
                                            (b[1] - a[1]) * (static_cast<float>(x) - a[0]) >=
                                        0);
                }
                if (inside) {
                    color = t ? 0x1f : 0xf800;
                }
            }
            reference.get()[y * HD_SIZE + x] = color;
        }
    }
    sprintf_s(path, "%s\\geometry.bin", dir);
    FILE *file = nullptr;
    if (fopen_s(&file, path, "wb") || !file) {
        return false;
    }
    bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
              std::fwrite(&pass, sizeof(pass), 1, file) == 1 &&
              std::fwrite(&draw, sizeof(draw), 1, file) == 1 &&
              std::fwrite(&indexed, sizeof(indexed), 1, file) == 1 &&
              std::fwrite(&texture, sizeof(texture), 1, file) == 1 &&
              std::fwrite(vertices, sizeof(vertices), 1, file) == 1 &&
              std::fwrite(indices, sizeof(indices), 1, file) == 1;
    ok = std::fclose(file) == 0 && ok;
    sprintf_s(path, "%s\\reference\\0.rgb565", dir);
    ok = file_write(path, reference.get(), HD_SIZE * HD_SIZE * 2) && ok;
    sprintf_s(path, "%s\\textures\\0.bin", dir);
    ok = file_write(path, texels, sizeof(texels)) && ok;
    sprintf_s(path, "%s\\draws.json", dir);
    const char draws[] =
        "{\"synthetic\":true,\"triangles\":2,\"reference\":\"independent CPU integer centres\"}\n";
    ok = file_write(path, draws, sizeof(draws) - 1) && ok;
    sprintf_s(path, "%s\\manifest.json", dir);
    const char manifest[] = "{\"version\":1,\"synthetic\":true,\"step\":1}\n";
    return file_write(path, manifest, sizeof(manifest) - 1) && ok;
}

bool run(const char *dir, bool check, bool self_test, bool verify = false)
{
    Dump dump;
    if (!dump.load(dir)) {
        std::fprintf(stderr, "Invalid, incomplete, or unsupported dump\n");
        return false;
    }
    unsigned side = check ? HD_SIZE : max_side;
    Replay replay;
    if (!replay.initialize(side)) {
        std::fprintf(stderr, "D3D11 WARP setup failed\n");
        return false;
    }
    auto pixels = allocate<uint32_t>(side * side);
    auto reference = allocate<WORD>(HD_SIZE * HD_SIZE);
    auto pair = allocate<uint32_t>(side * side * 2);
    /* Contact sheet uses four columns of pairs, reduced to 288x144 each. */
    unsigned rows = (dump.header.passes + 3) / 4;
    unsigned sheet_width = HD_SIZE * 8;
    unsigned sheet_height = HD_SIZE * rows;
    auto sheet = allocate<uint32_t>(sheet_width * sheet_height);
    if (!pixels || !reference || !pair || !sheet) {
        return false;
    }
    char path[MAX_PATH];
    for (unsigned p = 0; p < dump.header.passes; ++p) {
        sprintf_s(path, "%s\\reference\\%u.rgb565", dir, p);
        if (!file_read(path, reference.get(), HD_SIZE * HD_SIZE * 2) ||
            !replay.render(dump, dir, p, pixels.get(), verify)) {
            return false;
        }
        double total = 0;
        unsigned maximum = 0;
        unsigned different = 0;
        for (unsigned y = 0; y < side; ++y) {
            for (unsigned x = 0; x < side; ++x) {
                unsigned left = rgb565(reference.get()[(y * HD_SIZE / side) * HD_SIZE + x * HD_SIZE / side]);
                unsigned right = pixels.get()[y * side + x];
                pair.get()[y * side * 2 + x] = left;
                pair.get()[y * side * 2 + side + x] = right;
                if (check) {
                    bool changed = false;
                    /* Quantize replay through RGB565 before comparing expanded channels. */
                    unsigned packed = ((right & 0xf8) << 8) | ((right >> 5) & 0x7e0) | ((right >> 19) & 31);
                    right = rgb565(packed);
                    for (unsigned c = 0; c < 3; ++c) {
                        unsigned a = (left >> (c * 8)) & 255;
                        unsigned b = (right >> (c * 8)) & 255;
                        unsigned delta = a > b ? a - b : b - a;
                        maximum = delta > maximum ? delta : maximum;
                        total += delta;
                        changed = changed || delta != 0;
                    }
                    different += changed;
                }
            }
        }
        if (check) {
            double mean = total / (HD_SIZE * HD_SIZE * 3);
            std::printf(
                "pass=%u ship=%u game_pass=%u pixels_different=%u max=%u mean=%.6f RGB8 after RGB565\n", p,
                dump.passes.get()[p].id, dump.passes.get()[p].pass, different, maximum, mean);
            if ((self_test && mean > 3) || (verify && mean >= .5)) {
                std::fprintf(stderr, "Mean error exceeds threshold\n");
                return false;
            }
        }
        sprintf_s(path, "%s\\ship_%u_pass_%u_seq_%u_x%u.bmp", dir, dump.passes.get()[p].id,
                  dump.passes.get()[p].pass, p, side / HD_SIZE);
        if (!verify && !bmp_write(path, pair.get(), side * 2, side)) {
            return false;
        }
        for (unsigned y = 0; y < HD_SIZE; ++y) {
            for (unsigned x = 0; x < HD_SIZE * 2; ++x) {
                unsigned dest = (p / 4 * HD_SIZE + y) * sheet_width + p % 4 * HD_SIZE * 2 + x;
                sheet.get()[dest] = pair.get()[(y * side / HD_SIZE) * side * 2 + x * side / HD_SIZE];
            }
        }
    }
    sprintf_s(path, "%s\\contact_x%u.bmp", dir, side / HD_SIZE);
    return verify || bmp_write(path, sheet.get(), sheet_width, sheet_height);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[2], "--verify") == 0) {
        return run(argv[1], true, false, true) ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "--synthetic") == 0) {
        return synthetic(argv[2]) && run(argv[2], true, true) && run(argv[2], false, false) ? 0 : 1;
    }
    if (argc == 2 || (argc == 3 && std::strcmp(argv[2], "--x1") == 0)) {
        return run(argv[1], argc == 3, false) ? 0 : 1;
    }
    std::fprintf(stderr,
                 "Usage: ship_replay DUMP_DIR [--x1 | --verify]\n       ship_replay --synthetic NEW_DIR\n");
    return 2;
}
