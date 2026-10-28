#ifndef TOW_TEXTURE2D_H
#define TOW_TEXTURE2D_H
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

struct Texture2D {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    RECT box;
};
#endif
