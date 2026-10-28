#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cloud_art_gpu.h"
#include "render_raii.h"
#include "diag.h"
#include <memory>

#define MAX_BAG_BYTES 134217728LL

unsigned CloudArt::word(const BYTE *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

unsigned CloudArt::dword(const BYTE *p)
{
    return word(p) | (word(p + 2) << 16);
}

int CloudArt::decode(cloud_art *out, const BYTE *data, unsigned size, int blend)
{
    if (size < 4) {
        return 0;
    }
    unsigned width = word(data);
    unsigned height = word(data + 2);
    unsigned stride = (width * 2 + 3) & ~3u;
    if (!width || width > 2048 || !height || height > 2048 || size != 4 + stride * height) {
        return 0;
    }
    std::unique_ptr<DWORD[], decltype(&free)> pixels(
        static_cast<DWORD *>(malloc((size_t)width * height * sizeof(DWORD))), free);
    if (!pixels) {
        return 0;
    }
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            unsigned v = word(data + 4 + (height - y - 1) * stride + x * 2);
            unsigned r = v >> 11;
            unsigned g = (v >> 5) & 63;
            unsigned b = v & 31;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            unsigned alpha = blend ? max(r, max(g, b)) : (v == 0x07e0 ? 0 : 255);
            pixels[y * width + x] = (alpha << 24) | (blend ? 0xffffff : (r << 16) | (g << 8) | b);
        }
    }
    free(out->pixels);
    out->width = width;
    out->height = height;
    out->pixels = pixels.release();
    return 1;
}

int CloudArt::image_index(const char *name)
{
    for (unsigned i = 0; i < CLOUD_IMAGES; ++i) {
        char wanted[40];
        sprintf_s(wanted, sizeof(wanted), "%s\\CLOUD_%02u.BMP", i >= 18 ? "BLEND" : "SEA", i % 18);
        if (_stricmp(name, wanted) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Validate the entire directory before publishing any image. */
int CloudArt::read_directory(const BYTE *data, unsigned size, cloud_art *images, int publish)
{
    if (size < 6) {
        return 0;
    }
    unsigned start = dword(data);
    unsigned count = word(data + 4);
    unsigned cursor = 6;
    unsigned offset = start;
    if (start < 6 || start > size) {
        return 0;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (cursor >= start) {
            return 0;
        }
        unsigned length = data[cursor++];
        if (!length || length + 4 > start - cursor) {
            return 0;
        }
        char name[256];
        memcpy(name, data + cursor, length);
        name[length] = 0;
        cursor += length;
        unsigned bytes = dword(data + cursor);
        cursor += 4;
        if (bytes > size - offset) {
            return 0;
        }
        int index = image_index(name);
        if (publish && index >= 0) {
            decode(&images[index], data + offset, bytes, index >= 18);
        }
        offset += bytes;
    }
    return cursor == start;
}

void CloudArt::read(cloud_art images[CLOUD_IMAGES])
{
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        return;
    }
    char *slash = strrchr(path, '\\');
    if (!slash || (size_t)(slash - path) + sizeof("\\Baggage\\Bag2.tow") > sizeof(path)) {
        return;
    }
    strcpy_s(slash, sizeof(path) - (size_t)(slash - path), "\\Baggage\\Bag2.tow");
    unique_handle file(
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 6 || size.QuadPart > MAX_BAG_BYTES) {
        return;
    }
    std::unique_ptr<BYTE[], decltype(&free)> data(static_cast<BYTE *>(malloc((size_t)size.QuadPart)), free);
    DWORD bytes = 0;
    if (data && ReadFile(file.get(), data.get(), (DWORD)size.QuadPart, &bytes, NULL) &&
        bytes == size.QuadPart && read_directory(data.get(), bytes, images, 0)) {
        read_directory(data.get(), bytes, images, 1);
    }
}

void CloudArt::load(ID3D11Device *device, unsigned &uploads, unsigned long long &upload_bytes)
{
    cloud_art images[CLOUD_IMAGES] = {};
    cloud_art_load(images);
    for (unsigned i = 0; i < CLOUD_IMAGES; ++i) {
        cloud_art *image = &images[i];
        if (!image->pixels) {
            continue;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = image->width;
        desc.Height = image->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = image->pixels;
        data.SysMemPitch = image->width * 4;
        Texture2D *out = &images_[i];
        HRESULT hr = device->CreateTexture2D(&desc, &data, &out->texture);
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(out->texture.Get(), NULL, &out->view);
        }
        if (SUCCEEDED(hr)) {
            SetRect(&out->box, 0, 0, (int)image->width, (int)image->height);
            ++loaded;
            ++uploads;
            upload_bytes += (unsigned long long)image->width * image->height * 4;
        } else {
            out->view.Reset();
            out->texture.Reset();
        }
        free(image->pixels);
    }
    diag_log("scene: cloud art loaded=%u/36 alpha=brightness-white", loaded);
}

/* Whole-record fallback avoids drawing an art cloud over a baked cloud. */
int CloudArt::ready(const scene_record *record) const
{
    if (!(record->cloud_flags & 8)) {
        return 0;
    }
    unsigned layers = (1u << SCENE_D) | (1u << SCENE_E);
    if ((record->layers & layers) != layers) {
        return 0;
    }
    for (unsigned i = 0; i < SCENE_CLOUDS; ++i) {
        const scene_cloud *cloud = &record->clouds[i];
        if (!cloud->valid || cloud->image >= 18) {
            return 0;
        }
        const Texture2D *art = &images_[cloud->image + ((record->cloud_flags & 2) ? 18 : 0)];
        if (!art->view || art->box.right != cloud->width || art->box.bottom != cloud->height) {
            return 0;
        }
    }
    return 1;
}

void CloudArt::release()
{
    for (unsigned i = 0; i < CLOUD_IMAGES; ++i) {
        images_[i].view.Reset();
        images_[i].texture.Reset();
    }
    loaded = 0;
}

void cloud_art_load(cloud_art images[CLOUD_IMAGES])
{
    CloudArt::read(images);
}
