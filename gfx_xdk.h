// Re60 legacy Xenos renderer (historical / unsupported).
// Fast3D interface and combiner lineage: n64-fast3d-engine / Ship of Harkinian.
// Xbox 360 backend and SM3 adaptation are from the retired pre-RT64 Re60 renderer.
// See README.md and LICENSE.txt for scope and provenance.

#ifndef GFX_XDK_H
#define GFX_XDK_H

#include "gfx_rendering_api.h"

// Optional bounded render-state trace.
#ifndef OOT_SHOW_GFX_STATE_LOG
#define OOT_SHOW_GFX_STATE_LOG 0
#endif

// Optional generated-shader and vertex-declaration dump.
#ifndef OOT_SHOW_GFX_SHADER_DUMP
#define OOT_SHOW_GFX_SHADER_DUMP 0
#endif

// Optional debugger transport for shader/declaration failures.
#ifndef OOT_SHOW_XDK_DEBUG_OUTPUT
#define OOT_SHOW_XDK_DEBUG_OUTPUT 0
#endif

// Supply the host-created D3D9 device before gfx_init().
void gfx_xdk_set_device(LPDIRECT3DDEVICE9 device);

extern struct GfxRenderingAPI gfx_xdk_api;

#endif
