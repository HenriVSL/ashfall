# racer/sdk: DirectX 6-level headers for VC6

VC6 ships DX5-level DirectX headers (1998: no IDirectDraw4 / IDirect3D3 / IDirect3DDevice3 /
vertex buffers / texture stages). This folder holds DX6/DX7-level headers so VC6 code can use the
Direct3D 6 interfaces, which the stock Win98 SE DirectX 6.1 runtime provides.

| File | Origin | Edits |
|---|---|---|
| `ddraw.h` | Open Watcom 2.0 `h/nt/ddraw.h` (`~/Documents/86box/tools/ow2`) | `__cdecl` removed from the `EXTERN_C const IID` GUID declarations (VC6 warning C4229) |
| `d3d.h` | Open Watcom 2.0 `h/nt/directx/d3d.h` | same `__cdecl` removal |
| `d3dcaps.h` | Open Watcom 2.0 `h/nt/directx/d3dcaps.h` | none |
| `d3dtypes.h` | Open Watcom 2.0 `h/nt/directx/d3dtypes.h` | (1) removed a stray `PURE` from the `LPD3DENUMTEXTUREFORMATSCALLBACK` and `LPD3DENUMPIXELFORMATSCALLBACK` typedefs (VC6 error C2071); (2) **`D3DVERTEX` was missing its `nx`/`dvNX` member** (struct was 28 bytes instead of 32), added; (3) `D3DLVERTEX` member `dv` renamed to `tv` (union with `dvTV`); (4) `D3DFVF_LVERTEX` used the typo `D3DVFX_TEX1`, now `D3DFVF_TEX1` |
| `dx6guid.cpp` | our own code | GUID definitions (see below) |
| `OWPL-LICENSE.txt` | copy of Open Watcom's `license.txt` | |

**License.** The four headers are Open Watcom files, "Copyright (c) 2004-2026 The Open Watcom
Contributors", distributed under the **Sybase Open Watcom Public License 1.0** (`OWPL-LICENSE.txt`).
OWPL is a file-level copyleft (like the MPL): you may use and redistribute them; the source of these
files (including our small edits, listed above) must stay available under OWPL if we distribute them,
e.g. in a public repo. Our own code that merely includes them is not affected. `dx6guid.cpp` is ours (public domain).

## How to use

```make
CFLAGS = /nologo /W3 /O2 /G6 /DWIN32 /I..\sdk
```
In every source file, before any DirectX include:
```c
#define WIN32_LEAN_AND_MEAN
#define _ENABLE_AUTODEPEND          /* skips the Watcom-only "#pragma read_only_file" */
#define DIRECTDRAW_VERSION 0x0600   /* DDCAPS = DDCAPS_DX6 */
#define DIRECT3D_VERSION   0x0600   /* stay at DX6 (the headers default to 0x0700) */
#include <windows.h>
#include <ddraw.h>                  /* /I..\sdk wins over VC6's own DDRAW.H */
#include <d3d.h>
```
Compile `..\sdk\dx6guid.cpp` into the program; **do not link `dxguid.lib`** (duplicate symbols). Link
`ddraw.lib` (for `DirectDrawCreate`); get IDirectDraw4 / IDirect3D3 by `QueryInterface`.
`spike/Makefile` is a working example.

Verified sizes under VC6 (probe3d logs them): D3DTLVERTEX/D3DLVERTEX/D3DVERTEX 32, D3DDEVICEDESC 252,
DDSURFACEDESC2 124, DDPIXELFORMAT 32, D3DVIEWPORT2 44, D3DLIGHT2 80, D3DMATERIAL 80, DDCAPS (DX6) 380, DDDEVICEIDENTIFIER 1064.
At DIRECT3D_VERSION 0x0600 a few DX7 caps names (e.g. `D3DDEVCAPS_HWTRANSFORMANDLIGHT`) are absent, as in the real SDK.

GUIDs: the OW headers only declare them (OW ships them in a library). The DX5-era values are copied from
VC6's `D3D.H`/`DDRAW.H`; the DX6 ones (IDirectDraw4, IDirectDrawSurface4, IDirect3D3, IDirect3DDevice3,
IDirect3DViewport3, IDirect3DVertexBuffer, IDirect3DMaterial3, Ref/Null device) were checked against the
byte patterns in the VM's DX 6.1 `DDRAW.DLL` / `D3DIM.DLL`.
