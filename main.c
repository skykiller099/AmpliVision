#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <shellapi.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "shaders.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

#define APPNAME       L"AmpliVision"
#define CLS_OVER      L"AmpliOverlay"
#define CLS_PANEL     L"AmpliPanel"
#define HOT_TOGGLE    1
#define HOT_PANEL     2
#define HOT_COMPARE   3
#define HOT_PRESET    20
#define WDA_EXCL      0x00000011
#define MAX_PROFILES  16
#define MAX_MON       8
#define PANEL_W       920
#define PANEL_H       800

typedef struct {
    float exposure, gamma, local, lift, sharpen, contrast, sat, vibrance;
    float temp, rolloff, vig, blacks, autoAmt, dither, temporal;
} Params;

typedef struct { char name[64]; char process[64]; Params p; } Profile;

typedef struct {
    int startHidden, clickThrough, excludeCap, vsync, maxFps, capMode, targetMon;
    int hkToggle, hkPanel, hkCompare, hkPreset[9];
    char targetTitle[160];
} AppCfg;

static HINSTANCE g_inst;
static HWND g_over, g_panel, g_nameDlg, g_nameEdit;
static int g_running = 1, g_captureOn, g_hidden, g_panelOpen, g_compare;
static Params g_p, g_def;
static AppCfg g_cfg;
static Profile g_prof[MAX_PROFILES];
static int g_nprof, g_selProf = -1;
static char g_ini[MAX_PATH];
static FILE* g_log;
static int g_rebind = -1;
static int g_warnHotkey;
static char g_status[160] = "Prêt.";
static int g_pendingProf = -1;

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain* g_swap;
static ID3D11RenderTargetView* g_rtv;
static ID3D11Texture2D *g_dupTex, *g_gdiTex, *g_capTex;
static ID3D11ShaderResourceView *g_capSrv, *g_blurSrv, *g_histSrv;
static ID3D11Texture2D *g_luma[2], *g_blurH, *g_blurV, *g_histTex;
static ID3D11RenderTargetView *g_lumaRtv[2], *g_blurHRtv, *g_blurVRtv;
static ID3D11ShaderResourceView *g_lumaSrv[2];
static int g_lumaIdx;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader *g_psLuma, *g_psBlur, *g_psComp;
static ID3D11SamplerState* g_smp;
static ID3D11RasterizerState* g_rs;
static ID3D11Buffer* g_cb;
static IDXGIOutputDuplication* g_dup;
static IDXGIAdapter1* g_adapter;
static IDXGIOutput1* g_output;
static int g_sw, g_sh, g_ready;
static ID3D11Texture2D* g_stage;
static UINT64 g_frame;

static HDC g_gdc, g_gmdc;
static HBITMAP g_gbmp;
static int g_gw, g_gh;
static void* g_gbuf;

static int g_mx, g_my;
static float g_hist[64];
static HMONITOR g_mons[MAX_MON];
static int g_nmon;

static void load_ini(void);
static void save_ini(void);

/* ------------------------------------------------------------- journal */

static void logf_(const char* fmt, ...) {
    if (!g_log) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ----------------------------------------------------------------- INI */

static void get_ini_path(void) {
    char base[MAX_PATH] = { 0 };
    GetEnvironmentVariableA("APPDATA", base, MAX_PATH);
    if (!base[0]) lstrcpyA(base, ".");
    lstrcatA(base, "\\AmpliVision");
    CreateDirectoryA(base, NULL);
    wsprintfA(g_ini, "%s\\settings.ini", base);

    char logp[MAX_PATH];
    wsprintfA(logp, "%s\\amplivision.log", base);
    g_log = fopen(logp, "w");
}

static float ini_f(const char* s, const char* k, float d) {
    char b[64];
    GetPrivateProfileStringA(s, k, "", b, 64, g_ini);
    return b[0] ? (float)atof(b) : d;
}
static int ini_i(const char* s, const char* k, int d) {
    char b[64];
    GetPrivateProfileStringA(s, k, "", b, 64, g_ini);
    return b[0] ? atoi(b) : d;
}
static void ini_s(const char* s, const char* k, const char* v) {
    WritePrivateProfileStringA(s, k, v, g_ini);
}
static void ini_sf(const char* s, const char* k, float v) {
    char b[64]; sprintf(b, "%.4f", v); ini_s(s, k, b);
}
static void ini_si(const char* s, const char* k, int v) {
    char b[32]; sprintf(b, "%d", v); ini_s(s, k, b);
}

static void params_default(Params* p) {
    p->exposure = 2.40f; p->gamma = 1.75f; p->local = 0.72f; p->lift = 0.14f;
    p->sharpen = 0.70f; p->contrast = 1.16f; p->sat = 1.28f; p->vibrance = 0.25f;
    p->temp = 0.02f; p->rolloff = 0.70f; p->vig = 0.55f; p->blacks = 0.05f;
    p->autoAmt = 0.0f; p->dither = 0.35f; p->temporal = 0.30f;
}

static void profile_load_sec(const char* sec, Params* p) {
    p->exposure = ini_f(sec, "exposure", p->exposure);
    p->gamma    = ini_f(sec, "gamma", p->gamma);
    p->local    = ini_f(sec, "local", p->local);
    p->lift     = ini_f(sec, "lift", p->lift);
    p->sharpen  = ini_f(sec, "sharpen", p->sharpen);
    p->contrast = ini_f(sec, "contrast", p->contrast);
    p->sat      = ini_f(sec, "sat", p->sat);
    p->vibrance = ini_f(sec, "vibrance", p->vibrance);
    p->temp     = ini_f(sec, "temp", p->temp);
    p->rolloff  = ini_f(sec, "rolloff", p->rolloff);
    p->vig      = ini_f(sec, "vig", p->vig);
    p->blacks   = ini_f(sec, "blacks", p->blacks);
    p->autoAmt  = ini_f(sec, "autoAmt", p->autoAmt);
    p->dither   = ini_f(sec, "dither", p->dither);
    p->temporal = ini_f(sec, "temporal", p->temporal);
}

static void profile_save_sec(const char* sec, const Params* p) {
    ini_sf(sec, "exposure", p->exposure);
    ini_sf(sec, "gamma", p->gamma);
    ini_sf(sec, "local", p->local);
    ini_sf(sec, "lift", p->lift);
    ini_sf(sec, "sharpen", p->sharpen);
    ini_sf(sec, "contrast", p->contrast);
    ini_sf(sec, "sat", p->sat);
    ini_sf(sec, "vibrance", p->vibrance);
    ini_sf(sec, "temp", p->temp);
    ini_sf(sec, "rolloff", p->rolloff);
    ini_sf(sec, "vig", p->vig);
    ini_sf(sec, "blacks", p->blacks);
    ini_sf(sec, "autoAmt", p->autoAmt);
    ini_sf(sec, "dither", p->dither);
    ini_sf(sec, "temporal", p->temporal);
}

static void load_ini(void) {
    g_cfg.hkToggle     = ini_i("global", "hkToggle", VK_F8);
    g_cfg.hkPanel      = ini_i("global", "hkPanel", VK_F9);
    g_cfg.hkCompare    = ini_i("global", "hkCompare", VK_F7);
    g_cfg.clickThrough = ini_i("global", "clickThrough", 1);
    g_cfg.excludeCap   = ini_i("global", "excludeCap", 1);
    g_cfg.vsync        = ini_i("global", "vsync", 0);
    g_cfg.maxFps       = ini_i("global", "maxFps", 120);
    g_cfg.startHidden  = ini_i("global", "startHidden", 0);
    g_cfg.capMode      = ini_i("global", "capMode", 0);
    g_cfg.targetMon    = ini_i("global", "targetMon", 0);
    for (int i = 0; i < 9; i++) {
        char k[16]; sprintf(k, "hkP%d", i);
        g_cfg.hkPreset[i] = ini_i("global", k, VK_F1 + i);
    }
    GetPrivateProfileStringA("global", "targetTitle", "", g_cfg.targetTitle, 160, g_ini);

    params_default(&g_def);
    profile_load_sec("default", &g_def);

    g_nprof = ini_i("global", "profiles", 0);
    if (g_nprof > MAX_PROFILES) g_nprof = MAX_PROFILES;
    for (int i = 0; i < g_nprof; i++) {
        char sec[32]; sprintf(sec, "profile:%d", i);
        GetPrivateProfileStringA(sec, "name", "", g_prof[i].name, 64, g_ini);
        GetPrivateProfileStringA(sec, "process", "", g_prof[i].process, 64, g_ini);
        g_prof[i].p = g_def;
        profile_load_sec(sec, &g_prof[i].p);
    }
    logf_("config chargee, %d profils", g_nprof);
}

static void save_ini(void) {
    ini_si("global", "hkToggle", g_cfg.hkToggle);
    ini_si("global", "hkPanel", g_cfg.hkPanel);
    ini_si("global", "hkCompare", g_cfg.hkCompare);
    ini_si("global", "clickThrough", g_cfg.clickThrough);
    ini_si("global", "excludeCap", g_cfg.excludeCap);
    ini_si("global", "vsync", g_cfg.vsync);
    ini_si("global", "maxFps", g_cfg.maxFps);
    ini_si("global", "startHidden", g_cfg.startHidden);
    ini_si("global", "capMode", g_cfg.capMode);
    ini_si("global", "targetMon", g_cfg.targetMon);
    ini_s("global", "targetTitle", g_cfg.targetTitle);
    for (int i = 0; i < 9; i++) {
        char k[16]; sprintf(k, "hkP%d", i); ini_si("global", k, g_cfg.hkPreset[i]);
    }
    profile_save_sec("default", &g_def);
    ini_si("global", "profiles", g_nprof);
    for (int i = 0; i < g_nprof; i++) {
        char sec[32]; sprintf(sec, "profile:%d", i);
        ini_s(sec, "name", g_prof[i].name);
        ini_s(sec, "process", g_prof[i].process);
        profile_save_sec(sec, &g_prof[i].p);
    }
}

/* --------------------------------------------------------------- D3D11 */

static HRESULT compile_shader(const char* src, const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr) && err) {
        logf_("compile %s: %s", entry, (char*)err->GetBufferPointer());
        err->Release();
    }
    return hr;
}

static ID3D11ShaderResourceView* make_srv(ID3D11Texture2D* t) {
    ID3D11ShaderResourceView* s = NULL;
    if (t) g_dev->CreateShaderResourceView(t, NULL, &s);
    return s;
}

static void release_targets(void) {
    if (g_rtv) { g_rtv->Release(); g_rtv = NULL; }
    for (int i = 0; i < 2; i++) {
        if (g_lumaSrv[i]) { g_lumaSrv[i]->Release(); g_lumaSrv[i] = NULL; }
        if (g_lumaRtv[i]) { g_lumaRtv[i]->Release(); g_lumaRtv[i] = NULL; }
        if (g_luma[i]) { g_luma[i]->Release(); g_luma[i] = NULL; }
    }
    if (g_blurHRtv) { g_blurHRtv->Release(); g_blurHRtv = NULL; }
    if (g_blurVRtv) { g_blurVRtv->Release(); g_blurVRtv = NULL; }
    if (g_blurH) { g_blurH->Release(); g_blurH = NULL; }
    if (g_blurV) { g_blurV->Release(); g_blurV = NULL; }
    if (g_blurSrv) { g_blurSrv->Release(); g_blurSrv = NULL; }
    if (g_histSrv) { g_histSrv->Release(); g_histSrv = NULL; }
    if (g_histTex) { g_histTex->Release(); g_histTex = NULL; }
    if (g_capSrv) { g_capSrv->Release(); g_capSrv = NULL; }
    if (g_dupTex) { g_dupTex->Release(); g_dupTex = NULL; }
    if (g_gdiTex) { g_gdiTex->Release(); g_gdiTex = NULL; }
    if (g_stage) { g_stage->Release(); g_stage = NULL; }
    g_capTex = NULL;
    g_sw = g_sh = 0;
}

static IDXGIAdapter1* adapter_for_monitor(HMONITOR mon) {
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfo(mon, &mi)) return NULL;
    IDXGIFactory1* fac = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return NULL;
    IDXGIAdapter1* a = NULL;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput* o = NULL;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            if (d.DesktopCoordinates.left == mi.rcMonitor.left &&
                d.DesktopCoordinates.top == mi.rcMonitor.top) {
                o->Release();
                fac->Release();
                return a;
            }
            o->Release();
        }
        a->Release();
    }
    fac->Release();
    return NULL;
}

static HRESULT init_d3d(void) {
    HMONITOR mon = MonitorFromWindow(g_over, MONITOR_DEFAULTTONEAREST);
    IDXGIAdapter1* ad = adapter_for_monitor(mon);
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                   &g_dev, &fl, &g_ctx);
    if (ad) { g_adapter = ad; } else {
        IDXGIDevice* dxd = NULL;
        g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxd);
        IDXGIAdapter* a2 = NULL;
        dxd->GetAdapter(&a2);
        g_adapter = (IDXGIAdapter1*)a2;
        dxd->Release();
    }
    if (FAILED(hr)) { logf_("D3D11CreateDevice echec 0x%08X", hr); return hr; }

    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_over;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGIFactory2* fac = NULL;
    g_adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
    hr = fac->CreateSwapChainForHwnd(g_dev, g_over, &sd, NULL, NULL, &g_swap);
    fac->Release();
    if (FAILED(hr)) { logf_("swapchain echec 0x%08X", hr); return hr; }

    ID3DBlob* b = NULL;
    if (FAILED(compile_shader(VS_FULL, "main", "vs_4_0", &b))) return E_FAIL;
    g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &g_vs);
    b->Release();
    if (FAILED(compile_shader(PS_LUMA, "main", "ps_4_0", &b))) return E_FAIL;
    g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &g_psLuma);
    b->Release();
    if (FAILED(compile_shader(PS_BLUR, "main", "ps_4_0", &b))) return E_FAIL;
    g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &g_psBlur);
    b->Release();
    if (FAILED(compile_shader(PS_COMP, "main", "ps_4_0", &b))) return E_FAIL;
    g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &g_psComp);
    b->Release();

    D3D11_SAMPLER_DESC smd = { 0 };
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smd, &g_smp);

    D3D11_RASTERIZER_DESC rd = { 0 };
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rs);

    D3D11_BUFFER_DESC bd = { 0 };
    bd.ByteWidth = 96;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, NULL, &g_cb);

    g_ready = 1;
    return S_OK;
}

static void prepare_size(int w, int h) {
    if (w < 16 || h < 16) return;
    if (w == g_sw && h == g_sh && g_rtv) return;
    release_targets();

    g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* bb = NULL;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_dev->CreateRenderTargetView(bb, NULL, &g_rtv);
    bb->Release();

    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&td, NULL, &g_dupTex);
    td.Usage = D3D11_USAGE_DYNAMIC; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateTexture2D(&td, NULL, &g_gdiTex);
    g_capTex = g_dupTex;
    g_capSrv = make_srv(g_dupTex);

    td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
    g_dev->CreateTexture2D(&td, NULL, &g_histTex);
    g_histSrv = make_srv(g_histTex);

    D3D11_TEXTURE2D_DESC st = td;
    st.Width = 64; st.Height = 36;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_dev->CreateTexture2D(&st, NULL, &g_stage);

    D3D11_TEXTURE2D_DESC ld = { 0 };
    ld.Width = w / 2; ld.Height = h / 2; ld.MipLevels = 1; ld.ArraySize = 1;
    ld.Format = DXGI_FORMAT_R16_FLOAT; ld.SampleDesc.Count = 1;
    ld.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; i++) {
        g_dev->CreateTexture2D(&ld, NULL, &g_luma[i]);
        g_dev->CreateRenderTargetView(g_luma[i], NULL, &g_lumaRtv[i]);
        g_lumaSrv[i] = make_srv(g_luma[i]);
    }
    g_dev->CreateTexture2D(&ld, NULL, &g_blurH);
    g_dev->CreateRenderTargetView(g_blurH, NULL, &g_blurHRtv);
    g_dev->CreateTexture2D(&ld, NULL, &g_blurV);
    g_dev->CreateRenderTargetView(g_blurV, NULL, &g_blurVRtv);
    g_blurSrv = make_srv(g_blurV);

    g_sw = w; g_sh = h;
}

static HRESULT init_dup(void) {
    if (g_dup) return S_OK;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(MonitorFromWindow(g_over, MONITOR_DEFAULTTONEAREST), &mi);

    IDXGIOutput* o = NULL;
    for (UINT i = 0; g_adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_OUTPUT_DESC d;
        o->GetDesc(&d);
        if (d.DesktopCoordinates.left == mi.rcMonitor.left &&
            d.DesktopCoordinates.top == mi.rcMonitor.top) {
            o->QueryInterface(__uuidof(IDXGIOutput1), (void**)&g_output);
            o->Release();
            break;
        }
        o->Release();
    }
    if (!g_output) { logf_("sortie DXGI introuvable"); return E_FAIL; }
    HRESULT hr = g_output->DuplicateOutput(g_dev, &g_dup);
    if (FAILED(hr)) logf_("DuplicateOutput echec 0x%08X", hr);
    return hr;
}

static void drop_dup(void) {
    if (g_dup) { g_dup->Release(); g_dup = NULL; }
    if (g_output) { g_output->Release(); g_output = NULL; }
}

static int grab_dup(void) {
    if (!g_dup) return 0;
    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource* res = NULL;
    HRESULT hr = g_dup->AcquireNextFrame(16, &fi, &res);
    if (hr == DXGI_ERROR_ACCESS_LOST) { drop_dup(); return 0; }
    if (FAILED(hr)) return 0;

    ID3D11Texture2D* t = NULL;
    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
    res->Release();
    if (t) {
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        prepare_size((int)d.Width, (int)d.Height);
        if (g_dupTex) g_ctx->CopyResource(g_dupTex, t);
        t->Release();
    }
    g_dup->ReleaseFrame();
    return 1;
}

static int grab_window(void) {
    HWND h = FindWindowA(NULL, g_cfg.targetTitle);
    if (!h) return 0;
    RECT r;
    GetClientRect(h, &r);
    int w = r.right, ht = r.bottom;
    if (w < 16 || ht < 16) return 0;

    if (!g_gbuf || w != g_gw || ht != g_gh) {
        if (g_gbmp) DeleteObject(g_gbmp);
        if (g_gmdc) DeleteDC(g_gmdc);
        if (g_gdc) ReleaseDC(NULL, g_gdc);
        g_gdc = GetWindowDC(h);
        g_gmdc = CreateCompatibleDC(g_gdc);
        g_gbmp = CreateCompatibleBitmap(g_gdc, w, ht);
        SelectObject(g_gmdc, g_gbmp);
        free(g_gbuf);
        g_gbuf = malloc((size_t)w * ht * 4);
        g_gw = w; g_gh = ht;
    }
    if (!g_gbuf) return 0;
    if (!PrintWindow(h, g_gmdc, 2)) return 0;

    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -ht;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    GetDIBits(g_gmdc, g_gbmp, 0, ht, g_gbuf, &bi, DIB_RGB_COLORS);

    prepare_size(w, ht);
    if (!g_gdiTex) return 0;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_gdiTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        for (int y = 0; y < ht; y++)
            memcpy((BYTE*)ms.pData + (size_t)y * ms.RowPitch,
                   (BYTE*)g_gbuf + (size_t)y * w * 4, (size_t)w * 4);
        g_ctx->Unmap(g_gdiTex, 0);
    }
    return 1;
}

/* --------------------------------------------------------------- rendu */

static void set_cb(const float* v) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, v, 96);
        g_ctx->Unmap(g_cb, 0);
    }
}

static void draw_tri(void) {
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->Draw(3, 0);
}

static void viewport(int w, int h) {
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
}

static void render_frame(void) {
    if (!g_ready || !g_captureOn || g_hidden) return;

    int ok = (g_cfg.capMode == 1) ? grab_window() : grab_dup();
    if (!ok) {
        if (g_cfg.capMode == 0 && !g_dup) init_dup();
        return;
    }
    if (!g_rtv || g_sw < 16) return;

    int hw = g_sw / 2, hh = g_sh / 2;
    g_ctx->RSSetState(g_rs);
    g_ctx->PSSetSamplers(0, 1, &g_smp);
    g_ctx->VSSetShader(g_vs, NULL, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);

    float c1[4] = { 1.0f / g_sw, 1.0f / g_sh, 0.0f, 0.0f };
    set_cb(c1);
    g_ctx->PSSetShader(g_psLuma, NULL, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_capSrv);
    g_ctx->PSSetShaderResources(1, 1, &g_lumaSrv[g_lumaIdx ^ 1]);
    g_ctx->OMSetRenderTargets(1, &g_lumaRtv[g_lumaIdx], NULL);
    viewport(hw, hh);
    draw_tri();

    float c2[4] = { 1.0f / hw, 1.0f / hh, 1.0f, 0.0f };
    set_cb(c2);
    g_ctx->PSSetShader(g_psBlur, NULL, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_lumaSrv[g_lumaIdx]);
    g_ctx->OMSetRenderTargets(1, &g_blurHRtv, NULL);
    draw_tri();

    float c3[4] = { 1.0f / hw, 1.0f / hh, 0.0f, 1.0f };
    set_cb(c3);
    g_ctx->PSSetShaderResources(0, 1, &g_blurH ? &g_blurSrv : &g_blurSrv);
    {
        ID3D11ShaderResourceView* hsrv = NULL;
        g_blurH->QueryInterface(__uuidof(ID3D11ShaderResourceView), (void**)&hsrv);
        g_ctx->PSSetShaderResources(0, 1, &hsrv);
        g_ctx->OMSetRenderTargets(1, &g_blurVRtv, NULL);
        draw_tri();
        hsrv->Release();
    }

    float c[24] = {
        1.0f / g_sw, 1.0f / g_sh, (float)g_sw / (float)(g_sh ? g_sh : 1), (float)(g_frame % 64),
        g_p.exposure, g_p.gamma, g_p.local, g_p.lift,
        g_p.sharpen, g_p.contrast, g_p.sat, g_p.vibrance,
        g_p.temp, g_p.rolloff, g_p.vig, g_p.blacks,
        g_p.autoAmt, 0.18f, g_compare ? 0.5f : -1.0f, g_p.dither,
        g_p.temporal, 0.0f, 0.0f, 0.0f
    };
    set_cb(c);
    g_ctx->PSSetShader(g_psComp, NULL, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_capSrv);
    g_ctx->PSSetShaderResources(1, 1, &g_blurSrv);
    g_ctx->PSSetShaderResources(2, 1, &g_histSrv);
    g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
    viewport(g_sw, g_sh);
    draw_tri();

    if (g_p.temporal > 0.01f && g_histTex) {
        ID3D11Texture2D* bb = NULL;
        g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        if (bb) { g_ctx->CopyResource(g_histTex, bb); bb->Release(); }
    }

    g_lumaIdx ^= 1;
    g_frame++;
    g_swap->Present(g_cfg.vsync ? 1 : 0, 0);
}

static void hist_update(void) {
    if (!g_stage || !g_capTex || g_sw < 64 || g_sh < 36) return;
    D3D11_BOX box = { 0, 0, 0, 64, 36, 1 };
    g_ctx->CopySubresourceRegion(g_stage, 0, 0, 0, 0, g_capTex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_stage, 0, D3D11_MAP_READ, 0, &ms))) return;
    memset(g_hist, 0, sizeof(g_hist));
    for (int y = 0; y < 36; y++) {
        BYTE* row = (BYTE*)ms.pData + (size_t)y * ms.RowPitch;
        for (int x = 0; x < 64; x++) {
            BYTE* px = row + x * 4;
            float l = (0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2]) / 255.0f;
            g_hist[(int)(l * 63.0f)] += 1.0f;
        }
    }
    g_ctx->Unmap(g_stage, 0);
}