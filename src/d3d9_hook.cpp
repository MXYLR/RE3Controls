// D3D9 Present hook: per-frame tick on the render thread and dev screenshots of the back buffer.
#include "common.h"
#include <d3d9.h>
#include <stdio.h>
#include "MinHook.h"

typedef HRESULT(WINAPI* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef IDirect3D9*(WINAPI* PFN_Create9)(UINT);

static PFN_Present oPresent = nullptr;

static void WriteBmp(const char* path, const BYTE* bits, int pitch, int w, int h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)(w * h * 4);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = 0; y < h; ++y) fwrite(bits + (size_t)y * pitch, 4, (size_t)w, f);
    fclose(f);
}

static void CaptureBackBuffer(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* src = bb;
    IDirect3DSurface9* resolved = nullptr;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
        if (SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved,
                                              nullptr)) &&
            SUCCEEDED(dev->StretchRect(bb, nullptr, resolved, nullptr, D3DTEXF_NONE)))
            src = resolved;
    }
    IDirect3DSurface9* sys = nullptr;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) {
        if (SUCCEEDED(dev->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                char path[MAX_PATH];
                wsprintfA(path, "%sRE3C_dev_shot.bmp", ModDir());
                WriteBmp(path, (const BYTE*)lr.pBits, lr.Pitch, (int)d.Width, (int)d.Height);
                sys->UnlockRect();
            }
        } else {
            Log("shot: GetRenderTargetData failed (format %d)", (int)d.Format);
        }
        sys->Release();
    }
    if (resolved) resolved->Release();
    bb->Release();
}

void OnRenderFrame(IDirect3DDevice9* dev);

static HRESULT WINAPI HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    BridgeShared* br = Bridge();
    if (br->shot_req != br->shot_done) {
        CaptureBackBuffer(dev);
        br->shot_done = br->shot_req;
    }
    OnRenderFrame(dev);
    return oPresent(dev, a, b, w, r);
}

void InitD3D9Hooks() {
    char sys[MAX_PATH], path[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    wsprintfA(path, "%s\\d3d9.dll", sys);
    HMODULE d3d9 = LoadLibraryA(path);
    if (!d3d9) {
        Log("d3d9: load failed");
        return;
    }
    auto create = (PFN_Create9)GetProcAddress(d3d9, "Direct3DCreate9");
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) {
        Log("d3d9: Direct3DCreate9 failed");
        return;
    }
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RE3C_dummy";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("RE3C_dummy", "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance,
                              nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev);
    if (FAILED(hr) || !dev) {
        Log("d3d9: dummy device failed 0x%08lx", hr);
        d3d->Release();
        DestroyWindow(hwnd);
        return;
    }
    void** vt = *(void***)dev;
    void* present = vt[17];
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    if (MH_CreateHook(present, (void*)HookPresent, (void**)&oPresent) == MH_OK && MH_EnableHook(present) == MH_OK)
        Log("d3d9: Present hooked at %p", present);
    else
        Log("d3d9: Present hook failed");
}
