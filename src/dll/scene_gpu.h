#ifndef TOW_SCENE_GPU_H
#define TOW_SCENE_GPU_H

/* Worker-only API; GPU declarations must precede this header. */
int scene_gpu_init(ID3D11Device *device);
void scene_gpu_release(void);
void scene_gpu_update(ID3D11Device *device, ID3D11DeviceContext *context, LONG epoch);
int scene_gpu_draw(ID3D11DeviceContext *context, UINT width, UINT height, LONG epoch);
int scene_gpu_active(void);
void scene_gpu_counters(unsigned *draws, unsigned *uploads, unsigned long long *bytes);
void scene_gpu_log(void);
void scene_gpu_frame_begin(void);
void scene_gpu_frame_end(void);

#endif
