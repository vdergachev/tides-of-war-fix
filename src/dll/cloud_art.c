#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cloud_art.h"

#define MAX_BAG_BYTES 134217728LL

static unsigned word(const BYTE *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned dword(const BYTE *p)
{
    return word(p) | (word(p + 2) << 16);
}

static int decode(cloud_art *out, const BYTE *data, unsigned size, int blend)
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
    DWORD *pixels = malloc((size_t)width * height * sizeof(DWORD));
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
    out->pixels = pixels;
    return 1;
}

static int image_index(const char *name)
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
static int read_directory(const BYTE *data, unsigned size, cloud_art *images, int publish)
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

void cloud_art_load(cloud_art images[CLOUD_IMAGES])
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
    HANDLE file =
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 6 || size.QuadPart > MAX_BAG_BYTES) {
        CloseHandle(file);
        return;
    }
    BYTE *data = malloc((size_t)size.QuadPart);
    DWORD bytes = 0;
    if (data && ReadFile(file, data, (DWORD)size.QuadPart, &bytes, NULL) && bytes == size.QuadPart &&
        read_directory(data, bytes, images, 0)) {
        read_directory(data, bytes, images, 1);
    }
    free(data);
    CloseHandle(file);
}
