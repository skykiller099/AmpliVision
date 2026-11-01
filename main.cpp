#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <shellapi.h>
#include <stdarg.h>
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
static char g_status[160] = "Pret.";

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain1* g_swap;
static ID3D11RenderTargetView* g_rtv;
static ID3D11Texture2D *g_dupTex, *g_gdiTex, *g_capTex;
static ID3D11ShaderResourceView *g_capSrv, *g_blurSrv, *g_histSrv;
static ID3D11Texture2D *g_luma[2], *g_blurH, *g_blurV, *g_histTex;
static ID3D11RenderTargetView *g_lumaRtv[2], *g_blurHRtv, *g_blurVRtv;
static ID3D11ShaderResourceView *g_lumaSrv[2], *g_blurHSrv;
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

static HWND g_gwnd;
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
static void apply_overlay_style(void);
static void place_overlay(void);

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
    if (g_blurHSrv) { g_blurHSrv->Release(); g_blurHSrv = NULL; }
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
    if (ad) {
        g_adapter = ad;
    } else {
        IDXGIDevice* dxd = NULL;
        g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxd);
        IDXGIAdapter* a2 = NULL;
        dxd->GetAdapter(&a2);
        g_adapter = (IDXGIAdapter1*)a2;
        dxd->Release();
    }
    if (FAILED(hr)) { logf_("D3D11CreateDevice echec 0x%08X", hr); return hr; }

    DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
    sd.Width = 0;
    sd.Height = 0;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.Stereo = FALSE;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = 0;

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

    g_swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* bb = NULL;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_dev->CreateRenderTargetView(bb, NULL, &g_rtv);
    bb->Release();

    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&td, NULL, &g_dupTex);
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateTexture2D(&td, NULL, &g_gdiTex);
    g_capTex = g_dupTex;
    g_capSrv = make_srv(g_dupTex);

    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    g_dev->CreateTexture2D(&td, NULL, &g_histTex);
    g_histSrv = make_srv(g_histTex);

    D3D11_TEXTURE2D_DESC st = td;
    st.Width = 64;
    st.Height = 36;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_dev->CreateTexture2D(&st, NULL, &g_stage);

    D3D11_TEXTURE2D_DESC ld = { 0 };
    ld.Width = (UINT)(w / 2);
    ld.Height = (UINT)(h / 2);
    ld.MipLevels = 1;
    ld.ArraySize = 1;
    ld.Format = DXGI_FORMAT_R16_FLOAT;
    ld.SampleDesc.Count = 1;
    ld.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; i++) {
        g_dev->CreateTexture2D(&ld, NULL, &g_luma[i]);
        g_dev->CreateRenderTargetView(g_luma[i], NULL, &g_lumaRtv[i]);
        g_lumaSrv[i] = make_srv(g_luma[i]);
    }
    g_dev->CreateTexture2D(&ld, NULL, &g_blurH);
    g_dev->CreateRenderTargetView(g_blurH, NULL, &g_blurHRtv);
    g_blurHSrv = make_srv(g_blurH);
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

static void gdi_release(void) {
    if (g_gbmp) { DeleteObject(g_gbmp); g_gbmp = NULL; }
    if (g_gmdc) { DeleteDC(g_gmdc); g_gmdc = NULL; }
    if (g_gdc) { ReleaseDC(g_gwnd, g_gdc); g_gdc = NULL; }
    free(g_gbuf);
    g_gbuf = NULL;
    g_gw = g_gh = 0;
    g_gwnd = NULL;
}

static int grab_window(void) {
    HWND h = FindWindowA(NULL, g_cfg.targetTitle);
    if (!h) return 0;
    RECT r;
    GetClientRect(h, &r);
    int w = r.right, ht = r.bottom;
    if (w < 16 || ht < 16) return 0;

    if (!g_gbuf || w != g_gw || ht != g_gh || h != g_gwnd) {
        gdi_release();
        g_gwnd = h;
        g_gdc = GetWindowDC(h);
        g_gmdc = CreateCompatibleDC(g_gdc);
        g_gbmp = CreateCompatibleBitmap(g_gdc, w, ht);
        SelectObject(g_gmdc, g_gbmp);
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
    g_ctx->PSSetShaderResources(0, 1, &g_blurHSrv);
    g_ctx->OMSetRenderTargets(1, &g_blurVRtv, NULL);
    draw_tri();

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

/* ------------------------------------------------------------------ UI */

typedef struct { RECT r; int type; void* target; float mn, mx; } Hit;
static Hit g_hits[220];
static int g_nhits;
static int g_active = -1;

static void ui_fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
static void ui_text(HDC dc, int x, int y, const char* s, COLORREF c, int bold) {
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    HFONT f = CreateFontA(bold ? 15 : 13, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                          0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT old = (HFONT)SelectObject(dc, f);
    TextOutA(dc, x, y, s, lstrlenA(s));
    SelectObject(dc, old);
    DeleteObject(f);
}
static void ui_slider(HDC dc, int* y, int x, int w, const char* label, float* v, float mn, float mx) {
    char b[32];
    ui_text(dc, x, *y, label, RGB(150, 160, 175), 0);
    sprintf(b, "%.2f", *v);
    ui_text(dc, x + w - 40, *y, b, RGB(245, 197, 66), 0);
    int ty = *y + 18;
    ui_fill(dc, x, ty, w, 4, RGB(40, 46, 58));
    float rel = (*v - mn) / (mx - mn);
    if (rel < 0) rel = 0;
    if (rel > 1) rel = 1;
    ui_fill(dc, x, ty, (int)(w * rel), 4, RGB(245, 197, 66));
    ui_fill(dc, x + (int)(w * rel) - 5, ty - 4, 10, 12, RGB(255, 255, 255));
    if (g_nhits < 220) {
        RECT r = { x, ty - 9, x + w, ty + 15 };
        g_hits[g_nhits].r = r;
        g_hits[g_nhits].type = 1;
        g_hits[g_nhits].target = v;
        g_hits[g_nhits].mn = mn;
        g_hits[g_nhits].mx = mx;
        g_nhits++;
    }
    *y += 40;
}
static void ui_toggle(HDC dc, int* y, int x, int w, const char* label, int* v) {
    ui_text(dc, x, *y, label, RGB(200, 208, 220), 0);
    int bx = x + w - 42;
    ui_fill(dc, bx, *y, 42, 18, *v ? RGB(245, 197, 66) : RGB(40, 46, 58));
    ui_fill(dc, *v ? bx + 24 : bx + 2, *y + 2, 16, 14, RGB(255, 255, 255));
    if (g_nhits < 220) {
        RECT r = { bx, *y - 3, bx + 42, *y + 21 };
        g_hits[g_nhits].r = r;
        g_hits[g_nhits].type = 2;
        g_hits[g_nhits].target = v;
        g_nhits++;
    }
    *y += 30;
}
static void ui_button(HDC dc, int x, int y, int w, int h, const char* label, int on, int id) {
    ui_fill(dc, x, y, w, h, on ? RGB(245, 197, 66) : RGB(32, 37, 46));
    ui_text(dc, x + 10, y + 4, label, on ? RGB(18, 21, 27) : RGB(215, 222, 232), 0);
    if (g_nhits < 220) {
        RECT r = { x, y, x + w, y + h };
        g_hits[g_nhits].r = r;
        g_hits[g_nhits].type = 3;
        g_hits[g_nhits].target = (void*)(INT_PTR)id;
        g_nhits++;
    }
}

static const char* VK_NAME(int vk) {
    static char b[16];
    if (vk >= VK_F1 && vk <= VK_F24) { sprintf(b, "F%d", vk - VK_F1 + 1); return b; }
    if (vk >= '0' && vk <= '9') { b[0] = (char)vk; b[1] = 0; return b; }
    if (vk >= 'A' && vk <= 'Z') { b[0] = (char)vk; b[1] = 0; return b; }
    sprintf(b, "0x%02X", vk);
    return b;
}

static const char* PRESET_NAME[] = { "Discret", "Plein jour", "Vision nocturne",
                                     "FPS comp", "Horreur", "Maximum" };
static const Params PRESET[6] = {
    { 1.25f, 1.20f, 0.28f, 0.03f, 0.35f, 1.04f, 1.05f, 0.10f, 0.00f, 0.40f, 0.20f, 0.02f, 0.00f, 0.25f, 0.20f },
    { 2.40f, 1.75f, 0.72f, 0.14f, 0.70f, 1.16f, 1.28f, 0.25f, 0.02f, 0.70f, 0.55f, 0.05f, 0.00f, 0.35f, 0.30f },
    { 2.00f, 2.00f, 0.85f, 0.20f, 1.00f, 1.10f, 0.30f, 0.05f, -0.25f, 0.75f, 0.60f, 0.06f, 0.00f, 0.45f, 0.45f },
    { 2.00f, 1.55f, 0.60f, 0.05f, 1.30f, 1.26f, 0.82f, 0.20f, 0.00f, 0.55f, 0.50f, 0.04f, 0.00f, 0.20f, 0.10f },
    { 1.60f, 1.40f, 0.50f, 0.08f, 0.55f, 1.20f, 0.85f, 0.15f, 0.12f, 0.45f, 0.30f, 0.03f, 0.00f, 0.30f, 0.25f },
    { 3.40f, 2.30f, 1.00f, 0.24f, 1.50f, 1.32f, 1.30f, 0.30f, 0.00f, 0.95f, 0.80f, 0.08f, 0.40f, 0.55f, 0.55f }
};

static BOOL CALLBACK mon_cb(HMONITOR m, HDC d, LPRECT r, LPARAM l) {
    (void)d; (void)r; (void)l;
    if (g_nmon < MAX_MON) g_mons[g_nmon++] = m;
    return TRUE;
}
static void refresh_monitors(void) {
    g_nmon = 0;
    EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
}

static void paint_panel(HDC dc) {
    g_nhits = 0;
    ui_fill(dc, 0, 0, PANEL_W, PANEL_H, RGB(18, 21, 27));
    ui_text(dc, 20, 12, "AmpliVision", RGB(240, 244, 250), 1);
    ui_text(dc, 148, 16, "F8 afficher/masquer  ·  F9 panneau  ·  F7 comparer", RGB(110, 120, 135), 0);
    if (g_warnHotkey) ui_text(dc, 560, 16, "raccourci deja reserve", RGB(230, 120, 90), 0);

    int bx = 20;
    for (int i = 0; i < 6; i++) {
        char b[40];
        sprintf(b, "%d  %s", i + 1, PRESET_NAME[i]);
        ui_button(dc, bx, 42, 100, 24, b, 0, 100 + i);
        bx += 106;
    }
    ui_button(dc, bx, 42, 110, 24, g_captureOn ? "Arreter" : "Demarrer", g_captureOn, 1);

    ui_fill(dc, 0, 78, PANEL_W, 1, RGB(38, 44, 55));

    int cy = 92;
    ui_text(dc, 20, cy, "Image", RGB(245, 197, 66), 1);
    cy += 24;
    ui_slider(dc, &cy, 20, 270, "Exposition", &g_p.exposure, 0.2f, 4.0f);
    ui_slider(dc, &cy, 20, 270, "Gamma", &g_p.gamma, 0.5f, 3.0f);
    ui_slider(dc, &cy, 20, 270, "Gain adaptatif des ombres", &g_p.local, 0.0f, 1.0f);
    ui_slider(dc, &cy, 20, 270, "Releve des noirs", &g_p.blacks, 0.0f, 0.5f);
    ui_slider(dc, &cy, 20, 270, "Nettete", &g_p.sharpen, 0.0f, 1.6f);
    ui_slider(dc, &cy, 20, 270, "Contraste", &g_p.contrast, 0.6f, 2.0f);
    ui_slider(dc, &cy, 20, 270, "Saturation", &g_p.sat, 0.0f, 2.0f);
    ui_slider(dc, &cy, 20, 270, "Vibrance", &g_p.vibrance, 0.0f, 1.0f);
    ui_slider(dc, &cy, 20, 270, "Temperature", &g_p.temp, -1.0f, 1.0f);
    ui_slider(dc, &cy, 20, 270, "Compression hautes lumieres", &g_p.rolloff, 0.0f, 1.0f);
    ui_slider(dc, &cy, 20, 270, "Vignettage", &g_p.vig, 0.0f, 1.0f);

    int dy = 92;
    ui_text(dc, 315, dy, "Traitement", RGB(245, 197, 66), 1);
    dy += 24;
    ui_slider(dc, &dy, 315, 270, "Denoise temporelle", &g_p.temporal, 0.0f, 0.9f);
    ui_slider(dc, &dy, 315, 270, "Anti-banding (dither)", &g_p.dither, 0.0f, 1.0f);
    ui_slider(dc, &dy, 315, 270, "Auto-exposition", &g_p.autoAmt, 0.0f, 1.0f);

    ui_text(dc, 315, dy + 6, "Histogramme de la scene", RGB(245, 197, 66), 1);
    dy += 26;
    ui_fill(dc, 315, dy, 270, 62, RGB(26, 30, 38));
    for (int i = 0; i < 64; i++) {
        int hh = (int)(g_hist[i] * 1.6f);
        if (hh > 58) hh = 58;
        ui_fill(dc, 316 + i * 4, dy + 60 - hh, 3, hh, RGB(245, 197, 66));
    }
    dy += 74;

    ui_text(dc, 315, dy, "Systeme", RGB(245, 197, 66), 1);
    dy += 24;
    ui_toggle(dc, &dy, 315, 270, "Clic traversant", &g_cfg.clickThrough);
    ui_toggle(dc, &dy, 315, 270, "Exclure l'overlay de la capture", &g_cfg.excludeCap);
    ui_toggle(dc, &dy, 315, 270, "V-Sync", &g_cfg.vsync);
    ui_toggle(dc, &dy, 315, 270, "Demarrer masque", &g_cfg.startHidden);
    ui_button(dc, 315, dy + 4, 130, 24, "Sauver", 0, 2);
    ui_button(dc, 455, dy + 4, 130, 24, "Reinitialiser", 0, 3);

    int ey = 92;
    ui_text(dc, 610, ey, "Raccourcis", RGB(245, 197, 66), 1);
    ey += 24;
    const char* hkl[3] = { "Afficher / masquer", "Panneau", "Comparer (maintien)" };
    int* hkv[3] = { &g_cfg.hkToggle, &g_cfg.hkPanel, &g_cfg.hkCompare };
    for (int i = 0; i < 3; i++) {
        ui_text(dc, 610, ey, hkl[i], RGB(200, 208, 220), 0);
        char b[24];
        sprintf(b, "%s", g_rebind == 200 + i ? "..." : VK_NAME(*hkv[i]));
        ui_button(dc, 790, ey - 3, 110, 22, b, g_rebind == 200 + i, 200 + i);
        ey += 28;
    }

    ui_text(dc, 610, ey + 4, "Ecran cible", RGB(245, 197, 66), 1);
    ey += 26;
    refresh_monitors();
    for (int i = 0; i < g_nmon && i < 4; i++) {
        char b[24];
        sprintf(b, "Ecran %d", i + 1);
        ui_button(dc, 610 + i * 62, ey, 56, 22, b, g_cfg.targetMon == i, 300 + i);
    }
    ey += 32;

    ui_text(dc, 610, ey, "Profils par jeu", RGB(245, 197, 66), 1);
    ey += 24;
    for (int i = 0; i < g_nprof && i < 8; i++) {
        ui_fill(dc, 610, ey + i * 22, 290, 20, i == g_selProf ? RGB(52, 60, 74) : RGB(28, 32, 40));
        ui_text(dc, 616, ey + i * 22 + 2, g_prof[i].name, RGB(220, 226, 236), 0);
        ui_text(dc, 616 + 130, ey + i * 22 + 2,
                g_prof[i].process[0] ? g_prof[i].process : "-", RGB(120, 130, 145), 0);
        if (g_nhits < 220) {
            RECT r = { 610, ey + i * 22, 900, ey + i * 22 + 20 };
            g_hits[g_nhits].r = r;
            g_hits[g_nhits].type = 4;
            g_hits[g_nhits].target = (void*)(INT_PTR)i;
            g_nhits++;
        }
    }
    int py = ey + 8 * 22 + 4;
    ui_button(dc, 610, py, 140, 22, "Nouveau depuis reglages", 0, 400);
    ui_button(dc, 758, py, 142, 22, "Lier au jeu actif", 0, 401);
    py += 26;
    ui_button(dc, 610, py, 90, 22, "Charger", 0, 402);
    ui_button(dc, 708, py, 90, 22, "Supprimer", 0, 403);
    ui_button(dc, 806, py, 94, 22, "Exporter", 0, 404);

    ui_fill(dc, 0, PANEL_H - 26, PANEL_W, 1, RGB(38, 44, 55));
    ui_text(dc, 20, PANEL_H - 20, g_status, RGB(120, 130, 145), 0);
}

static void apply_overlay_style(void) {
    LONG ex = GetWindowLongW(g_over, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (g_cfg.clickThrough) ex |= WS_EX_TRANSPARENT;
    else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongW(g_over, GWL_EXSTYLE, ex);
    SetWindowPos(g_over, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowDisplayAffinity(g_over, g_cfg.excludeCap ? WDA_EXCL : 0);
}

static void place_overlay(void) {
    HMONITOR mon = MonitorFromWindow(g_over, MONITOR_DEFAULTTONEAREST);
    refresh_monitors();
    if (g_cfg.targetMon >= 0 && g_cfg.targetMon < g_nmon) mon = g_mons[g_cfg.targetMon];
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(mon, &mi);
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    SetWindowPos(g_over, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_NOACTIVATE);
    if (g_ready) prepare_size(w, h);
    drop_dup();
}

static void toggle_overlay(void) {
    g_hidden = !g_hidden;
    ShowWindow(g_over, g_hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (!g_hidden) {
        place_overlay();
        apply_overlay_style();
    } else {
        drop_dup();
    }
}

static void toggle_panel(void) {
    g_panelOpen = !g_panelOpen;
    if (g_panelOpen) {
        RECT wa;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(g_panel, HWND_TOPMOST,
                     wa.right - PANEL_W - 20, wa.top + 20, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(g_panel, SW_SHOWNOACTIVATE);
        InvalidateRect(g_panel, NULL, FALSE);
    } else {
        ShowWindow(g_panel, SW_HIDE);
    }
}

/* -------------------------------------------------------------- profils */

static void foreground_exe(char* out) {
    out[0] = 0;
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    char path[MAX_PATH] = { 0 };
    DWORD sz = MAX_PATH;
    if (QueryFullProcessImageNameA(p, 0, path, &sz)) {
        char* s = strrchr(path, '\\');
        lstrcpynA(out, s ? s + 1 : path, 64);
    }
    CloseHandle(p);
}

static void profile_apply(int i) {
    if (i < 0 || i >= g_nprof) return;
    g_p = g_prof[i].p;
    g_selProf = i;
    wsprintfA(g_status, "Profil %s charge.", g_prof[i].name);
}

static void auto_profile(void) {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1200) return;
    last = now;
    char exe[64];
    foreground_exe(exe);
    if (!exe[0]) return;
    for (int i = 0; i < g_nprof; i++) {
        if (g_prof[i].process[0] && lstrcmpiA(g_prof[i].process, exe) == 0) {
            if (g_selProf != i) profile_apply(i);
            return;
        }
    }
}

static LRESULT CALLBACK name_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == 1) {
            GetWindowTextA(g_nameEdit, g_prof[g_nprof].name, 64);
            g_prof[g_nprof].p = g_p;
            foreground_exe(g_prof[g_nprof].process);
            g_nprof++;
            g_selProf = g_nprof - 1;
            save_ini();
            DestroyWindow(h);
            g_nameDlg = NULL;
            InvalidateRect(g_panel, NULL, FALSE);
        } else if (LOWORD(w) == 2) {
            DestroyWindow(h);
            g_nameDlg = NULL;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        g_nameDlg = NULL;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void ask_profile_name(void) {
    if (g_nprof >= MAX_PROFILES) {
        lstrcpyA(g_status, "Nombre maximal de profils atteint.");
        return;
    }
    if (g_nameDlg) return;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = name_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(24, 28, 36));
    wc.lpszClassName = "AmpliName";
    RegisterClassA(&wc);

    RECT wa;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    g_nameDlg = CreateWindowExA(WS_EX_TOPMOST, "AmpliName", "Nom du profil",
                                WS_POPUP | WS_BORDER, wa.left + 300, wa.top + 240, 340, 108,
                                NULL, NULL, g_inst, NULL);
    g_nameEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 16, 16, 308, 24, g_nameDlg, NULL, g_inst, NULL);
    CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  150, 54, 80, 26, g_nameDlg, (HMENU)1, g_inst, NULL);
    CreateWindowA("BUTTON", "Annuler", WS_CHILD | WS_VISIBLE,
                  240, 54, 84, 26, g_nameDlg, (HMENU)2, g_inst, NULL);
    ShowWindow(g_nameDlg, SW_SHOW);
    SetFocus(g_nameEdit);
}

/* ----------------------------------------------------------- fenetres */

static void on_hit(int id) {
    if (id >= 100 && id < 106) { g_p = PRESET[id - 100]; return; }
    if (id >= 300 && id < 304) {
        g_cfg.targetMon = id - 300;
        place_overlay();
        apply_overlay_style();
        return;
    }
    if (id >= 200 && id < 203) { g_rebind = id; return; }
    switch (id) {
    case 1:
        g_captureOn = !g_captureOn;
        if (!g_captureOn) drop_dup();
        break;
    case 2:
        save_ini();
        lstrcpyA(g_status, "Reglages sauvegardes.");
        break;
    case 3:
        g_p = g_def;
        break;
    case 400:
        ask_profile_name();
        break;
    case 401: {
        char exe[64];
        foreground_exe(exe);
        if (g_selProf >= 0 && exe[0]) {
            lstrcpyA(g_prof[g_selProf].process, exe);
            wsprintfA(g_status, "Profil lie a %s.", exe);
            save_ini();
        }
        break;
    }
    case 402:
        profile_apply(g_selProf);
        break;
    case 403:
        if (g_selProf >= 0) {
            for (int i = g_selProf; i < g_nprof - 1; i++) g_prof[i] = g_prof[i + 1];
            g_nprof--;
            g_selProf = -1;
            save_ini();
            lstrcpyA(g_status, "Profil supprime.");
        }
        break;
    case 404: {
        char path[MAX_PATH];
        char dir[MAX_PATH];
        lstrcpynA(dir, g_ini, MAX_PATH);
        char* s = strrchr(dir, '\\');
        if (s) *s = 0;
        wsprintfA(path, "%s\\profils-export.ini", dir);
        CopyFileA(g_ini, path, FALSE);
        wsprintfA(g_status, "Exporte vers %s", path);
        break;
    }
    }
}

static LRESULT CALLBACK over_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_DISPLAYCHANGE:
        place_overlay();
        apply_overlay_style();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, PANEL_W, PANEL_H);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint_panel(mem);
        BitBlt(dc, 0, 0, PANEL_W, PANEL_H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        g_mx = GET_X_LPARAM(l);
        g_my = GET_Y_LPARAM(l);
        if (g_active >= 0 && g_active < g_nhits && g_hits[g_active].type == 1) {
            Hit* t = &g_hits[g_active];
            float rel = (float)(g_mx - t->r.left) / (float)(t->r.right - t->r.left);
            if (rel < 0) rel = 0;
            if (rel > 1) rel = 1;
            *(float*)t->target = t->mn + rel * (t->mx - t->mn);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        g_mx = GET_X_LPARAM(l);
        g_my = GET_Y_LPARAM(l);
        POINT pt = { g_mx, g_my };
        for (int i = 0; i < g_nhits; i++) {
            if (PtInRect(&g_hits[i].r, pt)) {
                if (g_hits[i].type == 3) on_hit((int)(INT_PTR)g_hits[i].target);
                else if (g_hits[i].type == 2) *(int*)g_hits[i].target ^= 1;
                else if (g_hits[i].type == 4) g_selProf = (int)(INT_PTR)g_hits[i].target;
                else g_active = i;
                break;
            }
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        g_active = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        g_panelOpen = 0;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void rebind_done(int vk) {
    if (vk == VK_ESCAPE) { g_rebind = -1; return; }
    if (g_rebind == 200) {
        UnregisterHotKey(g_panel, HOT_TOGGLE);
        g_cfg.hkToggle = vk;
        RegisterHotKey(g_panel, HOT_TOGGLE, MOD_NOREPEAT, vk);
    } else if (g_rebind == 201) {
        UnregisterHotKey(g_panel, HOT_PANEL);
        g_cfg.hkPanel = vk;
        RegisterHotKey(g_panel, HOT_PANEL, MOD_NOREPEAT, vk);
    } else if (g_rebind == 202) {
        g_cfg.hkCompare = vk;
    }
    save_ini();
    g_rebind = -1;
}

/* --------------------------------------------------------------- main */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

    typedef BOOL (WINAPI * PFN_DPI)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_DPI dpi = (PFN_DPI)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (dpi) dpi((HANDLE)-4);

    get_ini_path();
    load_ini();
    g_p = g_def;

    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW };
    wc.lpfnWndProc = over_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = CLS_OVER;
    RegisterClassExW(&wc);

    g_over = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                             CLS_OVER, APPNAME, WS_POPUP,
                             0, 0, 1920, 1080, NULL, NULL, inst, NULL);
    if (!g_over) { logf_("overlay non cree"); return 1; }

    wc.lpfnWndProc = panel_proc;
    wc.lpszClassName = CLS_PANEL;
    RegisterClassExW(&wc);
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW, CLS_PANEL, L"AmpliVision",
                              WS_POPUP | WS_BORDER, 100, 100, PANEL_W, PANEL_H,
                              NULL, NULL, inst, NULL);

    if (FAILED(init_d3d())) {
        MessageBoxW(NULL, L"Direct3D 11 indisponible. Verifie les pilotes graphiques.",
                    APPNAME, MB_ICONERROR);
        return 1;
    }

    place_overlay();
    apply_overlay_style();
    g_captureOn = 1;
    if (g_cfg.startHidden) {
        g_hidden = 1;
        ShowWindow(g_over, SW_HIDE);
    } else {
        ShowWindow(g_over, SW_SHOWNOACTIVATE);
    }

    if (!RegisterHotKey(g_panel, HOT_TOGGLE, MOD_NOREPEAT, g_cfg.hkToggle)) g_warnHotkey = 1;
    if (!RegisterHotKey(g_panel, HOT_PANEL, MOD_NOREPEAT, g_cfg.hkPanel)) g_warnHotkey = 1;
    for (int i = 0; i < 9; i++)
        if (!RegisterHotKey(g_panel, HOT_PRESET + i, MOD_NOREPEAT, g_cfg.hkPreset[i])) g_warnHotkey = 1;

    MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == HOT_TOGGLE) toggle_overlay();
                else if (msg.wParam == HOT_PANEL) toggle_panel();
                else if (msg.wParam >= HOT_PRESET && msg.wParam < HOT_PRESET + 9) {
                    int i = (int)msg.wParam - HOT_PRESET;
                    if (i < 6) g_p = PRESET[i];
                }
                continue;
            }
            if (msg.message == WM_QUIT) { g_running = 0; break; }
            if (g_rebind >= 0 && msg.message == WM_KEYDOWN) {
                rebind_done((int)msg.wParam);
                InvalidateRect(g_panel, NULL, FALSE);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        g_compare = (GetAsyncKeyState(g_cfg.hkCompare) & 0x8000) != 0;
        auto_profile();

        ULONGLONG t0 = GetTickCount64();
        if (!g_hidden) {
            render_frame();
            hist_update();
        }
        if (g_panelOpen) InvalidateRect(g_panel, NULL, FALSE);

        int budget = 1000 / (g_cfg.maxFps > 0 ? g_cfg.maxFps : 120);
        int spent = (int)(GetTickCount64() - t0);
        if (spent < budget) Sleep(budget - spent);
    }

    UnregisterHotKey(g_panel, HOT_TOGGLE);
    UnregisterHotKey(g_panel, HOT_PANEL);
    for (int i = 0; i < 9; i++) UnregisterHotKey(g_panel, HOT_PRESET + i);
    save_ini();
    drop_dup();
    gdi_release();
    release_targets();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    if (g_log) fclose(g_log);
    return 0;
}