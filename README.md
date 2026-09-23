# Re60 Legacy Xenos Renderer

Historical source snapshot of Re60's retired pre-RT64 Fast3D/Xenos renderer. Re60 was subsequently retired; active Xbox 360 libultraship/Fast3D platform development continues in Shipyard. This repository is preserved unchanged as historical reference and is not a supported implementation.

Included files:

- `gfx_xdk.cpp` / `gfx_xdk.h` - Xbox 360 D3D9/Xenos implementation of the Fast3D rendering API.
- `gfx_xdk_shader.cpp` - SM3.0 shader generator adapted from the shared Direct3D combiner generator.
- `gfx_cc.cpp` / `gfx_cc.h` - combiner feature decoding used by the shader generator.
- `gfx_rendering_api.h` - rendering API contract used by the backend.
- `LICENSE.txt` - upstream n64-fast3d-engine license retained for the Fast3D-derived source.

## Status

This code is historical, unsupported, and incomplete. The framebuffer/offscreen/depth-readback path is stubbed. Three-point filtering and the noise/VPOS shader variant are not implemented in this snapshot.

The files were extracted from a larger historical Re60/Shipwright-era tree and are intended primarily as a reference implementation. Adjacent project revisions evolved independently, so a consumer may need to reconcile API typedef/signature differences with the exact Fast3D revision they are integrating against.

## External requirements

No Microsoft Xbox 360 SDK files are included. Building requires your own legally obtained XDK environment. The backend includes `<xtl.h>`, D3D9/D3DX9, and `<xgraphics.h>` and uses Xbox-specific texture tiling APIs.

`gfx_xdk.cpp` also includes `saving/oot_platform.h` for optional diagnostic file helpers (`OotPlatform_ResetFile` / `OotPlatform_AppendFile`). That Re60-specific platform header is intentionally not included in this renderer-only snapshot; replace those helpers or provide equivalent declarations if you enable/integrate the diagnostics.

## Provenance

The rendering API, combiner structures, and shared formula-generation lineage come from `n64-fast3d-engine` and the Ship of Harkinian / libultraship Fast3D path. The Xbox 360 D3D9/Xenos backend and SM3-specific adaptation were developed for the retired pre-RT64 Re60 renderer.

