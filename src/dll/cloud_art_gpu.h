#ifndef TOW_CLOUD_ART_GPU_H
#define TOW_CLOUD_ART_GPU_H
#include "cloud_art.h"
#include "scene.h"
#include "texture2d.h"

/* Render-thread member of SceneCompositor. CPU decoding publishes malloc pixels. */
class CloudArt
{
  public:
    void load(ID3D11Device *device, unsigned &uploads, unsigned long long &upload_bytes);
    int ready(const scene_record *record) const;
    void release();
    static void read(cloud_art images[CLOUD_IMAGES]);

    const Texture2D &image(unsigned index) const
    {
        return images_[index];
    }

    unsigned loaded = 0;

  private:
    Texture2D images_[CLOUD_IMAGES] = {};
    static unsigned word(const BYTE *p);
    static unsigned dword(const BYTE *p);
    static int decode(cloud_art *out, const BYTE *data, unsigned size, int blend);
    static int image_index(const char *name);
    static int read_directory(const BYTE *data, unsigned size, cloud_art *images, int publish);
};
#endif
