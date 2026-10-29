#ifndef TOW_SHIP_DUMP_H
#define TOW_SHIP_DUMP_H

#include <stdint.h>

#define HD_MAGIC    0x35385754u
#define HD_VERSION  1u
#define HD_PASSES   128u
#define HD_DRAWS    8192u
#define HD_TEXTURES 512u
#define HD_GEOMETRY (32u * 1024u * 1024u)
#define HD_PIXELS   (16u * 1024u * 1024u)
#define HD_SIZE     144u
#define HD_RS       61u
#define HD_LS       9u

/* Disk ABI: little endian, IEEE floats, four-byte packing, no native pointers. */
#pragma pack(push, 4)

typedef struct {
    float x;
    float y;
    float z;
    float reserved;
    uint32_t color;
    uint32_t specular;
    float u;
    float v;
} hd_vertex;

typedef struct {
    uint32_t id;
    uint32_t object;
    uint32_t image;
    uint32_t type;
    uint32_t owner;
    uint32_t pass;
    double pose[4];
    uint32_t handler;
    uint32_t queue;
    uint32_t blend;
    uint32_t reverse;
    uint32_t main_first;
    uint32_t key;
    int32_t source[4];
    int32_t dest[4];
    uint32_t first_draw;
    uint32_t draw_count;
    uint32_t reference_valid;
} hd_pass;

typedef struct {
    uint32_t pass;
    uint32_t polyset;
    uint32_t material;
    uint32_t damage;
    uint32_t part_class;
    uint32_t primitive;
    uint32_t flags;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t vertices;
    uint32_t indices;
    uint32_t preclip_count;
    uint32_t preclip;
    uint32_t texture;
    int32_t result;
    uint32_t rs[HD_RS];
    uint32_t ls[HD_LS];
    int32_t rs_hr[HD_RS];
    int32_t ls_hr[HD_LS];
    int32_t transform_hr[3];
    int32_t viewport_hr;
    float transforms[3][16];
    uint32_t viewport[11];
} hd_draw;

typedef struct {
    uint32_t device;
    uint32_t handle;
    uint32_t surface;
    uint32_t width;
    uint32_t height;
    int32_t pitch;
    uint32_t row_bytes;
    uint32_t bytes;
    uint32_t flags;
    uint32_t bpp;
    uint32_t rmask;
    uint32_t gmask;
    uint32_t bmask;
    uint32_t amask;
    uint32_t palette_valid;
    uint32_t key_valid;
    uint32_t key_low;
    uint32_t key_high;
    uint8_t palette[1024];
} hd_texture;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t step;
    uint32_t passes;
    uint32_t draws;
    uint32_t textures;
    uint32_t geometry_bytes;
    uint32_t incomplete;
    char exe_sha256[65];
    char text_sha256[65];
} hd_header;

#pragma pack(pop)

typedef char hd_vertex_size_check[sizeof(hd_vertex) == 32 ? 1 : -1];
typedef char hd_header_size_check[sizeof(hd_header) == 164 ? 1 : -1];
typedef char hd_pass_size_check[sizeof(hd_pass) == 124 ? 1 : -1];
typedef char hd_draw_size_check[sizeof(hd_draw) == 872 ? 1 : -1];
typedef char hd_texture_size_check[sizeof(hd_texture) == 1096 ? 1 : -1];

#endif
