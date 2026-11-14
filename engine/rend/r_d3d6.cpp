/* r_d3d6.cpp - Direct3D 6 backend (DESIGN 3; IDirectDraw4 / IDirect3D3 / IDirect3DDevice3). The only file that
   includes d3d.h. Owner: T05. Started from spike/probe3d.cpp; follows spike/RESULTS.md and the orchestrator
   decisions at the top of DESIGN 3:
   - Own CPU transform of RVert1/RVert2 to D3DTLVERTEX-style vertices (XYZRHW|DIFFUSE|TEX1/TEX2) and own clipping
     (near, far, and the four screen edges: the Voodoo3 has no guard band), then DrawIndexedPrimitive from system
     memory with D3DDP_DONOTCLIP. "Vertex buffers" (r_vb_create) are system-memory copies; r_draw_vb transforms only
     the index range it uses.
   - W-based table fog (never vertex fog), W-buffer when available, D3DDP lighting never used (vertex colours final).
   - Stage-1 ops (RS1_DETAIL = MODULATE2X, RS1_ENVADD = ADD) are probed with ValidateDevice at init; when the device
     rejects them (the Voodoo3 does) they are drawn as a second pass (DESTCOLOR/SRCCOLOR = 2x modulate, ONE/ONE add).
   - Textures: DDSCAPS2_TEXTUREMANAGE when accepted, else sysmem copy + vidmem texture loaded with Load() (the copy
     restores the texture after a device loss). Our own KB accounting against a budget.
   - Pacing (3.13): refresh measured with WaitForVerticalBlank over 30 blanks, divisor = round(refresh / fpsTarget),
     present right after the divisor-th blank (late frames go at the next blank), pace statistics in RPace.
   - Render-state cache: every SetRenderState / SetTextureStageState / SetTexture goes through a cache; RStats counts
     the real calls. */
#define WIN32_LEAN_AND_MEAN
#define _ENABLE_AUTODEPEND          /* skips the Watcom-only "#pragma read_only_file" */
#define DIRECTDRAW_VERSION 0x0600   /* DDCAPS = DDCAPS_DX6 */
#define DIRECT3D_VERSION   0x0600   /* stay at DX6 (the headers default to 0x0700) */
#include <windows.h>
#include <ddraw.h>                  /* /Isdk wins over VC6's own DDRAW.H */
#include <d3d.h>

#include "r_backend.h"
#include "e_log.h"
#include "e_image.h"
#include "e_prof.h"
#include "e_cfg.h"

/* ================================================================ types and globals */
enum { MAX_TEX = 1024, MAX_VB = 4096, MAX_MODES = 64 };
enum { MAXV = 65535, MAXI = 3 * 65536 };
enum { TF_565, TF_555, TF_1555, TF_4444, TF_8888, TF_COUNT };
static const char *const s_tfName[TF_COUNT] = { "565", "555", "1555", "4444", "8888" };

typedef struct TLV1 { float sx, sy, sz, rhw; DWORD color; float u, v; } TLV1;               /* 28 B */
typedef struct TLV2 { float sx, sy, sz, rhw; DWORD color; float u, v, u2, v2; } TLV2;       /* 36 B */
#define FVF_TLV1 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FVF_TLV2 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2)
#define DIP_FLAGS (D3DDP_DONOTCLIP | D3DDP_DONOTUPDATEEXTENTS)

typedef struct PFmt { DDPIXELFORMAT pf; int ok, bytes; int rs, rb, gs, gb, bs, bb, as, ab; } PFmt;

typedef struct DTex {
    LPDIRECTDRAWSURFACE4 surf; LPDIRECT3DTEXTURE2 tex;   /* the texture used for drawing */
    LPDIRECTDRAWSURFACE4 sys; LPDIRECT3DTEXTURE2 systex; /* unmanaged only: sysmem copy for Load / restore */
    int w, h, levels, kb, fmt, flags, used;
} DTex;

typedef struct DVb { int fmt, count; u8 *verts; } DVb;

typedef struct CV { float x, y, z, w, r, g, b, a, u, v, u2, v2; } CV;   /* clip-space vertex for the polygon clipper */

static HWND                 s_hwnd;
static LPDIRECTDRAW         s_dd1;
static LPDIRECTDRAW4        s_dd;
static LPDIRECT3D3          s_d3d;
static LPDIRECTDRAWSURFACE4 s_prim, s_back, s_zbuf;
static LPDIRECTDRAWCLIPPER  s_clip;
static LPDIRECT3DDEVICE3    s_dev;
static LPDIRECT3DVIEWPORT3  s_vp;
static RInit  s_ri;
static RCaps  s_cp;
static int    s_W, s_H, s_fs, s_bpp, s_inScene, s_lost;
static int    s_vx, s_vy, s_vw, s_vh;
static GUID   s_devGuid;
static int    s_haveDev;
static char   s_devName[64];
static D3DDEVICEDESC s_hal;
static DDPIXELFORMAT s_zfmt;
static int    s_haveZ, s_zBitsWant;
static PFmt   s_tf[TF_COUNT];
static int    s_oldPace;                             /* T16: -oldpace = T05 blank-counting present loop */
static int    s_flipLate, s_flipFirst;               /* T16: flips not done at the first blank wait / done */
static int    s_pace1;                               /* T16: -pace1 = spin + one blank wait, then Flip (fullscreen) */
static int    s_lodBiasCap;                          /* T16: D3DPRASTERCAPS_MIPMAPLODBIAS (RMaterial.lodBias) */
static int    s_zmode, s_wfog, s_zbiasCap, s_twoPassOk, s_mt2x, s_mtAdd, s_managed;
static int    s_texBudgetKB, s_texKB, s_texCount;
static int    s_modeN, s_modeW[MAX_MODES], s_modeH[MAX_MODES], s_modeB[MAX_MODES];

static DTex   s_tex[MAX_TEX];
static DVb    s_vb[MAX_VB];

static Mat4   s_proj, s_view, s_world, s_wvp;
static int    s_wvpDirty;
static int    s_fogOn;
static u32    s_fogRGB;
static float  s_fogStart, s_fogEnd;
static RMaterial s_mat;
static int    s_pass2;                              /* current material needs a second pass */

static RStats s_st;
static int    s_cntClip = -1;                       /* e_prof counter: triangles that went through the clipper */
static int    s_secXform = -1, s_secSubmit = -1, s_secClear = -1;   /* e_prof sections (bench breakdown) */

/* render-state cache */
static DWORD  s_rsVal[256];
static u8     s_rsOk[256];
static DWORD  s_tssVal[3][32];
static u8     s_tssOk[3][32];
static RTex   s_curTex[3];
static int    s_curTexOk[3];

/* scratch */
static u8    *s_tlv;                                /* MAXV * sizeof(TLV2) */
static float *s_clipv;                              /* MAXV * 4 */
static u8    *s_code;                               /* MAXV */
static u16   *s_oidx;                               /* MAXI */

/* pacing */
static s64    s_qpf, s_periodFx, s_lastPresent, s_cpuPeriod;
static int    s_vbOk, s_div;
static RPace  s_pace;
static s64    s_paceLogAt;

static s64 qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }

/* ================================================================ small helpers */
static const char *hrname(HRESULT hr)
{
    switch (hr) {
    case DD_OK: return "OK";
    case DDERR_SURFACELOST: return "SURFACELOST";
    case DDERR_OUTOFVIDEOMEMORY: return "OUTOFVIDEOMEMORY";
    case DDERR_OUTOFMEMORY: return "OUTOFMEMORY";
    case DDERR_INVALIDPARAMS: return "INVALIDPARAMS";
    case DDERR_INVALIDPIXELFORMAT: return "INVALIDPIXELFORMAT";
    case DDERR_NOEXCLUSIVEMODE: return "NOEXCLUSIVEMODE";
    case DDERR_WASSTILLDRAWING: return "WASSTILLDRAWING";
    case DDERR_UNSUPPORTED: return "UNSUPPORTED";
    case DDERR_INVALIDMODE: return "INVALIDMODE";
    case D3DERR_UNSUPPORTEDCOLOROPERATION: return "UNSUPPORTEDCOLOROPERATION";
    case D3DERR_UNSUPPORTEDCOLORARG: return "UNSUPPORTEDCOLORARG";
    case D3DERR_UNSUPPORTEDALPHAOPERATION: return "UNSUPPORTEDALPHAOPERATION";
    case D3DERR_UNSUPPORTEDALPHAARG: return "UNSUPPORTEDALPHAARG";
    case D3DERR_TOOMANYOPERATIONS: return "TOOMANYOPERATIONS";
    case D3DERR_CONFLICTINGTEXTUREFILTER: return "CONFLICTINGTEXTUREFILTER";
    case D3DERR_CONFLICTINGRENDERSTATE: return "CONFLICTINGRENDERSTATE";
    case D3DERR_WRONGTEXTUREFORMAT: return "WRONGTEXTUREFORMAT";
    case DDERR_WRONGMODE: return "WRONGMODE";
    }
    return "?";
}

static int fail(const char *what, HRESULT hr)
{
    log_printf(LOG_ERR, "err: rend %s hr=0x%08lX %s", what, (unsigned long)hr, hrname(hr));
    return 0;
}
#define CHK(call, what) do { HRESULT hr_ = (call); if (FAILED(hr_)) return fail(what, hr_); } while (0)

static DWORD fbits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

static int mshift(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mbits(DWORD m) { int b = 0; m >>= mshift(m); while (m & 1) { m >>= 1; b++; } return b; }

static void pf_setup(PFmt *f, const DDPIXELFORMAT *pf)
{
    f->pf = *pf;
    f->ok = 1;
    f->bytes = (int)pf->dwRGBBitCount / 8;
    f->rs = mshift(pf->dwRBitMask); f->rb = mbits(pf->dwRBitMask);
    f->gs = mshift(pf->dwGBitMask); f->gb = mbits(pf->dwGBitMask);
    f->bs = mshift(pf->dwBBitMask); f->bb = mbits(pf->dwBBitMask);
    if ((pf->dwFlags & DDPF_ALPHAPIXELS) && pf->dwRGBAlphaBitMask) {
        f->as = mshift(pf->dwRGBAlphaBitMask); f->ab = mbits(pf->dwRGBAlphaBitMask);
    } else {
        f->as = 0; f->ab = 0;
    }
}

/* ================================================================ state cache */
static void rs(int state, DWORD v)
{
    if (state < 0 || state > 255) return;
    if (s_rsOk[state] && s_rsVal[state] == v) return;
    s_rsOk[state] = 1;
    s_rsVal[state] = v;
    s_dev->SetRenderState((D3DRENDERSTATETYPE)state, v);
    s_st.stateChanges++;
}

static void tss(int stage, int type, DWORD v)
{
    if (s_tssOk[stage][type] && s_tssVal[stage][type] == v) return;
    s_tssOk[stage][type] = 1;
    s_tssVal[stage][type] = v;
    s_dev->SetTextureStageState((DWORD)stage, (D3DTEXTURESTAGESTATETYPE)type, v);
    s_st.stateChanges++;
}

static const DTex *tex_ptr(RTex t)
{
    if (t <= 0 || t > MAX_TEX || !s_tex[t - 1].used) return NULL;
    return &s_tex[t - 1];
}

static void set_tex(int stage, RTex t)
{
    const DTex *d = tex_ptr(t);
    if (!d) t = 0;
    if (s_curTexOk[stage] && s_curTex[stage] == t) return;
    s_curTexOk[stage] = 1;
    s_curTex[stage] = t;
    s_dev->SetTexture((DWORD)stage, d ? d->tex : NULL);
    s_st.texChanges++;
}

static void cache_invalidate(void)
{
    memset(s_rsOk, 0, sizeof(s_rsOk));
    memset(s_tssOk, 0, sizeof(s_tssOk));
    memset(s_curTexOk, 0, sizeof(s_curTexOk));
}

static void default_states(void)
{
    cache_invalidate();
    rs(D3DRENDERSTATE_ZENABLE, (DWORD)s_zmode);
    rs(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
    rs(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
    rs(D3DRENDERSTATE_CULLMODE, D3DCULL_CCW);
    rs(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    rs(D3DRENDERSTATE_DITHERENABLE, TRUE);
    rs(D3DRENDERSTATE_SPECULARENABLE, FALSE);
    rs(D3DRENDERSTATE_TEXTUREPERSPECTIVE, TRUE);
    rs(D3DRENDERSTATE_FOGENABLE, FALSE);
    rs(D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_NONE);
    rs(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
    rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
    rs(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    rs(D3DRENDERSTATE_ALPHAREF, 0x80);
    if (s_zbiasCap) rs(D3DRENDERSTATE_ZBIAS, 0);
    tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    tss(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    tss(0, D3DTSS_TEXCOORDINDEX, 0);
    tss(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
    tss(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    tss(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
    tss(0, D3DTSS_ADDRESS, D3DTADDRESS_WRAP);
    tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    tss(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    tss(1, D3DTSS_TEXCOORDINDEX, 1);
    tss(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    tss(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    set_tex(0, 0);
    set_tex(1, 0);
}

/* ================================================================ enumeration callbacks */
static HRESULT CALLBACK devcb(GUID *guid, LPSTR desc, LPSTR name, LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel, LPVOID ctx)
{
    DWORD want = *(DWORD *)ctx;
    (void)hel;
    log_printf(LOG_INFO, "caps: enum_device name=\"%s\" desc=\"%s\" hw=%d", name ? name : "", desc ? desc : "",
               (hal && hal->dcmColorModel) ? 1 : 0);
    if (hal && hal->dcmColorModel && (hal->dwDeviceRenderBitDepth & want)) {
        int isHal = !memcmp(guid, &IID_IDirect3DHALDevice, sizeof(GUID));
        if (!s_haveDev || isHal) {
            s_devGuid = *guid;
            s_hal = *hal;
            s_haveDev = 1;
            e_strlcpy(s_devName, name ? name : "?", (int)sizeof(s_devName));
        }
    }
    return D3DENUMRET_OK;
}

static HRESULT CALLBACK zcb(LPDDPIXELFORMAT pf, LPVOID ctx)
{
    (void)ctx;
    if (!(pf->dwFlags & DDPF_ZBUFFER)) return D3DENUMRET_OK;
    log_printf(LOG_INFO, "caps: zformat bits=%lu stencil=%d", (unsigned long)pf->dwZBufferBitDepth,
               (pf->dwFlags & DDPF_STENCILBUFFER) ? 1 : 0);
    /* prefer the wanted depth (16 at 16 bpp, 24/32 at 32 bpp), then anything */
    if (!s_haveZ || ((int)pf->dwZBufferBitDepth == s_zBitsWant && (int)s_zfmt.dwZBufferBitDepth != s_zBitsWant) ||
        (s_zBitsWant > 16 && pf->dwZBufferBitDepth > s_zfmt.dwZBufferBitDepth && pf->dwZBufferBitDepth <= 32)) {
        s_zfmt = *pf;
        s_haveZ = 1;
    }
    return D3DENUMRET_OK;
}

static HRESULT CALLBACK tfcb(LPDDPIXELFORMAT pf, LPVOID ctx)
{
    (void)ctx;
    if (!(pf->dwFlags & DDPF_RGB)) return D3DENUMRET_OK;
    if (pf->dwRGBBitCount == 16) {
        int alpha = (pf->dwFlags & DDPF_ALPHAPIXELS) != 0;
        if (!alpha && pf->dwGBitMask == 0x07E0 && !s_tf[TF_565].ok) pf_setup(&s_tf[TF_565], pf);
        if (!alpha && pf->dwGBitMask == 0x03E0 && !s_tf[TF_555].ok) pf_setup(&s_tf[TF_555], pf);
        if (alpha && pf->dwRGBAlphaBitMask == 0x8000 && !s_tf[TF_1555].ok) pf_setup(&s_tf[TF_1555], pf);
        if (alpha && pf->dwRGBAlphaBitMask == 0xF000 && !s_tf[TF_4444].ok) pf_setup(&s_tf[TF_4444], pf);
    } else if (pf->dwRGBBitCount == 32 && (pf->dwFlags & DDPF_ALPHAPIXELS) && pf->dwRGBAlphaBitMask == 0xFF000000u &&
               !s_tf[TF_8888].ok) {
        pf_setup(&s_tf[TF_8888], pf);
    }
    return D3DENUMRET_OK;
}

static HRESULT CALLBACK modecb(LPDDSURFACEDESC2 sd, LPVOID ctx)
{
    int w = (int)sd->dwWidth, h = (int)sd->dwHeight, b = (int)sd->ddpfPixelFormat.dwRGBBitCount;
    (void)ctx;
    if (w < 640 || h < 480 || (b != 16 && b != 32)) return DDENUMRET_OK;
    if (w * 3 != h * 4 && w * 10 != h * 16) return DDENUMRET_OK;      /* 4:3 and 16:10 */
    if (b == 32 && !(s_hal.dwDeviceRenderBitDepth & DDBD_32)) return DDENUMRET_OK;
    if (s_modeN < MAX_MODES) {
        s_modeW[s_modeN] = w; s_modeH[s_modeN] = h; s_modeB[s_modeN] = b;
        s_modeN++;
    }
    return DDENUMRET_OK;
}

/* ================================================================ caps logging */
typedef struct FlagName { DWORD f; const char *n; } FlagName;
#define FN(x, n) { x, n }
static void log_flags(const char *label, DWORD v, const FlagName *t, int n)
{
    char buf[400];
    int i, used;
    used = e_snprintf(buf, (int)sizeof(buf), "caps: %s=0x%08lX", label, (unsigned long)v);
    if (used < 0) used = 0;
    for (i = 0; i < n; i++) {
        int k;
        if (!(v & t[i].f)) continue;
        k = e_snprintf(buf + used, (int)sizeof(buf) - used, " %s", t[i].n);
        if (k < 0) break;
        used += k;
    }
    log_printf(LOG_INFO, "%s", buf);
}

static const FlagName fRast[] = { FN(D3DPRASTERCAPS_DITHER, "dither"), FN(D3DPRASTERCAPS_ZTEST, "ztest"),
    FN(D3DPRASTERCAPS_SUBPIXEL, "subpixel"), FN(D3DPRASTERCAPS_FOGVERTEX, "fogvertex"),
    FN(D3DPRASTERCAPS_FOGTABLE, "fogtable"), FN(D3DPRASTERCAPS_MIPMAPLODBIAS, "lodbias"),
    FN(D3DPRASTERCAPS_ZBIAS, "zbias"), FN(D3DPRASTERCAPS_FOGRANGE, "fogrange"),
    FN(D3DPRASTERCAPS_ANISOTROPY, "aniso"), FN(D3DPRASTERCAPS_WBUFFER, "wbuffer"),
    FN(D3DPRASTERCAPS_WFOG, "wfog"), FN(D3DPRASTERCAPS_ZBUFFERLESSHSR, "zbufferlesshsr") };
static const FlagName fTex[] = { FN(D3DPTEXTURECAPS_PERSPECTIVE, "perspective"), FN(D3DPTEXTURECAPS_POW2, "pow2"),
    FN(D3DPTEXTURECAPS_ALPHA, "alpha"), FN(D3DPTEXTURECAPS_SQUAREONLY, "squareonly"),
    FN(D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE, "repeatnotscaled"), FN(D3DPTEXTURECAPS_ALPHAPALETTE, "alphapalette"),
    FN(D3DPTEXTURECAPS_NONPOW2CONDITIONAL, "nonpow2cond") };
static const FlagName fFilt[] = { FN(D3DPTFILTERCAPS_NEAREST, "nearest"), FN(D3DPTFILTERCAPS_LINEAR, "linear"),
    FN(D3DPTFILTERCAPS_MIPNEAREST, "mipnearest"), FN(D3DPTFILTERCAPS_MIPLINEAR, "miplinear"),
    FN(D3DPTFILTERCAPS_LINEARMIPNEAREST, "linearmipnearest"), FN(D3DPTFILTERCAPS_LINEARMIPLINEAR, "trilinear") };
static const FlagName fBlend[] = { FN(D3DPBLENDCAPS_ZERO, "zero"), FN(D3DPBLENDCAPS_ONE, "one"),
    FN(D3DPBLENDCAPS_SRCCOLOR, "srccolor"), FN(D3DPBLENDCAPS_INVSRCCOLOR, "invsrccolor"),
    FN(D3DPBLENDCAPS_SRCALPHA, "srcalpha"), FN(D3DPBLENDCAPS_INVSRCALPHA, "invsrcalpha"),
    FN(D3DPBLENDCAPS_DESTALPHA, "destalpha"), FN(D3DPBLENDCAPS_INVDESTALPHA, "invdestalpha"),
    FN(D3DPBLENDCAPS_DESTCOLOR, "destcolor"), FN(D3DPBLENDCAPS_INVDESTCOLOR, "invdestcolor") };
static const FlagName fTop[] = { FN(D3DTEXOPCAPS_SELECTARG1, "selectarg1"), FN(D3DTEXOPCAPS_MODULATE, "modulate"),
    FN(D3DTEXOPCAPS_MODULATE2X, "modulate2x"), FN(D3DTEXOPCAPS_MODULATE4X, "modulate4x"), FN(D3DTEXOPCAPS_ADD, "add"),
    FN(D3DTEXOPCAPS_ADDSIGNED, "addsigned"), FN(D3DTEXOPCAPS_SUBTRACT, "subtract"),
    FN(D3DTEXOPCAPS_BLENDDIFFUSEALPHA, "blenddiffusealpha"), FN(D3DTEXOPCAPS_BLENDTEXTUREALPHA, "blendtexturealpha"),
    FN(D3DTEXOPCAPS_BLENDCURRENTALPHA, "blendcurrentalpha"), FN(D3DTEXOPCAPS_DOTPRODUCT3, "dot3") };
static const FlagName fMisc[] = { FN(D3DPMISCCAPS_CULLNONE, "cullnone"), FN(D3DPMISCCAPS_CULLCW, "cullcw"),
    FN(D3DPMISCCAPS_CULLCCW, "cullccw"), FN(D3DPMISCCAPS_MASKZ, "maskz") };

static void vidmem_kb(DWORD caps, u32 *tot, u32 *fr)
{
    DDSCAPS2 c;
    DWORD t = 0, f = 0;
    memset(&c, 0, sizeof(c));
    c.dwCaps = caps;
    if (FAILED(s_dd->GetAvailableVidMem(&c, &t, &f))) { t = 0; f = 0; }
    *tot = (u32)(t / 1024);
    *fr = (u32)(f / 1024);
}

/* ================================================================ textures */
static int bayer4(int x, int y)
{
    static const u8 m[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    return m[(y & 3) * 4 + (x & 3)];
}

static u32 pack_px(const PFmt *f, u32 c, int x, int y, int dither)
{
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF), a = (int)(c >> 24);
    u32 v;
    if (dither) {
        int d = bayer4(x, y);
        if (f->rb < 8) { r += (d << (8 - f->rb)) >> 4; if (r > 255) r = 255; }
        if (f->gb < 8) { g += (d << (8 - f->gb)) >> 4; if (g > 255) g = 255; }
        if (f->bb < 8) { b += (d << (8 - f->bb)) >> 4; if (b > 255) b = 255; }
    }
    v = ((u32)(r >> (8 - f->rb)) << f->rs) | ((u32)(g >> (8 - f->gb)) << f->gs) | ((u32)(b >> (8 - f->bb)) << f->bs);
    if (f->ab) v |= (u32)(a >> (8 - f->ab)) << f->as;
    return v;
}

static int fill_level(LPDIRECTDRAWSURFACE4 s, const PFmt *f, const u32 *src, int w, int h)
{
    DDSURFACEDESC2 sd;
    int x, y, dither = f->rb < 8;
    HRESULT hr;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    hr = s->Lock(NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
    if (FAILED(hr)) return fail("tex Lock", hr);
    for (y = 0; y < h; y++) {
        u8 *row = (u8 *)sd.lpSurface + y * sd.lPitch;
        const u32 *p = src + y * w;
        if (f->bytes == 2) {
            u16 *d = (u16 *)row;
            for (x = 0; x < w; x++) d[x] = (u16)pack_px(f, p[x], x, y, dither);
        } else {
            u32 *d = (u32 *)row;
            for (x = 0; x < w; x++) d[x] = pack_px(f, p[x], x, y, 0);
        }
    }
    s->Unlock(NULL);
    return 1;
}

static int fill_chain(LPDIRECTDRAWSURFACE4 top, const PFmt *f, const EImage *img, int base, int levels)
{
    LPDIRECTDRAWSURFACE4 s = top, nx;
    DDSCAPS2 c;
    int i;
    s->AddRef();
    for (i = 0; i < levels; i++) {
        if (!fill_level(s, f, img->pix[base + i], img_level_w(img, base + i), img_level_h(img, base + i))) {
            s->Release();
            return 0;
        }
        if (i + 1 < levels) {
            memset(&c, 0, sizeof(c));
            c.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
            if (FAILED(s->GetAttachedSurface(&c, &nx))) {
                log_printf(LOG_WARN, "warn: tex mip level %d missing", i + 1);
                break;
            }
            s->Release();
            s = nx;
        }
    }
    s->Release();
    return 1;
}

static LPDIRECTDRAWSURFACE4 create_tex_surface(int w, int h, const PFmt *f, int levels, DWORD caps, DWORD caps2,
                                               HRESULT *hrOut)
{
    DDSURFACEDESC2 sd;
    LPDIRECTDRAWSURFACE4 s = NULL;
    HRESULT hr;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | (levels > 1 ? DDSD_MIPMAPCOUNT : 0);
    sd.dwWidth = (DWORD)w;
    sd.dwHeight = (DWORD)h;
    sd.ddpfPixelFormat = f->pf;
    sd.dwMipMapCount = (DWORD)levels;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | caps | (levels > 1 ? DDSCAPS_MIPMAP | DDSCAPS_COMPLEX : 0);
    sd.ddsCaps.dwCaps2 = caps2;
    hr = s_dd->CreateSurface(&sd, &s, NULL);
    if (hrOut) *hrOut = hr;
    return FAILED(hr) ? NULL : s;
}

static void tex_release_slot(DTex *d)
{
    if (d->tex) d->tex->Release();
    if (d->surf) d->surf->Release();
    if (d->systex) d->systex->Release();
    if (d->sys) d->sys->Release();
    memset(d, 0, sizeof(*d));
}

static int pick_fmt(int imgFlags)
{
    if (imgFlags & IMG_ALPHA) {
        if (s_tf[TF_4444].ok) return TF_4444;
        if (s_tf[TF_8888].ok) return TF_8888;
        if (s_tf[TF_1555].ok) return TF_1555;
    }
    if (imgFlags & IMG_KEYED) {
        if (s_tf[TF_1555].ok) return TF_1555;
        if (s_tf[TF_4444].ok) return TF_4444;
    }
    if (s_tf[TF_565].ok) return TF_565;
    if (s_tf[TF_555].ok) return TF_555;
    if (s_tf[TF_1555].ok) return TF_1555;
    return -1;
}

static RTex d_tex_create(const EImage *imgIn, int flags)
{
    EImage tmp;
    const EImage *img = imgIn;
    int base = 0, levels, fmt, i, slot = -1, kb = 0, maxW, maxH;
    DTex *d;
    HRESULT hr;
    memset(&tmp, 0, sizeof(tmp));
    if (!s_dd || !img || !img->pix[0] || img->w <= 0 || img->h <= 0) return 0;
    maxW = s_cp.maxTexW > 0 ? s_cp.maxTexW : 256;
    maxH = s_cp.maxTexH > 0 ? s_cp.maxTexH : 256;
    if (s_ri.maxTexSize > 0) { if (maxW > s_ri.maxTexSize) maxW = s_ri.maxTexSize; if (maxH > s_ri.maxTexSize) maxH = s_ri.maxTexSize; }
    if (img->w > maxW || img->h > maxH) {
        if (img->levels <= 1) {                    /* oversize without mips: make a temporary chain */
            if (!img_alloc(&tmp, img->w, img->h, img->flags)) return 0;
            memcpy(tmp.pix[0], img->pix[0], (size_t)img->w * (size_t)img->h * 4);
            img_gen_mips(&tmp);
            img = &tmp;
        }
        while (base + 1 < img->levels && (img_level_w(img, base) > maxW || img_level_h(img, base) > maxH)) base++;
        log_printf(LOG_WARN, "warn: tex %dx%d over max %dx%d, using level %d", imgIn->w, imgIn->h, maxW, maxH, base);
    }
    levels = (flags & RT_NOMIP) ? 1 : img->levels - base;
    {                                               /* stop the chain when the smaller side reaches 1 */
        int mn = img_level_w(img, base) < img_level_h(img, base) ? img_level_w(img, base) : img_level_h(img, base), mx = 1;
        while ((1 << mx) <= mn) mx++;
        if (levels > mx) levels = mx;
    }
    if (levels < 1) levels = 1;
    fmt = pick_fmt(img->flags);
    if (fmt < 0) { img_free(&tmp); return 0; }
    for (i = 0; i < MAX_TEX; i++) if (!s_tex[i].used) { slot = i; break; }
    if (slot < 0) {
        log_printf(LOG_ERR, "err: tex table full (%d)", MAX_TEX);
        img_free(&tmp);
        return 0;
    }
    d = &s_tex[slot];
    memset(d, 0, sizeof(*d));
    d->w = img_level_w(img, base);
    d->h = img_level_h(img, base);
    d->levels = levels;
    d->fmt = fmt;
    d->flags = flags;
    for (i = 0; i < levels; i++) kb += img_level_w(img, base + i) * img_level_h(img, base + i) * s_tf[fmt].bytes;
    kb = (kb + 1023) / 1024;
    d->kb = kb;
    if (s_managed) {
        d->surf = create_tex_surface(d->w, d->h, &s_tf[fmt], levels, 0, DDSCAPS2_TEXTUREMANAGE, &hr);
        if (!d->surf && levels > 1) {               /* some drivers reject a mip chain: retry without mips */
            log_printf(LOG_WARN, "warn: tex managed %dx%d fmt=%s levels=%d hr=0x%08lX %s, retry without mips", d->w, d->h,
                       s_tfName[fmt], levels, (unsigned long)hr, hrname(hr));
            levels = d->levels = 1;
            d->kb = kb = (d->w * d->h * s_tf[fmt].bytes + 1023) / 1024;
            d->surf = create_tex_surface(d->w, d->h, &s_tf[fmt], 1, 0, DDSCAPS2_TEXTUREMANAGE, &hr);
        }
        if (!d->surf) { fail("tex CreateSurface managed", hr); goto bad; }
        if (!fill_chain(d->surf, &s_tf[fmt], img, base, levels)) goto bad;
        hr = d->surf->QueryInterface(IID_IDirect3DTexture2, (void **)&d->tex);
        if (FAILED(hr)) { fail("tex QI Texture2", hr); goto bad; }
    } else {
        d->sys = create_tex_surface(d->w, d->h, &s_tf[fmt], levels, DDSCAPS_SYSTEMMEMORY, 0, &hr);
        if (!d->sys) { fail("tex CreateSurface sysmem", hr); goto bad; }
        if (!fill_chain(d->sys, &s_tf[fmt], img, base, levels)) goto bad;
        hr = d->sys->QueryInterface(IID_IDirect3DTexture2, (void **)&d->systex);
        if (FAILED(hr)) { fail("tex QI Texture2 sys", hr); goto bad; }
        d->surf = create_tex_surface(d->w, d->h, &s_tf[fmt], levels, DDSCAPS_VIDEOMEMORY | DDSCAPS_ALLOCONLOAD, 0, &hr);
        if (!d->surf) { fail("tex CreateSurface vidmem", hr); goto bad; }
        hr = d->surf->QueryInterface(IID_IDirect3DTexture2, (void **)&d->tex);
        if (FAILED(hr)) { fail("tex QI Texture2 vid", hr); goto bad; }
        hr = d->tex->Load(d->systex);
        if (FAILED(hr)) { fail("tex Load", hr); goto bad; }
    }
    d->used = 1;
    s_texKB += kb;
    s_texCount++;
    if (s_texKB > s_texBudgetKB)
        log_printf(LOG_WARN, "warn: tex over budget total_kb=%d budget_kb=%d", s_texKB, s_texBudgetKB);
    log_printf(LOG_DEBUG, "tex: create id=%d %dx%d levels=%d fmt=%s kb=%d total_kb=%d", slot + 1, d->w, d->h, levels,
               s_tfName[fmt], kb, s_texKB);
    img_free(&tmp);
    return slot + 1;
bad:
    tex_release_slot(d);
    img_free(&tmp);
    return 0;
}

static void d_tex_free(RTex t)
{
    int i;
    DTex *d;
    if (t <= 0 || t > MAX_TEX || !s_tex[t - 1].used) return;
    d = &s_tex[t - 1];
    for (i = 0; i < 3; i++) {
        if (s_curTexOk[i] && s_curTex[i] == t) {
            if (s_dev) s_dev->SetTexture((DWORD)i, NULL);
            s_curTex[i] = 0;
        }
    }
    s_texKB -= d->kb;
    s_texCount--;
    tex_release_slot(d);
}

static int d_tex_kb(void) { return s_texKB; }
static int d_tex_budget_kb(void) { return s_texBudgetKB; }
static int d_tex_count(void) { return s_texCount; }

/* ================================================================ vertex buffers (system memory copies) */
static RVBuf d_vb_create(int fmt, const void *verts, int count)
{
    int i, stride = fmt == RFMT_V2 ? (int)sizeof(RVert2) : (int)sizeof(RVert1);
    if (!verts || count <= 0 || count > 65536 || (fmt != RFMT_V1 && fmt != RFMT_V2)) return 0;
    for (i = 0; i < MAX_VB; i++) {
        if (s_vb[i].verts) continue;
        s_vb[i].verts = (u8 *)malloc((size_t)count * (size_t)stride);
        if (!s_vb[i].verts) return 0;
        memcpy(s_vb[i].verts, verts, (size_t)count * (size_t)stride);
        s_vb[i].fmt = fmt;
        s_vb[i].count = count;
        return i + 1;
    }
    log_printf(LOG_ERR, "err: vb table full (%d)", MAX_VB);
    return 0;
}

static void d_vb_free(RVBuf vb)
{
    if (vb <= 0 || vb > MAX_VB || !s_vb[vb - 1].verts) return;
    free(s_vb[vb - 1].verts);
    memset(&s_vb[vb - 1], 0, sizeof(DVb));
}

/* ================================================================ matrices, fog, materials */
static void update_wvp(void)
{
    Mat4 wv;
    if (!s_wvpDirty) return;
    m4_mul(&wv, &s_world, &s_view);
    m4_mul(&s_wvp, &wv, &s_proj);
    s_wvpDirty = 0;
}

static void d_set_proj(const Mat4 *m)
{
    if (m) s_proj = *m; else m4_ident(&s_proj);
    s_wvpDirty = 1;
    if (s_dev) s_dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, (LPD3DMATRIX)&s_proj);   /* W range for W-buffer/fog */
}
static void d_set_view(const Mat4 *m) { if (m) s_view = *m; else m4_ident(&s_view); s_wvpDirty = 1; }
static void d_set_world(const Mat4 *m) { if (m) s_world = *m; else m4_ident(&s_world); s_wvpDirty = 1; }

static float fog_param(float d)
{
    if (s_wfog) return d;                           /* W fog: eye-space distances */
    if (d < 0.001f) d = 0.001f;                     /* Z fog: depth-buffer value of distance d */
    return s_proj.m[2][2] + s_proj.m[3][2] / d;
}

static void d_set_fog(int on, u32 rgb, float start, float end)
{
    s_fogOn = on && s_cp.fogTable;
    s_fogRGB = rgb & 0xFFFFFF;
    s_fogStart = start;
    s_fogEnd = end > start ? end : start + 1.0f;
}

static void d_set_material(const RMaterial *m)
{
    if (m) s_mat = *m;
    else { memset(&s_mat, 0, sizeof(s_mat)); s_mat.zTest = 1; s_mat.zWrite = 1; }
}

static void stage_filter(int stage, const DTex *d)
{
    DWORD mip = D3DTFP_NONE;
    if (d && d->levels > 1) mip = (s_ri.filter && s_cp.trilinear) ? D3DTFP_LINEAR : D3DTFP_POINT;
    tss(stage, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
    tss(stage, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    tss(stage, D3DTSS_MIPFILTER, mip);
    tss(stage, D3DTSS_ADDRESS, ((d && (d->flags & RT_CLAMP)) || s_mat.clampUV) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
}

static void blend_mode(int blend)
{
    switch (blend) {
    case RB_ALPHATEST:
        rs(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        rs(D3DRENDERSTATE_ALPHATESTENABLE, TRUE);
        break;
    case RB_BLEND:
        rs(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        break;
    case RB_ADD:
        rs(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE);
        rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_ONE);
        rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        break;
    case RB_SHADOW:
        rs(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ZERO);
        rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        break;
    case RB_PREMUL:                                 /* jet T02: premultiplied "over" (HUD, DESIGN 10.1) */
        rs(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE);
        rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
        rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        break;
    default:
        rs(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        break;
    }
}

/* pass 0 = base pass (with stage 1 if the device validated it), pass 1 = stage-1 fallback pass */
static void apply_material(int pass, int fmt)
{
    const RMaterial *m = &s_mat;
    const DTex *t0 = tex_ptr(m->tex0), *t1 = tex_ptr(m->tex1);
    int s1 = (m->stage1 != RS1_NONE && t1) ? m->stage1 : RS1_NONE;
    int single = (s1 == RS1_DETAIL && s_mt2x) || (s1 == RS1_ENVADD && s_mtAdd);
    int fog = s_fogOn && m->fog;
    rs(D3DRENDERSTATE_ZENABLE, m->zTest ? (DWORD)s_zmode : (DWORD)D3DZB_FALSE);
    rs(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
    rs(D3DRENDERSTATE_CULLMODE, m->cullNone ? D3DCULL_NONE : D3DCULL_CCW);
    if (s_zbiasCap) rs(D3DRENDERSTATE_ZBIAS, m->zBias);
    rs(D3DRENDERSTATE_FOGENABLE, fog ? TRUE : FALSE);
    if (fog) {
        rs(D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_LINEAR);
        rs(D3DRENDERSTATE_FOGTABLESTART, fbits(fog_param(s_fogStart)));
        rs(D3DRENDERSTATE_FOGTABLEEND, fbits(fog_param(s_fogEnd)));
    }
    s_pass2 = (s1 != RS1_NONE && !single && s_twoPassOk);
    if (pass == 0) {
        rs(D3DRENDERSTATE_ZWRITEENABLE, m->zWrite ? TRUE : FALSE);
        blend_mode(m->blend);
        if (fog) rs(D3DRENDERSTATE_FOGCOLOR, s_fogRGB);
        set_tex(0, t0 ? m->tex0 : 0);
        tss(0, D3DTSS_COLOROP, t0 ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        tss(0, D3DTSS_ALPHAOP, t0 ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        tss(0, D3DTSS_TEXCOORDINDEX, 0);
        stage_filter(0, t0);
        if (s_lodBiasCap) tss(0, D3DTSS_MIPMAPLODBIAS, fbits(t0 && t0->levels > 1 ? 0.25f * (float)m->lodBias : 0.0f));   /* T16 */
        if (single) {
            set_tex(1, m->tex1);
            tss(1, D3DTSS_COLOROP, s1 == RS1_DETAIL ? D3DTOP_MODULATE2X : D3DTOP_ADD);
            tss(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            tss(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
            tss(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            tss(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
            tss(1, D3DTSS_TEXCOORDINDEX, fmt == RFMT_V2 ? 1 : 0);
            stage_filter(1, t1);
        } else {
            tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            tss(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
    } else {
        /* second pass: tex1 alone, blended onto the first pass; fog colour neutral for the blend */
        rs(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
        rs(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        rs(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        if (s1 == RS1_DETAIL) {
            rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_DESTCOLOR);
            rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_SRCCOLOR);
            if (fog) rs(D3DRENDERSTATE_FOGCOLOR, 0x808080);
        } else {
            rs(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE);
            rs(D3DRENDERSTATE_DESTBLEND, D3DBLEND_ONE);
            if (fog) rs(D3DRENDERSTATE_FOGCOLOR, 0x000000);
        }
        set_tex(0, m->tex1);
        tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        tss(0, D3DTSS_TEXCOORDINDEX, fmt == RFMT_V2 ? 1 : 0);
        stage_filter(0, t1);
        if (s_lodBiasCap) tss(0, D3DTSS_MIPMAPLODBIAS, fbits(0.0f));   /* T16 */
        tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        tss(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

/* ================================================================ transform, clip, submit */
static void submit(int fmt, int nv, int ni)
{
    DWORD fvf = fmt == RFMT_V2 ? FVF_TLV2 : FVF_TLV1;
    if (ni <= 0) return;
    prof_begin(s_secSubmit);
    apply_material(0, fmt);
    s_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf, s_tlv, (DWORD)nv, s_oidx, (DWORD)ni, DIP_FLAGS);
    s_st.draws++;
    s_st.tris += ni / 3;
    if (s_pass2) {
        apply_material(1, fmt);
        s_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf, s_tlv, (DWORD)nv, s_oidx, (DWORD)ni, DIP_FLAGS);
        s_st.draws++;
        s_st.tris += ni / 3;
    }
    prof_end(s_secSubmit);
}

/* Clip planes in clip space. The x/y planes sit at the device guard band (k = 1: viewport edges; the Voodoo3 has no
   guard band), so on devices with one only triangles crossing the near plane or leaving the guard band are clipped.
   The far plane only rejects (geometry beyond far is fully fogged; its depth is clamped to 1). */
static float s_kL = 1.0f, s_kR = 1.0f, s_kB = 1.0f, s_kT = 1.0f;
static float s_gbL, s_gbT, s_gbR, s_gbB;            /* device guard band in screen pixels (0 = none) */

static float plane_d(const CV *v, int bit)
{
    switch (bit) {
    case 1: return v->z;                            /* near: z >= 0 */
    case 4: return v->x + s_kL * v->w;              /* left */
    case 8: return s_kR * v->w - v->x;              /* right */
    case 16: return v->y + s_kB * v->w;             /* bottom */
    default: return s_kT * v->w - v->y;             /* top */
    }
}

/* clip triangle pool[0..2] against the planes in codes (far bit excluded); out[] = pool indices, returns count */
static int clip_tri(CV *pool, int codes, int two, int *out)
{
    int bufA[16], bufB[16], *src = bufA, *dst = bufB, n = 3, np = 3, bit, i, m;
    float d[16];
    src[0] = 0; src[1] = 1; src[2] = 2;
    for (bit = 1; bit <= 32; bit <<= 1) {
        if (!(codes & bit)) continue;
        for (i = 0; i < n; i++) d[i] = plane_d(&pool[src[i]], bit);
        m = 0;
        for (i = 0; i < n; i++) {
            int j = i + 1 == n ? 0 : i + 1;
            if (d[i] >= 0.0f) dst[m++] = src[i];
            if ((d[i] >= 0.0f) != (d[j] >= 0.0f) && np < 24 && m < 15) {
                const CV *p = &pool[src[i]], *q = &pool[src[j]];
                CV *o = &pool[np];
                float t = d[i] / (d[i] - d[j]);
                o->x = p->x + (q->x - p->x) * t; o->y = p->y + (q->y - p->y) * t;
                o->z = p->z + (q->z - p->z) * t; o->w = p->w + (q->w - p->w) * t;
                o->r = p->r + (q->r - p->r) * t; o->g = p->g + (q->g - p->g) * t;
                o->b = p->b + (q->b - p->b) * t; o->a = p->a + (q->a - p->a) * t;
                o->u = p->u + (q->u - p->u) * t; o->v = p->v + (q->v - p->v) * t;
                if (two) { o->u2 = p->u2 + (q->u2 - p->u2) * t; o->v2 = p->v2 + (q->v2 - p->v2) * t; }
                dst[m++] = np++;
            }
        }
        n = m;
        if (n < 3) return 0;
        { int *sw = src; src = dst; dst = sw; }
    }
    for (i = 0; i < n; i++) out[i] = src[i];
    return n;
}

static int clampi255(float f) { int i = e_ftoi(f); return i < 0 ? 0 : (i > 255 ? 255 : i); }

/* transform vertices [first, first+count) of an RVert1/RVert2 array, cull/clip the index list, submit */
static void draw_tris(int fmt, const u8 *verts, int first, int count, const u16 *idx, int nIdx)
{
    const int two = fmt == RFMT_V2;
    const int stride = two ? (int)sizeof(RVert2) : (int)sizeof(RVert1);
    const int tls = two ? (int)sizeof(TLV2) : (int)sizeof(TLV1);
    float hw = (float)s_vw * 0.5f, hh = (float)s_vh * 0.5f, cxo = (float)s_vx + hw, cyo = (float)s_vy + hh;
    float xmin = (float)s_vx, xmax = (float)(s_vx + s_vw), ymin = (float)s_vy, ymax = (float)(s_vy + s_vh);
    float m00, m01, m02, m03, m10, m11, m12, m13, m20, m21, m22, m23, m30, m31, m32, m33, kL, kR, kB, kT;
    int i, k, nv, ni, nclip = 0;
    if (count <= 0 || nIdx < 3) return;
    if (count > MAXV) count = MAXV;
    prof_begin(s_secXform);
    update_wvp();
    if (s_gbR > s_gbL && s_gbB > s_gbT) {           /* guard band: widen the x/y clip planes, clamp to it */
        if (s_gbL < xmin) xmin = s_gbL;
        if (s_gbR > xmax) xmax = s_gbR;
        if (s_gbT < ymin) ymin = s_gbT;
        if (s_gbB > ymax) ymax = s_gbB;
    }
    s_kL = kL = (cxo - xmin) / hw; s_kR = kR = (xmax - cxo) / hw;
    s_kT = kT = (cyo - ymin) / hh; s_kB = kB = (ymax - cyo) / hh;
    m00 = s_wvp.m[0][0]; m01 = s_wvp.m[0][1]; m02 = s_wvp.m[0][2]; m03 = s_wvp.m[0][3];
    m10 = s_wvp.m[1][0]; m11 = s_wvp.m[1][1]; m12 = s_wvp.m[1][2]; m13 = s_wvp.m[1][3];
    m20 = s_wvp.m[2][0]; m21 = s_wvp.m[2][1]; m22 = s_wvp.m[2][2]; m23 = s_wvp.m[2][3];
    m30 = s_wvp.m[3][0]; m31 = s_wvp.m[3][1]; m32 = s_wvp.m[3][2]; m33 = s_wvp.m[3][3];
    for (i = 0; i < count; i++) {
        const float *p = (const float *)(verts + (size_t)(first + i) * (size_t)stride);
        float x = p[0], y = p[1], z = p[2];
        float cx = x * m00 + y * m10 + z * m20 + m30;
        float cy = x * m01 + y * m11 + z * m21 + m31;
        float cz = x * m02 + y * m12 + z * m22 + m32;
        float cw = x * m03 + y * m13 + z * m23 + m33;
        float *c = s_clipv + i * 4;
        TLV1 *t = (TLV1 *)(s_tlv + i * tls);
        int code = 0;
        c[0] = cx; c[1] = cy; c[2] = cz; c[3] = cw;
        if (cz < 0.0f) code |= 1;
        if (cz > cw) code |= 2;
        if (cx < -kL * cw) code |= 4;
        if (cx > kR * cw) code |= 8;
        if (cy < -kB * cw) code |= 16;
        if (cy > kT * cw) code |= 32;
        s_code[i] = (u8)code;
        if (!(code & ~2)) {
            float rhw = 1.0f / cw;
            t->sx = cxo + cx * rhw * hw;
            t->sy = cyo - cy * rhw * hh;
            t->sz = code ? 1.0f : cz * rhw;
            t->rhw = rhw;
        } else {
            t->sx = xmin; t->sy = ymin; t->sz = 0.0f; t->rhw = 1.0f;
        }
        t->color = *(const DWORD *)(p + 3);
        t->u = p[4]; t->v = p[5];
        if (two) { ((TLV2 *)t)->u2 = p[6]; ((TLV2 *)t)->v2 = p[7]; }
    }
    s_st.verts += count;
    nv = count;
    ni = 0;
    for (k = 0; k + 2 < nIdx; k += 3) {
        int a = (int)idx[k] - first, b = (int)idx[k + 1] - first, c = (int)idx[k + 2] - first, ca, cb, cc, codes;
        if (a < 0 || b < 0 || c < 0 || a >= count || b >= count || c >= count) continue;
        ca = s_code[a]; cb = s_code[b]; cc = s_code[c];
        if (ca & cb & cc) continue;                 /* all outside one plane */
        if (ni + 48 > MAXI || nv + 16 > MAXV) {     /* scratch full: draw what we have, keep the transformed range */
            prof_end(s_secXform);
            submit(fmt, nv, ni);
            prof_begin(s_secXform);
            ni = 0;
            nv = count;
        }
        codes = (ca | cb | cc) & ~2;
        if (!codes) {
            s_oidx[ni++] = (u16)a; s_oidx[ni++] = (u16)b; s_oidx[ni++] = (u16)c;
            continue;
        }
        {
            CV pool[24];
            int vi[3], out[16], tl[16], j, n;
            vi[0] = a; vi[1] = b; vi[2] = c;
            for (j = 0; j < 3; j++) {
                const float *cl = s_clipv + vi[j] * 4;
                const float *p = (const float *)(verts + (size_t)(first + vi[j]) * (size_t)stride);
                DWORD col = *(const DWORD *)(p + 3);
                CV *o = &pool[j];
                o->x = cl[0]; o->y = cl[1]; o->z = cl[2]; o->w = cl[3];
                o->a = (float)(col >> 24); o->r = (float)((col >> 16) & 0xFF);
                o->g = (float)((col >> 8) & 0xFF); o->b = (float)(col & 0xFF);
                o->u = p[4]; o->v = p[5];
                if (two) { o->u2 = p[6]; o->v2 = p[7]; }
            }
            n = clip_tri(pool, codes, two, out);
            if (n < 3) continue;
            for (j = 0; j < n; j++) {
                const CV *q;
                TLV1 *t;
                float rhw, sx, sy, sz;
                if (out[j] < 3) { tl[j] = vi[out[j]]; continue; }   /* surviving corner: already projected */
                q = &pool[out[j]];
                t = (TLV1 *)(s_tlv + nv * tls);
                rhw = q->w > 1e-6f ? 1.0f / q->w : 1e6f;
                sx = cxo + q->x * rhw * hw;
                sy = cyo - q->y * rhw * hh;
                sz = q->z * rhw;
                t->sx = sx < xmin ? xmin : (sx > xmax ? xmax : sx);
                t->sy = sy < ymin ? ymin : (sy > ymax ? ymax : sy);
                t->sz = sz < 0.0f ? 0.0f : (sz > 1.0f ? 1.0f : sz);
                t->rhw = rhw;
                t->color = ((DWORD)clampi255(q->a) << 24) | ((DWORD)clampi255(q->r) << 16) |
                           ((DWORD)clampi255(q->g) << 8) | (DWORD)clampi255(q->b);
                t->u = q->u; t->v = q->v;
                if (two) { ((TLV2 *)t)->u2 = q->u2; ((TLV2 *)t)->v2 = q->v2; }
                tl[j] = nv++;
            }
            for (j = 1; j + 1 < n; j++) {
                s_oidx[ni++] = (u16)tl[0]; s_oidx[ni++] = (u16)tl[j]; s_oidx[ni++] = (u16)tl[j + 1];
            }
            nclip++;
        }
    }
    prof_end(s_secXform);
    prof_count(s_cntClip, nclip);
    submit(fmt, nv, ni);
}

static void d_draw_vb(RVBuf vb, const u16 *idx, int nIdx)
{
    const DVb *b;
    int i, lo = 65535, hi = -1;
    if (vb <= 0 || vb > MAX_VB || !s_vb[vb - 1].verts || !idx || nIdx < 3 || !s_inScene) return;
    b = &s_vb[vb - 1];
    for (i = 0; i < nIdx; i++) {
        int v = idx[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (hi >= b->count) hi = b->count - 1;
    if (hi < lo) return;
    draw_tris(b->fmt, b->verts, lo, hi - lo + 1, idx, nIdx);
}

static void d_draw_mem(int fmt, const void *verts, int nVerts, const u16 *idx, int nIdx)
{
    if (!verts || nVerts <= 0 || !idx || nIdx < 3 || !s_inScene || (fmt != RFMT_V1 && fmt != RFMT_V2)) return;
    s_st.dynVerts += nVerts;
    draw_tris(fmt, (const u8 *)verts, 0, nVerts, idx, nIdx);
}

static void d_draw_2d(const RVert2D *v, int nVerts, const u16 *idx, int nIdx, RTex tex, int blend)
{
    const DTex *t = tex_ptr(tex);
    float x1 = (float)s_W, y1 = (float)s_H;
    int i;
    TLV1 *o = (TLV1 *)s_tlv;
    if (!v || nVerts <= 0 || nVerts > MAXV || !idx || nIdx < 3 || !s_inScene) return;
    for (i = 0; i < nVerts; i++) {
        float x = v[i].x - 0.5f, y = v[i].y - 0.5f;   /* D3D pixel centres are at integer coordinates */
        o[i].sx = x < -0.5f ? -0.5f : (x > x1 ? x1 : x);
        o[i].sy = y < -0.5f ? -0.5f : (y > y1 ? y1 : y);
        o[i].sz = 0.0f;
        o[i].rhw = 1.0f;
        o[i].color = v[i].color;
        o[i].u = v[i].u;
        o[i].v = v[i].v;
    }
    rs(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    rs(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    rs(D3DRENDERSTATE_FOGENABLE, FALSE);
    rs(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    if (s_zbiasCap) rs(D3DRENDERSTATE_ZBIAS, 0);
    blend_mode(blend);
    set_tex(0, t ? tex : 0);
    tss(0, D3DTSS_COLOROP, t ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    tss(0, D3DTSS_ALPHAOP, t ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    tss(0, D3DTSS_TEXCOORDINDEX, 0);
    tss(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
    tss(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    tss(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
    tss(0, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP);
    tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    tss(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    s_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, FVF_TLV1, s_tlv, (DWORD)nVerts, (LPWORD)idx, (DWORD)nIdx, DIP_FLAGS);
    s_st.draws++;
    s_st.tris += nIdx / 3;
    s_st.verts += nVerts;
}

/* ================================================================ init helpers */
static void validate_probe(void)
{
    static const char *const names[4] = { "base", "mod2x", "add", "lightmap" };
    EImage im;
    RTex a, b;
    int i;
    s_mt2x = s_mtAdd = 0;
    if (!img_alloc(&im, 64, 64, 0)) return;
    for (i = 0; i < 64 * 64; i++) im.pix[0][i] = 0xFF808080u ^ (u32)(i * 2654435761u >> 24);
    a = d_tex_create(&im, RT_NOMIP);
    b = d_tex_create(&im, RT_NOMIP);
    img_free(&im);
    if (!a || !b) { d_tex_free(a); d_tex_free(b); return; }
    for (i = 0; i < 4; i++) {
        DWORD passes = 0;
        HRESULT hr;
        default_states();
        set_tex(0, a);
        set_tex(1, b);
        if (i == 3) {
            tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            tss(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
            tss(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            tss(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
            tss(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            tss(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
            tss(2, D3DTSS_COLOROP, D3DTOP_MODULATE);
            tss(2, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            tss(2, D3DTSS_COLORARG2, D3DTA_CURRENT);
            tss(2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            tss(2, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        } else if (i > 0) {
            tss(1, D3DTSS_COLOROP, i == 1 ? D3DTOP_MODULATE2X : D3DTOP_ADD);
            tss(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            tss(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
            tss(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            tss(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        } else {
            set_tex(1, 0);
        }
        hr = s_dev->ValidateDevice(&passes);
        log_printf(LOG_INFO, "caps: validate layout=%s hr=0x%08lX %s passes=%lu", names[i], (unsigned long)hr, hrname(hr),
                   (unsigned long)passes);
        if (i == 1) s_mt2x = SUCCEEDED(hr) && passes == 1;
        if (i == 2) s_mtAdd = SUCCEEDED(hr) && passes == 1;
    }
    tss(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    tss(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    s_dev->SetTexture(2, NULL);
    d_tex_free(a);
    d_tex_free(b);
    default_states();
}

static void measure_refresh(void)
{
    s64 t0, t1;
    int i;
    float hz = 0.0f;
    s_vbOk = 0;
    if (SUCCEEDED(s_dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, NULL))) {
        t0 = qpc();
        for (i = 0; i < 30; i++) s_dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, NULL);
        t1 = qpc();
        if (t1 > t0) {
            hz = (float)((s64)30 * s_qpf * 1000 / (t1 - t0)) * 0.001f;
            if (hz >= 40.0f && hz <= 250.0f) {
                s_vbOk = 1;
                s_periodFx = (t1 - t0) * 1024 / 30;
            }
        }
    }
    if (!s_vbOk) {
        HDC dc = GetDC(NULL);
        int vr = dc ? GetDeviceCaps(dc, VREFRESH) : 0;
        if (dc) ReleaseDC(NULL, dc);
        log_printf(LOG_WARN, "warn: rend vblank wait unusable measured_hz=%.2f vrefresh=%d (QPC pacing)", (double)hz, vr);
        hz = (vr >= 50 && vr <= 200) ? (float)vr : 60.0f;
        s_periodFx = s_qpf * 1024 / (s64)e_ftoi(hz);
    }
    s_cp.refreshHz = hz;
    s_div = 1;
    if (s_ri.fpsTarget > 0) {
        s_div = e_ftoi(hz / (float)s_ri.fpsTarget);
        if (s_div < 1) s_div = 1;
    }
    s_cp.vblankDivisor = s_div;
    s_cpuPeriod = s_ri.fpsTarget > 0 ? s_qpf / s_ri.fpsTarget : s_qpf / 60;
    log_printf(LOG_INFO, "caps: refresh hz=%.2f vblank=%d divisor=%d fpsTarget=%d -> fps=%.2f nocap=%d", (double)hz, s_vbOk,
               s_div, s_ri.fpsTarget, (double)(hz / (float)s_div), s_ri.noCap);
}

static void log_caps(void)
{
    const D3DPRIMCAPS *tc = &s_hal.dpcTriCaps;
    DDDEVICEIDENTIFIER id;
    DDCAPS hw, hel;
    u32 tot, fr;
    int i;
    memset(&id, 0, sizeof(id));
    if (SUCCEEDED(s_dd->GetDeviceIdentifier(&id, 0))) {
        log_printf(LOG_INFO, "caps: driver=%s desc=\"%s\" version=%u.%u.%u.%u vendor=0x%04lX device=0x%04lX", id.szDriver,
                   id.szDescription, HIWORD(id.liDriverVersion.HighPart), LOWORD(id.liDriverVersion.HighPart),
                   HIWORD(id.liDriverVersion.LowPart), LOWORD(id.liDriverVersion.LowPart), (unsigned long)id.dwVendorId,
                   (unsigned long)id.dwDeviceId);
    }
    memset(&hw, 0, sizeof(hw)); hw.dwSize = sizeof(hw);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
    if (SUCCEEDED(s_dd->GetCaps(&hw, &hel))) {
        log_printf(LOG_INFO, "caps: ddraw caps=0x%08lX caps2=0x%08lX vidmem_total_kb=%lu vidmem_free_kb=%lu flipnovsync=%d "
                   "canrenderwindowed=%d", (unsigned long)hw.dwCaps, (unsigned long)hw.dwCaps2,
                   (unsigned long)(hw.dwVidMemTotal / 1024), (unsigned long)(hw.dwVidMemFree / 1024),
                   (hw.dwCaps2 & DDCAPS2_FLIPNOVSYNC) != 0, (hw.dwCaps2 & DDCAPS2_CANRENDERWINDOWED) != 0);
    }
    log_printf(LOG_INFO, "caps: device name=\"%s\" hal=%d devcaps=0x%08lX transformcaps=0x%08lX clipping=%d "
               "renderdepth=0x%lX zdepth=0x%lX maxvertexcount=%lu", s_devName, s_cp.hal, (unsigned long)s_hal.dwDevCaps,
               (unsigned long)s_hal.dtcTransformCaps.dwCaps, (int)s_hal.bClipping,
               (unsigned long)s_hal.dwDeviceRenderBitDepth, (unsigned long)s_hal.dwDeviceZBufferBitDepth,
               (unsigned long)s_hal.dwMaxVertexCount);
    log_flags("raster", tc->dwRasterCaps, fRast, E_COUNT(fRast));
    log_flags("misc", tc->dwMiscCaps, fMisc, E_COUNT(fMisc));
    log_flags("texture", tc->dwTextureCaps, fTex, E_COUNT(fTex));
    log_flags("filter", tc->dwTextureFilterCaps, fFilt, E_COUNT(fFilt));
    log_flags("srcblend", tc->dwSrcBlendCaps, fBlend, E_COUNT(fBlend));
    log_flags("destblend", tc->dwDestBlendCaps, fBlend, E_COUNT(fBlend));
    log_flags("texop", s_hal.dwTextureOpCaps, fTop, E_COUNT(fTop));
    log_printf(LOG_INFO, "caps: alphacmp=0x%02lX zcmp=0x%02lX shade=0x%08lX texaddress=0x%02lX",
               (unsigned long)tc->dwAlphaCmpCaps, (unsigned long)tc->dwZCmpCaps, (unsigned long)tc->dwShadeCaps,
               (unsigned long)tc->dwTextureAddressCaps);
    log_printf(LOG_INFO, "caps: tex min=%lux%lu max=%lux%lu aspect=%lu repeat=%lu aniso=%lu stages=%u simultaneous=%u",
               (unsigned long)s_hal.dwMinTextureWidth, (unsigned long)s_hal.dwMinTextureHeight,
               (unsigned long)s_hal.dwMaxTextureWidth, (unsigned long)s_hal.dwMaxTextureHeight,
               (unsigned long)s_hal.dwMaxTextureAspectRatio, (unsigned long)s_hal.dwMaxTextureRepeat,
               (unsigned long)s_hal.dwMaxAnisotropy, (unsigned)s_hal.wMaxTextureBlendStages,
               (unsigned)s_hal.wMaxSimultaneousTextures);
    log_printf(LOG_INFO, "caps: guardband l=%.0f t=%.0f r=%.0f b=%.0f stencil=0x%lX fvf=0x%lX",
               (double)s_hal.dvGuardBandLeft, (double)s_hal.dvGuardBandTop, (double)s_hal.dvGuardBandRight,
               (double)s_hal.dvGuardBandBottom, (unsigned long)s_hal.dwStencilCaps, (unsigned long)s_hal.dwFVFCaps);
    log_printf(LOG_INFO, "caps: formats 565=%d 555=%d 1555=%d 4444=%d 8888=%d zbits=%d managed=%d", s_cp.fmt565, s_cp.fmt555,
               s_cp.fmt1555, s_cp.fmt4444, s_cp.fmt8888, s_cp.zBits, s_cp.managedTex);
    vidmem_kb(DDSCAPS_VIDEOMEMORY, &tot, &fr);
    log_printf(LOG_INFO, "caps: vidmem video total_kb=%u free_kb=%u", tot, fr);
    vidmem_kb(DDSCAPS_TEXTURE, &tot, &fr);
    log_printf(LOG_INFO, "caps: vidmem texture total_kb=%u free_kb=%u", tot, fr);
    {
        char buf[600];
        int used = 0, k;
        buf[0] = 0;
        for (i = 0; i < s_modeN; i++) {
            k = e_snprintf(buf + used, (int)sizeof(buf) - used, " %dx%dx%d", s_modeW[i], s_modeH[i], s_modeB[i]);
            if (k < 0) break;
            used += k;
        }
        log_printf(LOG_INFO, "caps: modes n=%d%s", s_modeN, buf);
    }
}

static void d_shutdown(void);

static int init_inner(void *hwnd, const RInit *ri, RCaps *caps)
{
    DDSURFACEDESC2 sd;
    DDSCAPS2 c;
    D3DVIEWPORT2 vd;
    DWORD want, coop;
    HRESULT hr;
    LARGE_INTEGER f;
    u32 tot, fr;
    int i;
    d_shutdown();
    memset(&s_cp, 0, sizeof(s_cp));
    memset(s_tf, 0, sizeof(s_tf));
    memset(&s_pace, 0, sizeof(s_pace));
    memset(&s_st, 0, sizeof(s_st));
    s_ri = *ri;
    s_hwnd = (HWND)hwnd;
    s_W = ri->width > 0 ? ri->width : 640;
    s_H = ri->height > 0 ? ri->height : 480;
    s_bpp = ri->bpp == 32 ? 32 : 16;
    s_fs = ri->fullscreen;
    s_vx = s_vy = 0; s_vw = s_W; s_vh = s_H;
    s_lost = s_inScene = 0;
    s_lastPresent = 0;
    QueryPerformanceFrequency(&f);
    s_qpf = f.QuadPart;
    if (!s_hwnd) { log_printf(LOG_ERR, "err: rend no window"); return 0; }

    s_tlv = (u8 *)malloc((size_t)MAXV * sizeof(TLV2));
    s_clipv = (float *)malloc((size_t)MAXV * 4 * sizeof(float));
    s_code = (u8 *)malloc((size_t)MAXV);
    s_oidx = (u16 *)malloc((size_t)MAXI * sizeof(u16));
    if (!s_tlv || !s_clipv || !s_code || !s_oidx) { log_printf(LOG_ERR, "err: rend scratch alloc failed"); return 0; }
    memset(s_tlv, 0, (size_t)MAXV * sizeof(TLV2));

    CHK(DirectDrawCreate(NULL, &s_dd1, NULL), "DirectDrawCreate");
    CHK(s_dd1->QueryInterface(IID_IDirectDraw4, (void **)&s_dd), "QI IDirectDraw4 (DirectX 6 needed)");
    CHK(s_dd->QueryInterface(IID_IDirect3D3, (void **)&s_d3d), "QI IDirect3D3");
    s_haveDev = 0;
    want = s_bpp == 32 ? DDBD_32 : DDBD_16;
    CHK(s_d3d->EnumDevices(devcb, &want), "EnumDevices");
    if (!s_haveDev) {
        log_printf(LOG_ERR, "err: rend no 3D accelerator (no hardware D3D device for %d bpp)", s_bpp);
        return 0;
    }
    s_cp.hal = !memcmp(&s_devGuid, &IID_IDirect3DHALDevice, sizeof(GUID));

    /* cooperative level + surfaces (spike order: coop -> mode -> primary chain -> Z -> attach -> device) */
    coop = DDSCL_FPUSETUP | (s_fs ? (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) : DDSCL_NORMAL);
    CHK(s_dd->SetCooperativeLevel(s_hwnd, coop), "SetCooperativeLevel");
    if (s_fs) {
        CHK(s_dd->SetDisplayMode((DWORD)s_W, (DWORD)s_H, (DWORD)s_bpp, 0, 0), "SetDisplayMode");
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
        sd.dwBackBufferCount = 1;
        CHK(s_dd->CreateSurface(&sd, &s_prim, NULL), "CreateSurface primary flip chain");
        memset(&c, 0, sizeof(c));
        c.dwCaps = DDSCAPS_BACKBUFFER;
        CHK(s_prim->GetAttachedSurface(&c, &s_back), "GetAttachedSurface back");
    } else {
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        CHK(s_dd->GetDisplayMode(&sd), "GetDisplayMode");
        if ((int)sd.ddpfPixelFormat.dwRGBBitCount != s_bpp) {
            log_printf(LOG_ERR, "err: rend windowed mode needs a %d-bit desktop (desktop is %lu-bit)", s_bpp,
                       (unsigned long)sd.ddpfPixelFormat.dwRGBBitCount);
            return 0;
        }
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        CHK(s_dd->CreateSurface(&sd, &s_prim, NULL), "CreateSurface primary");
        CHK(s_dd->CreateClipper(0, &s_clip, NULL), "CreateClipper");
        s_clip->SetHWnd(0, s_hwnd);
        s_prim->SetClipper(s_clip);
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.dwWidth = (DWORD)s_W;
        sd.dwHeight = (DWORD)s_H;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        CHK(s_dd->CreateSurface(&sd, &s_back, NULL), "CreateSurface back (windowed)");
    }
    s_haveZ = 0;
    s_zBitsWant = s_bpp == 32 ? 24 : 16;
    CHK(s_d3d->EnumZBufferFormats(s_devGuid, zcb, NULL), "EnumZBufferFormats");
    if (!s_haveZ) { log_printf(LOG_ERR, "err: rend no Z-buffer format"); return 0; }
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.dwWidth = (DWORD)s_W;
    sd.dwHeight = (DWORD)s_H;
    sd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    sd.ddpfPixelFormat = s_zfmt;
    CHK(s_dd->CreateSurface(&sd, &s_zbuf, NULL), "CreateSurface Z");
    CHK(s_back->AddAttachedSurface(s_zbuf), "AddAttachedSurface Z");
    CHK(s_d3d->CreateDevice(s_devGuid, s_back, &s_dev, NULL), "CreateDevice");
    CHK(s_d3d->CreateViewport(&s_vp, NULL), "CreateViewport");
    CHK(s_dev->AddViewport(s_vp), "AddViewport");
    memset(&vd, 0, sizeof(vd));
    vd.dwSize = sizeof(vd);
    vd.dwX = 0; vd.dwY = 0; vd.dwWidth = (DWORD)s_W; vd.dwHeight = (DWORD)s_H;
    vd.dvClipX = -1.0f; vd.dvClipY = 1.0f; vd.dvClipWidth = 2.0f; vd.dvClipHeight = 2.0f;
    vd.dvMinZ = 0.0f; vd.dvMaxZ = 1.0f;
    CHK(s_vp->SetViewport2(&vd), "SetViewport2");
    CHK(s_dev->SetCurrentViewport(s_vp), "SetCurrentViewport");
    CHK(s_dev->EnumTextureFormats(tfcb, NULL), "EnumTextureFormats");
    if (!s_tf[TF_565].ok && !s_tf[TF_555].ok) { log_printf(LOG_ERR, "err: rend no 16-bit RGB texture format"); return 0; }

    /* caps */
    e_strlcpy(s_cp.device, s_devName, (int)sizeof(s_cp.device));
    s_cp.maxTexW = s_hal.dwMaxTextureWidth ? (int)s_hal.dwMaxTextureWidth : 256;
    s_cp.maxTexH = s_hal.dwMaxTextureHeight ? (int)s_hal.dwMaxTextureHeight : 256;
    s_cp.maxAspect = s_hal.dwMaxTextureAspectRatio ? (int)s_hal.dwMaxTextureAspectRatio : 8;
    s_cp.texStages = s_hal.wMaxSimultaneousTextures ? (int)s_hal.wMaxSimultaneousTextures : 1;
    s_cp.fogTable = (s_hal.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_FOGTABLE) != 0;
    s_cp.fogVertex = (s_hal.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_FOGVERTEX) != 0;
    s_cp.wbuffer = (s_hal.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_WBUFFER) != 0;
    s_wfog = (s_hal.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_WFOG) != 0;
    s_zbiasCap = (s_hal.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_ZBIAS) != 0;
    s_oldPace = arg_has("-oldpace");                /* T16 */
    s_pace1 = arg_has("-pace1");
    s_lodBiasCap = (s_hal.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_MIPMAPLODBIAS) != 0 && !arg_has("-nolodbias");   /* T16 */
    s_cp.zBits = (int)s_zfmt.dwZBufferBitDepth;
    s_cp.alphaTest = (s_hal.dpcTriCaps.dwAlphaCmpCaps & D3DPCMPCAPS_GREATEREQUAL) != 0;
    s_cp.mipmap = (s_hal.dpcTriCaps.dwTextureFilterCaps & (D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_MIPLINEAR |
                   D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR)) != 0;
    s_cp.trilinear = (s_hal.dpcTriCaps.dwTextureFilterCaps & D3DPTFILTERCAPS_LINEARMIPLINEAR) != 0;
    s_cp.fmt565 = s_tf[TF_565].ok; s_cp.fmt555 = s_tf[TF_555].ok; s_cp.fmt1555 = s_tf[TF_1555].ok;
    s_cp.fmt4444 = s_tf[TF_4444].ok; s_cp.fmt8888 = s_tf[TF_8888].ok;
    s_twoPassOk = (s_hal.dpcTriCaps.dwSrcBlendCaps & D3DPBLENDCAPS_DESTCOLOR) &&
                  (s_hal.dpcTriCaps.dwDestBlendCaps & D3DPBLENDCAPS_SRCCOLOR) &&
                  (s_hal.dpcTriCaps.dwSrcBlendCaps & D3DPBLENDCAPS_ONE) && (s_hal.dpcTriCaps.dwDestBlendCaps & D3DPBLENDCAPS_ONE);
    s_zmode = (ri->wantWBuffer && s_cp.wbuffer) ? D3DZB_USEW : D3DZB_TRUE;
    s_gbL = s_hal.dvGuardBandLeft; s_gbT = s_hal.dvGuardBandTop;
    s_gbR = s_hal.dvGuardBandRight; s_gbB = s_hal.dvGuardBandBottom;
    if (arg_has("-noguardband")) s_gbL = s_gbT = s_gbR = s_gbB = 0.0f;

    /* managed textures: accepted by CreateSurface? */
    {
        LPDIRECTDRAWSURFACE4 t;
        PFmt *pf = s_tf[TF_565].ok ? &s_tf[TF_565] : &s_tf[TF_555];
        t = create_tex_surface(64, 64, pf, 1, 0, DDSCAPS2_TEXTUREMANAGE, &hr);
        s_managed = t != NULL;
        if (t) t->Release();
        else log_printf(LOG_WARN, "warn: rend managed textures rejected hr=0x%08lX %s (sysmem copies + Load)",
                        (unsigned long)hr, hrname(hr));
        s_cp.managedTex = s_managed;
    }
    s_texKB = 0;
    s_texCount = 0;
    s_texBudgetKB = ri->texBudgetKB > 0 ? ri->texBudgetKB : 6144;
    m4_ident(&s_proj); m4_ident(&s_view); m4_ident(&s_world);
    s_wvpDirty = 1;
    s_fogOn = 0;
    d_set_material(NULL);
    default_states();
    validate_probe();
    s_cp.mod2xOp = s_mt2x;
    s_cp.addOp = s_mtAdd;

    s_modeN = 0;
    s_dd->EnumDisplayModes(0, NULL, NULL, modecb);
    measure_refresh();
    vidmem_kb(DDSCAPS_TEXTURE, &tot, &fr);
    s_cp.vidTotalKB = tot;
    s_cp.vidFreeKB = fr;
    if (fr >= 1024) {
        int cap = (int)(fr * 85 / 100);
        if (cap < s_texBudgetKB) s_texBudgetKB = cap;
    }
    log_caps();
    log_printf(LOG_INFO, "caps: summary hal=%d %dx%dx%d fs=%d zbits=%d zmode=%s wfog=%d fogtable=%d zbias=%d stages=%d "
               "mod2x=%d add=%d twopass=%d managed=%d maxtex=%dx%d", s_cp.hal, s_W, s_H, s_bpp, s_fs, s_cp.zBits,
               s_zmode == D3DZB_USEW ? "w" : "z", s_wfog, s_cp.fogTable, s_zbiasCap, s_cp.texStages, s_mt2x, s_mtAdd,
               s_twoPassOk, s_managed, s_cp.maxTexW, s_cp.maxTexH);
    log_printf(LOG_INFO, "tex: budget_kb=%d (ini %d, texture vidmem free_kb=%u total_kb=%u)", s_texBudgetKB, ri->texBudgetKB,
               fr, tot);
    s_secXform = s_secSubmit = s_secClear = s_cntClip = -1;
    if (arg_has("-bench")) {                        /* breakdown only in benches (QPC reads cost a few us each) */
        s_secXform = prof_section("r_xform");
        s_secSubmit = prof_section("r_submit");
        s_secClear = prof_section("r_clear");
        s_cntClip = prof_section("r_clipped");
    }
    s_pace.divisor = s_div;
    s_pace.refreshHz = s_cp.refreshHz;
    s_pace.vblankOk = s_vbOk;
    s_paceLogAt = qpc() + s_qpf * 60;
    for (i = 0; i < 3; i++) s_curTex[i] = 0;
    *caps = s_cp;
    return 1;
}

static int d_init(void *hwnd, const RInit *ri, RCaps *caps)
{
    if (init_inner(hwnd, ri, caps)) return 1;
    d_shutdown();                                   /* restores the desktop mode after a partial init */
    return 0;
}

static void d_shutdown(void)
{
    int i;
    if (s_flipLate + s_flipFirst)                   /* T16: GetFlipStatus works if some flips needed a 2nd blank */
        log_printf(LOG_INFO, "pace: flips first_blank=%d later=%d", s_flipFirst, s_flipLate);
    for (i = 0; i < MAX_TEX; i++) if (s_tex[i].used) tex_release_slot(&s_tex[i]);
    for (i = 0; i < MAX_VB; i++) if (s_vb[i].verts) { free(s_vb[i].verts); s_vb[i].verts = NULL; }
    if (s_texCount) log_printf(LOG_INFO, "tex: shutdown freed=%d kb=%d", s_texCount, s_texKB);
    s_texKB = s_texCount = 0;
    if (s_dev && s_vp) s_dev->DeleteViewport(s_vp);
    if (s_vp) { s_vp->Release(); s_vp = NULL; }
    if (s_dev) { s_dev->Release(); s_dev = NULL; }
    if (s_zbuf) { if (s_back) s_back->DeleteAttachedSurface(0, s_zbuf); s_zbuf->Release(); s_zbuf = NULL; }
    if (s_back) { s_back->Release(); s_back = NULL; }
    if (s_prim) { s_prim->Release(); s_prim = NULL; }
    if (s_clip) { s_clip->Release(); s_clip = NULL; }
    if (s_d3d) { s_d3d->Release(); s_d3d = NULL; }
    if (s_dd) {
        if (s_fs) { s_dd->RestoreDisplayMode(); s_dd->SetCooperativeLevel(s_hwnd, DDSCL_NORMAL); }
        s_dd->Release();
        s_dd = NULL;
    }
    if (s_dd1) { s_dd1->Release(); s_dd1 = NULL; }
    if (s_tlv) { free(s_tlv); s_tlv = NULL; }
    if (s_clipv) { free(s_clipv); s_clipv = NULL; }
    if (s_code) { free(s_code); s_code = NULL; }
    if (s_oidx) { free(s_oidx); s_oidx = NULL; }
    s_inScene = 0;
}

/* ================================================================ frame */
static void d_set_viewport(int x, int y, int w, int h)
{
    D3DVIEWPORT2 vd;
    if (!s_vp) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w <= 0 || x + w > s_W) w = s_W - x;
    if (h <= 0 || y + h > s_H) h = s_H - y;
    if (x == s_vx && y == s_vy && w == s_vw && h == s_vh) return;
    s_vx = x; s_vy = y; s_vw = w; s_vh = h;
    memset(&vd, 0, sizeof(vd));
    vd.dwSize = sizeof(vd);
    vd.dwX = (DWORD)x; vd.dwY = (DWORD)y; vd.dwWidth = (DWORD)w; vd.dwHeight = (DWORD)h;
    vd.dvClipX = -1.0f; vd.dvClipY = 1.0f; vd.dvClipWidth = 2.0f; vd.dvClipHeight = 2.0f;
    vd.dvMinZ = 0.0f; vd.dvMaxZ = 1.0f;
    s_vp->SetViewport2(&vd);
}

static int d_begin_frame(u32 clearRGB)
{
    D3DRECT r;
    HRESULT hr;
    if (!s_dev) return 0;
    if (s_hwnd && IsIconic(s_hwnd)) return 0;      /* minimised (alt-tab away from fullscreen minimises) */
    if (s_lost || s_prim->IsLost() == DDERR_SURFACELOST || s_back->IsLost() == DDERR_SURFACELOST) {
        s_lost = 1;
        return 0;
    }
    d_set_viewport(0, 0, s_W, s_H);
    r.x1 = 0; r.y1 = 0; r.x2 = s_W; r.y2 = s_H;
    prof_begin(s_secClear);
    hr = s_vp->Clear2(1, &r, (clearRGB & R_ZONLY) ? D3DCLEAR_ZBUFFER : (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER),
                      (D3DCOLOR)(clearRGB & 0xFFFFFF), 1.0f, 0);
    prof_end(s_secClear);
    if (hr == DDERR_SURFACELOST) { s_lost = 1; return 0; }
    hr = s_dev->BeginScene();
    if (FAILED(hr)) {
        if (hr == DDERR_SURFACELOST) s_lost = 1;
        else fail("BeginScene", hr);
        return 0;
    }
    s_inScene = 1;
    return 1;
}

/* jet T01: Z-only clear mid-frame (two-pass depth, DESIGN 9.1 Plan B); same call as d_begin_frame */
static void d_clear_z(void)
{
    D3DRECT r;
    HRESULT hr;
    if (!s_dev || !s_vp || s_lost) return;
    r.x1 = 0; r.y1 = 0; r.x2 = s_W; r.y2 = s_H;
    prof_begin(s_secClear);
    hr = s_vp->Clear2(1, &r, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    prof_end(s_secClear);
    if (hr == DDERR_SURFACELOST) s_lost = 1;
}

static void d_end_frame(void)
{
    if (!s_dev || !s_inScene) return;
    s_dev->EndScene();
    s_inScene = 0;
}

static void pace_record(int interval)
{
    if (interval < 1) interval = 1;
    s_pace.presents++;
    s_pace.hist[interval > 8 ? 8 : interval]++;
    if (interval > s_div) s_pace.misses++;
    if (interval > s_pace.worstBlanks) s_pace.worstBlanks = interval;
}

static void d_present(void)
{
    HRESULT hr;
    s64 now;
    int interval = 0, havePrev = s_lastPresent != 0;
    if (!s_dev || s_lost) return;
    now = qpc();
    if (!s_ri.noCap && s_vbOk && s_fs && !s_oldPace && !s_pace1) {
        /* T16 fullscreen pacing: spin on QPC until 0.6 refresh periods before the target blank (the previous blank
           + divisor periods), queue the Flip (the Voodoo swaps at the first retrace after it has drawn the frame), then
           wait for blank begins until the flip is done; that blank is the reference for the next frame and the
           measured interval goes into RPace. Waiting for the blank BEFORE the Flip (T05, -pace1) turned a frame whose
           GPU work ended just before the blank into a late one; on 86Box a single WaitForVerticalBlank also sleeps
           through a blank now and then, which T05's 2-3 waits per frame made frequent (and counted as on time). */
        s64 prev = s_lastPresent, tb;
        int tries = 0;
        if (havePrev) {
            s64 target = prev + (s_periodFx * (s64)s_div - s_periodFx * 6 / 10) / 1024;
            while (qpc() < target) {}
        }
        hr = s_prim->Flip(NULL, DDFLIP_WAIT);
        if (hr == DDERR_SURFACELOST) s_lost = 1;
        for (;;) {
            int done;
            s_dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, NULL);
            tb = qpc();
            tries++;
            if (s_lost || tries >= 3) break;
            done = s_prim->GetFlipStatus(DDGFS_ISFLIPDONE) != DDERR_WASSTILLDRAWING;
            if (!done) {                            /* the swap may not have registered yet: ask again after ~1 ms */
                s64 settle = tb + s_periodFx / 1024 / 8;
                while (qpc() < settle) {}
                done = s_prim->GetFlipStatus(DDGFS_ISFLIPDONE) != DDERR_WASSTILLDRAWING;
            }
            if (done) break;
        }
        if (tries > 1) s_flipLate++; else s_flipFirst++;
        s_lastPresent = tb;
        interval = havePrev ? (int)E_MINF(((s_lastPresent - prev) * 1024 + s_periodFx / 2) / s_periodFx, 1000) : s_div;
        if (havePrev) pace_record(interval);
        return;
    }
    if (!s_ri.noCap) {
        if (s_vbOk && s_oldPace) {               /* T05 pacing (-oldpace, for comparison) */
            int blanks = havePrev ? (int)E_MINF((now - s_lastPresent) * 1024 / s_periodFx, 1000) : s_div;
            int need = s_div - blanks, i;
            if (need < 1) need = 1;
            for (i = 0; i < need; i++) s_dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, NULL);
            interval = blanks + need;
            s_lastPresent = qpc();
        } else if (s_vbOk) {
            /* T16: spin on QPC until 0.6 refresh periods before the target blank, then wait for ONE blank begin. On
               86Box a WaitForVerticalBlank call now and then sleeps through a blank; T05's loop made 2-3 calls per
               frame, so a frame with headroom still came a blank late (and was counted as on time). The interval is
               now measured (rounded periods since the previous present), so late frames show up in RPace. */
            s64 prev = s_lastPresent;
            if (havePrev) {
                s64 target = prev + (s_periodFx * (s64)s_div - s_periodFx * 6 / 10) / 1024;
                while (qpc() < target) {}
            }
            s_dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, NULL);
            s_lastPresent = qpc();
            interval = havePrev ? (int)E_MINF(((s_lastPresent - prev) * 1024 + s_periodFx / 2) / s_periodFx, 1000) : s_div;
        } else {
            s64 target = s_lastPresent + s_cpuPeriod;
            if (havePrev && now < target) {
                int ms = (int)((target - now) * 1000 / s_qpf) - 1;
                if (ms > 0) Sleep((DWORD)ms);
                while (qpc() < target) {}
            }
            now = qpc();
            interval = havePrev ? (int)(((now - s_lastPresent) * 1024 + s_periodFx / 2) / s_periodFx) : s_div;
            s_lastPresent = now;
        }
    } else {
        interval = havePrev ? (int)(((now - s_lastPresent) * 1024 + s_periodFx / 2) / s_periodFx) : 1;
        s_lastPresent = now;
    }
    if (s_fs) {
        hr = s_prim->Flip(NULL, DDFLIP_WAIT);
    } else {
        POINT p;
        RECT r;
        p.x = 0; p.y = 0;
        ClientToScreen(s_hwnd, &p);
        r.left = p.x; r.top = p.y; r.right = p.x + s_W; r.bottom = p.y + s_H;
        hr = s_prim->Blt(&r, s_back, NULL, DDBLT_WAIT, NULL);
    }
    if (hr == DDERR_SURFACELOST) s_lost = 1;
    if (havePrev) pace_record(interval);
    if (log_level() >= LOG_DEBUG && s_lastPresent >= s_paceLogAt) {
        log_printf(LOG_DEBUG, "pace: presents=%d misses=%d worst=%d divisor=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d 8+=%d",
                   s_pace.presents, s_pace.misses, s_pace.worstBlanks, s_div, s_pace.hist[1], s_pace.hist[2],
                   s_pace.hist[3], s_pace.hist[4], s_pace.hist[5], s_pace.hist[6], s_pace.hist[7], s_pace.hist[8]);
        s_paceLogAt = s_lastPresent + s_qpf * 60;
    }
}

static void d_pace_stats(RPace *out, int reset)
{
    if (out) *out = s_pace;
    if (reset) {
        memset(&s_pace, 0, sizeof(s_pace));
        s_pace.divisor = s_div;
        s_pace.refreshHz = s_cp.refreshHz;
        s_pace.vblankOk = s_vbOk;
    }
}

static void d_device_lost_check(void)
{
    HRESULT hr;
    int i, n = 0;
    if (!s_dd || !s_lost) {
        if (!s_dd || !s_prim || s_prim->IsLost() != DDERR_SURFACELOST) return;
    }
    if (s_hwnd && IsIconic(s_hwnd)) return;
    hr = s_dd->RestoreAllSurfaces();
    if (FAILED(hr)) {
        log_printf(LOG_DEBUG, "rend: restore pending hr=0x%08lX %s", (unsigned long)hr, hrname(hr));
        return;
    }
    for (i = 0; i < MAX_TEX; i++) {
        if (!s_tex[i].used || !s_tex[i].systex) continue;
        if (SUCCEEDED(s_tex[i].tex->Load(s_tex[i].systex))) n++;
    }
    default_states();
    s_lost = 0;
    s_lastPresent = 0;
    log_printf(LOG_INFO, "rend: surfaces restored reloaded_tex=%d managed=%d", n, s_managed);
}

/* ================================================================ screenshot, stats, modes */
static int d_save_bmp(const char *path)
{
    DDSURFACEDESC2 sd;
    HRESULT hr;
    u32 *buf;
    int x, y, w, h, ok, bpp;
    DWORD rm, gm, bm;
    int rsft, gsft, bsft, rmax, gmax, bmax;
    if (!s_back || !path) return 0;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    hr = s_back->Lock(NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(hr)) return fail("shot Lock back buffer", hr);
    w = (int)sd.dwWidth; h = (int)sd.dwHeight;
    bpp = (int)sd.ddpfPixelFormat.dwRGBBitCount;
    rm = sd.ddpfPixelFormat.dwRBitMask; gm = sd.ddpfPixelFormat.dwGBitMask; bm = sd.ddpfPixelFormat.dwBBitMask;
    rsft = mshift(rm); gsft = mshift(gm); bsft = mshift(bm);
    rmax = (int)(rm >> rsft); gmax = (int)(gm >> gsft); bmax = (int)(bm >> bsft);
    if (rmax <= 0) rmax = 1;
    if (gmax <= 0) gmax = 1;
    if (bmax <= 0) bmax = 1;
    buf = (u32 *)malloc((size_t)w * (size_t)h * 4);
    if (!buf) { s_back->Unlock(NULL); return 0; }
    for (y = 0; y < h; y++) {
        const u8 *row = (const u8 *)sd.lpSurface + y * sd.lPitch;
        for (x = 0; x < w; x++) {
            DWORD v = bpp == 32 ? ((const DWORD *)row)[x] : (DWORD)((const u16 *)row)[x];
            int r = (int)((v & rm) >> rsft) * 255 / rmax, g = (int)((v & gm) >> gsft) * 255 / gmax;
            int b = (int)((v & bm) >> bsft) * 255 / bmax;
            buf[y * w + x] = 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }
    s_back->Unlock(NULL);
    ok = img_save_bmp24(path, buf, w, h);
    free(buf);
    log_printf(LOG_INFO, "rend: shot path=%s %dx%d bpp=%d ok=%d", path, w, h, bpp, ok);
    return ok;
}

static void d_stats(RStats *out, int reset)
{
    if (out) *out = s_st;
    if (reset) memset(&s_st, 0, sizeof(s_st));
}

static int d_mode_count(void) { return s_modeN; }

static int d_mode_at(int i, int *w, int *h, int *bpp)
{
    if (i < 0 || i >= s_modeN) return 0;
    if (w) *w = s_modeW[i];
    if (h) *h = s_modeH[i];
    if (bpp) *bpp = s_modeB[i];
    return 1;
}

static const RBackend s_d3d6 = {
    "d3d6", d_init, d_shutdown, d_begin_frame, d_end_frame, d_present, d_set_viewport, d_set_proj, d_set_view,
    d_set_world, d_set_fog, d_set_material, d_tex_create, d_tex_free, d_tex_kb, d_vb_create, d_vb_free, d_draw_vb,
    d_draw_mem, d_draw_2d, d_save_bmp, d_stats, d_device_lost_check,
    d_pace_stats, d_mode_count, d_mode_at, d_tex_budget_kb, d_tex_count,
    d_clear_z
};

const RBackend *r_backend_d3d6(void) { return &s_d3d6; }
