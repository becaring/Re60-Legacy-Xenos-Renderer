// Re60 legacy Xenos renderer (historical / unsupported).
// Fast3D interface and combiner lineage: n64-fast3d-engine / Ship of Harkinian.
// Xbox 360 backend and SM3 adaptation are from the retired pre-RT64 Re60 renderer.
// See README.md and LICENSE.txt for scope and provenance.

// Xbox 360/XDK D3D9 backend for Fast3D's GfxRenderingAPI.
#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <xgraphics.h>

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "gfx_xdk.h"
#include "gfx_cc.h"
#include "saving/oot_platform.h"

extern void gfx_xdk_build_shader(char buf[4096], size_t& len, size_t& num_floats, const CCFeatures& cc_features, bool include_root_signature, bool three_point_filtering);

// Optional legacy bring-up status exposed to the host integration.
enum GfxXdkDebugStatus {
    XDK_DEBUG_UNTOUCHED = 0,
    XDK_DEBUG_SHADER_CREATE_FAILED = 1,
    XDK_DEBUG_DRAW_SKIPPED_NULL_SHADER = 2,
    XDK_DEBUG_DRAW_CALLED_OK = 3,
    XDK_DEBUG_TEXTURE_CREATE_FAILED = 4,
    XDK_DEBUG_TEXTURE_LOCK_FAILED = 5,
};
static int g_xdk_debug_status = XDK_DEBUG_UNTOUCHED;

extern "C" int gfx_xdk_debug_get_status(void) {
    return g_xdk_debug_status;
}

struct ShaderProgramXdk {
    uint64_t shader_id0;
    uint64_t shader_id1;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;      // floats per vertex, for stride
    bool has_clamp[2][2];    // [tex][s/t] - drives whether texSize const is needed
    UINT vp_transform_reg;   // ACTUAL VS constant register of vpTransform (from constant table - the register() annotation may be ignored by this compiler)

    // Xenos does not implement HLSL discard in this shader path.
    // Texture-edge and alpha-threshold variants use fixed-function
    // alpha testing after the pixel shader instead.
    bool alpha_test_enabled;
    DWORD alpha_test_ref;
    D3DCMPFUNC alpha_test_func;

    LPDIRECT3DVERTEXSHADER9 vertex_shader;
    LPDIRECT3DPIXELSHADER9 pixel_shader;
    LPDIRECT3DVERTEXDECLARATION9 vertex_decl;
};

struct TextureXdk {
    LPDIRECT3DTEXTURE9 texture;
    uint32_t width, height;

    // Fast3D's shared texture cache treats filtering and addressing as
    // properties of the cached texture. D3D9/Xenos stores those values on
    // the sampler stage instead, so retain them here and restore them every
    // time this texture is rebound to a stage.
    bool sampler_valid;
    bool linear_filter;
    uint32_t cms;
    uint32_t cmt;
};

// ---------------------------------------------------------------------
// Globals - mirrors the `d3d` struct pattern in gfx_direct3d11.cpp
// ---------------------------------------------------------------------

static struct {
    LPDIRECT3DDEVICE9 device; // set via gfx_xdk_set_device() from main.cpp, we don't own device creation

    std::map<std::pair<uint64_t, uint64_t>, ShaderProgramXdk> shader_program_pool;
    ShaderProgramXdk *shader_program;

    std::vector<TextureXdk> textures;
    int current_tile;
    uint32_t bound_texture_id[2];

    uint32_t rt_width, rt_height; // backbuffer dims, captured in set_device - used by viewport emulation + scissor flip
    float vp_transform[4];        // current viewport-emulation constant values (scale.xy, offset.zw) - uploaded per-draw to each shader's ACTUAL register

    FilteringMode current_filter_mode;
} d3d; // Static storage provides zero initialization; non-zero defaults are set in gfx_xdk_set_device().

// The backend uses a D3D9 device created by the host integration.
void gfx_xdk_set_device(LPDIRECT3DDEVICE9 device) {
    d3d.device = device;
    d3d.shader_program = NULL;
    d3d.current_tile = 0;
    d3d.bound_texture_id[0] = 0;
    d3d.bound_texture_id[1] = 0;
    d3d.current_filter_mode = FILTER_LINEAR;

    // Capture render target dimensions from the device's initial (full
    // backbuffer) viewport - needed by the viewport-emulation constant
    // and the scissor y-flip.
    D3DVIEWPORT9 vp;
    if (SUCCEEDED(device->GetViewport(&vp))) {
        d3d.rt_width = vp.Width;
        d3d.rt_height = vp.Height;
    } else {
        d3d.rt_width = 1280;
        d3d.rt_height = 720;
    }

    // Identity viewport transform until the first set_viewport call
    // (scale 1, offset 0) so raw clip-space draws behave normally.
    d3d.vp_transform[0] = 1.0f;
    d3d.vp_transform[1] = 1.0f;
    d3d.vp_transform[2] = 0.0f;
    d3d.vp_transform[3] = 0.0f;
}

// ---------------------------------------------------------------------
// Clip parameters
// ---------------------------------------------------------------------

static struct GfxClipParameters gfx_xdk_get_clip_parameters(void) {
    // D3D-style clip space: z in [0,1], with no API-level Y inversion.
    GfxClipParameters params;
    params.z_is_from_0_to_1 = true;
    params.invert_y = false;
    return params;
}

#if OOT_SHOW_GFX_SHADER_DUMP
static const char OOT_GFX_SHADER_DUMP_PATH[] =
    "cache:\\gfx_shader_dump.txt";

static void gfx_shader_dump_append(const char* format, ...) {
    char line[2048];
    va_list args;
    int count;

    va_start(args, format);
    count = _vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);

    if (count < 0 || count >= (int)sizeof(line)) {
        line[sizeof(line) - 1] = '\0';
    } else {
        line[count] = '\0';
    }

    OotPlatform_AppendFile(OOT_GFX_SHADER_DUMP_PATH, line);
}

static void gfx_shader_dump_append_text(const char* text, size_t length) {
    char chunk[1024];
    size_t offset = 0;

    while (offset < length) {
        size_t copyLength = length - offset;
        if (copyLength >= sizeof(chunk)) {
            copyLength = sizeof(chunk) - 1;
        }
        memcpy(chunk, text + offset, copyLength);
        chunk[copyLength] = '\0';
        OotPlatform_AppendFile(OOT_GFX_SHADER_DUMP_PATH, chunk);
        offset += copyLength;
    }
}
#endif

// ---------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------

static void gfx_xdk_unload_shader(struct ShaderProgram *old_prg) {
    // Nothing to release per-unload; shader objects persist in the pool
    // for the process lifetime, same as the DX11 backend does.
}

static void gfx_xdk_load_shader(struct ShaderProgram *new_prg) {
    d3d.shader_program = (ShaderProgramXdk *)new_prg;
}

static struct ShaderProgram *gfx_xdk_create_and_load_new_shader(uint64_t shader_id0, uint64_t shader_id1) {
    CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    char buf[4096];
    size_t len, num_floats;
    gfx_xdk_build_shader(buf, len, num_floats, cc_features, false, false /* three_point_filtering: not ported yet */);

#if OOT_SHOW_GFX_SHADER_DUMP
    // Optional generated-HLSL and compile dump.
    gfx_shader_dump_append(
        "==== shader %016llX %016llX num_floats=%u ====\r\n",
        (unsigned long long)shader_id0,
        (unsigned long long)shader_id1,
        (unsigned)num_floats);
    gfx_shader_dump_append_text(buf, len);
    gfx_shader_dump_append("\r\n");
#endif

    LPD3DXBUFFER vs_code = nullptr, ps_code = nullptr;
    LPD3DXBUFFER vs_err = nullptr, ps_err = nullptr;

    HRESULT hr = D3DXCompileShader(buf, (UINT)len, NULL, NULL, "VSMain", "vs_3_0", 0, &vs_code, &vs_err, NULL);
#if OOT_SHOW_GFX_SHADER_DUMP
    gfx_shader_dump_append(
        "VSMain compile: hr=0x%08lX %s\r\n",
        (unsigned long)hr,
        FAILED(hr) ? "FAILED" : "ok");
    if (vs_err) {
        gfx_shader_dump_append(
            "VS errors:\r\n%s\r\n",
            (char*)vs_err->GetBufferPointer());
    }
#endif
    if (FAILED(hr)) {
        g_xdk_debug_status = XDK_DEBUG_SHADER_CREATE_FAILED;
#if OOT_SHOW_XDK_DEBUG_OUTPUT
        if (vs_err) {
            OutputDebugStringA("gfx_xdk: VSMain compile failed:\n");
            OutputDebugStringA((char*)vs_err->GetBufferPointer());
        }
#endif
        assert(false && "vertex shader compile failed - see debug output");
        return nullptr;
    }

    hr = D3DXCompileShader(buf, (UINT)len, NULL, NULL, "PSMain", "ps_3_0", 0, &ps_code, &ps_err, NULL);
#if OOT_SHOW_GFX_SHADER_DUMP
    gfx_shader_dump_append(
        "PSMain compile: hr=0x%08lX %s\r\n",
        (unsigned long)hr,
        FAILED(hr) ? "FAILED" : "ok");
    if (ps_err) {
        gfx_shader_dump_append(
            "PS errors:\r\n%s\r\n",
            (char*)ps_err->GetBufferPointer());
    }

    // Disassembly is diagnostic-only and is removed entirely when the
    // shader dump flag is disabled.
    {
        LPD3DXBUFFER disasm = NULL;
        if (vs_code && SUCCEEDED(D3DXDisassembleShader((DWORD*)vs_code->GetBufferPointer(), FALSE, NULL, &disasm))) {
            gfx_shader_dump_append(
                "---- VS disassembly ----\r\n%s\r\n",
                (char*)disasm->GetBufferPointer());
            disasm->Release();
            disasm = NULL;
        }
        if (SUCCEEDED(hr) && ps_code && SUCCEEDED(D3DXDisassembleShader((DWORD*)ps_code->GetBufferPointer(), FALSE, NULL, &disasm))) {
            gfx_shader_dump_append(
                "---- PS disassembly ----\r\n%s\r\n",
                (char*)disasm->GetBufferPointer());
            disasm->Release();
        }
    }
#endif
    if (FAILED(hr)) {
        g_xdk_debug_status = XDK_DEBUG_SHADER_CREATE_FAILED;
#if OOT_SHOW_XDK_DEBUG_OUTPUT
        if (ps_err) {
            OutputDebugStringA("gfx_xdk: PSMain compile failed:\n");
            OutputDebugStringA((char*)ps_err->GetBufferPointer());
        }
#endif
        assert(false && "pixel shader compile failed - see debug output");
        return nullptr;
    }

    ShaderProgramXdk *prg = &d3d.shader_program_pool[std::make_pair(shader_id0, shader_id1)];
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->num_inputs = cc_features.num_inputs;
    prg->num_floats = (uint8_t)num_floats;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->has_clamp[0][0] = cc_features.clamp[0][0];
    prg->has_clamp[0][1] = cc_features.clamp[0][1];
    prg->has_clamp[1][0] = cc_features.clamp[1][0];
    prg->has_clamp[1][1] = cc_features.clamp[1][1];

    prg->alpha_test_enabled = false;
    prg->alpha_test_ref = 0;
    prg->alpha_test_func = D3DCMP_ALWAYS;

    if (cc_features.opt_alpha && cc_features.opt_texture_edge) {
        // Shader converts coverage to output alpha 0.0 or 1.0.
        prg->alpha_test_enabled = true;
        prg->alpha_test_ref = 0;
        prg->alpha_test_func = D3DCMP_GREATER;
    } else if (cc_features.opt_alpha && cc_features.opt_alpha_threshold) {
        // Original test passes alpha >= 8/256.
        prg->alpha_test_enabled = true;
        prg->alpha_test_ref = 8;
        prg->alpha_test_func = D3DCMP_GREATEREQUAL;
    }

    // vpTransform is generated at c0. XDK constant-table name lookup is
    // unreliable for compiled microcode, so the annotated register is the
    // authoritative default; reflection is retained only as a cross-check.
    prg->vp_transform_reg = 0;
    {
        LPD3DXCONSTANTTABLE ctab = NULL;
        if (SUCCEEDED(D3DXGetShaderConstantTable((DWORD*)vs_code->GetBufferPointer(), &ctab)) && ctab) {
            D3DXHANDLE h = ctab->GetConstantByName(NULL, "vpTransform");
            if (h) {
                D3DXCONSTANT_DESC cdesc;
                UINT cnt = 1;
                if (SUCCEEDED(ctab->GetConstantDesc(h, &cdesc, &cnt))) {
                    prg->vp_transform_reg = cdesc.RegisterIndex;
                }
            }
            ctab->Release();
        }
#if OOT_SHOW_GFX_SHADER_DUMP
        gfx_shader_dump_append(
            "vpTransform actual register: c%u (annotation said c254)\r\n",
            prg->vp_transform_reg);
#endif
    }

    HRESULT hrVS = d3d.device->CreateVertexShader((DWORD*)vs_code->GetBufferPointer(), &prg->vertex_shader);
    if (FAILED(hrVS)) {
        g_xdk_debug_status = XDK_DEBUG_SHADER_CREATE_FAILED;
        assert(false && "CreateVertexShader failed");
        return nullptr;
    }
    HRESULT hrPS = d3d.device->CreatePixelShader((DWORD*)ps_code->GetBufferPointer(), &prg->pixel_shader);
    if (FAILED(hrPS)) {
        g_xdk_debug_status = XDK_DEBUG_SHADER_CREATE_FAILED;
        assert(false && "CreatePixelShader failed");
        return nullptr;
    }

    // D3D9 vertex declarations are built directly from the generated
    // interleaved Fast3D input layout.
    struct VeBuilder {
        // Preserve the full XDK D3DDECLTYPE value; it is not safe to pass
        // through an 8-bit intermediate on this toolchain.
        static D3DVERTEXELEMENT9 make(WORD offset, DWORD type, DWORD usage, BYTE usageIndex) {
            D3DVERTEXELEMENT9 e;
            e.Stream = 0;
            e.Offset = offset;
            e.Type = type;
            e.Method = D3DDECLMETHOD_DEFAULT;
            e.Usage = usage;
            e.UsageIndex = usageIndex;
            return e;
        }
    };

    D3DVERTEXELEMENT9 decl[16];
    BYTE idx = 0;
    WORD offset = 0;

    decl[idx++] = VeBuilder::make(offset, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
    offset += 16;

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            decl[idx++] = VeBuilder::make(offset, D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, (BYTE)i);
            offset += 8;
            // Clamp floats ride along as extra TEXCOORD slots (2,3,4,5),
            // matching the semantic numbering gfx_xdk_shader.cpp uses.
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    decl[idx++] = VeBuilder::make(offset, D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_TEXCOORD, (BYTE)(2 + i * 2 + j));
                    offset += 4;
                }
            }
        }
    }
    {
        BYTE nt = 6;
        if (cc_features.opt_fog) {
            decl[idx++] = VeBuilder::make(offset, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, nt++);
            offset += 16;
        }
        if (cc_features.opt_grayscale) {
            decl[idx++] = VeBuilder::make(offset, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, nt++);
            offset += 16;
        }
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        DWORD type = cc_features.opt_alpha ? D3DDECLTYPE_FLOAT4 : D3DDECLTYPE_FLOAT3;
        decl[idx++] = VeBuilder::make(offset, type, D3DDECLUSAGE_COLOR, (BYTE)i);
        offset += cc_features.opt_alpha ? 16 : 12;
    }
    // Use a named aggregate sentinel for compatibility with the XDK compiler.
    {
        static const D3DVERTEXELEMENT9 declEnd = D3DDECL_END();
        decl[idx] = declEnd;
    }

    // Sanity check: offset should match num_floats * 4 bytes. If not, the
    // shader text generator and this declaration builder have drifted
    // out of sync - loop structure must match gfx_xdk_shader.cpp exactly.
    assert(offset == num_floats * 4 && "vertex decl stride mismatch vs shader num_floats - check loop parity with gfx_xdk_shader.cpp");

#if OOT_SHOW_GFX_SHADER_DUMP
    // Diagnostic-only construction dump.
    gfx_shader_dump_append(
        "==== decl[] at construction (shader %016llX %016llX), idx=%u ====\r\n",
        (unsigned long long)shader_id0,
        (unsigned long long)shader_id1,
        (unsigned)idx);
    for (BYTE e = 0; e <= idx; e++) {
        gfx_shader_dump_append(
            "  [%u] Stream=%u Offset=%u Type=%u Method=%u Usage=%u UsageIndex=%u\r\n",
            e,
            decl[e].Stream,
            decl[e].Offset,
            decl[e].Type,
            decl[e].Method,
            decl[e].Usage,
            decl[e].UsageIndex);
    }
#endif

    HRESULT hrDecl = d3d.device->CreateVertexDeclaration(decl, &prg->vertex_decl);
    if (FAILED(hrDecl)) {
        g_xdk_debug_status = XDK_DEBUG_SHADER_CREATE_FAILED;
#if OOT_SHOW_XDK_DEBUG_OUTPUT
        OutputDebugStringA("gfx_xdk: CreateVertexDeclaration failed - check decl array contents/count.\n");
#endif
        assert(false && "CreateVertexDeclaration failed");
        return nullptr;
    }

    if (vs_code) vs_code->Release();
    if (ps_code) ps_code->Release();
    if (vs_err) vs_err->Release();
    if (ps_err) ps_err->Release();

    d3d.shader_program = prg;
    return (struct ShaderProgram *)prg;
}

static struct ShaderProgram *gfx_xdk_lookup_shader(uint64_t shader_id0, uint64_t shader_id1) {
    auto it = d3d.shader_program_pool.find(std::make_pair(shader_id0, shader_id1));
    return it == d3d.shader_program_pool.end() ? nullptr : (struct ShaderProgram *)&it->second;
}

static void gfx_xdk_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    ShaderProgramXdk *p = (ShaderProgramXdk *)prg;
    *num_inputs = p->num_inputs;
    used_textures[0] = p->used_textures[0];
    used_textures[1] = p->used_textures[1];
}

// ---------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------

static uint32_t gfx_xdk_new_texture(void) {
    // Named temporary keeps initialization compatible with the XDK compiler.
    TextureXdk tex;
    tex.texture = NULL;
    tex.width = 0;
    tex.height = 0;
    tex.sampler_valid = false;
    tex.linear_filter = false;
    tex.cms = 0;
    tex.cmt = 0;
    d3d.textures.push_back(tex);
    return (uint32_t)(d3d.textures.size() - 1);
}

static void gfx_xdk_delete_texture(uint32_t texture_id) {
    TextureXdk &tex = d3d.textures[texture_id];
    if (tex.texture) {
        tex.texture->Release();
        tex.texture = nullptr;
    }
}

static D3DTEXTUREADDRESS gfx_xdk_texture_address_mode(uint32_t value) {
    if (value & 0x2) {
        return D3DTADDRESS_CLAMP;
    }

    if (value & 0x1) {
        return D3DTADDRESS_MIRROR;
    }

    return D3DTADDRESS_WRAP;
}

static void gfx_xdk_apply_texture_sampler(
    int tile,
    const TextureXdk& texture) {

    D3DTEXTUREFILTERTYPE filter;

    if (!texture.sampler_valid) {
        return;
    }

    filter = texture.linear_filter ?
        D3DTEXF_LINEAR :
        D3DTEXF_POINT;

    d3d.device->SetSamplerState(
        tile,
        D3DSAMP_MAGFILTER,
        filter);

    d3d.device->SetSamplerState(
        tile,
        D3DSAMP_MINFILTER,
        filter);

    d3d.device->SetSamplerState(
        tile,
        D3DSAMP_ADDRESSU,
        gfx_xdk_texture_address_mode(texture.cms));

    d3d.device->SetSamplerState(
        tile,
        D3DSAMP_ADDRESSV,
        gfx_xdk_texture_address_mode(texture.cmt));
}

static void gfx_xdk_select_texture(
    int tile,
    uint32_t texture_id) {

    TextureXdk& texture = d3d.textures[texture_id];

    d3d.current_tile = tile;
    d3d.bound_texture_id[tile] = texture_id;

    d3d.device->SetTexture(
        tile,
        texture.texture);

    // A cache hit can select a texture whose desired wrap/filter state was
    // recorded earlier while the D3D sampler stage currently contains the
    // state of a different texture. Restore the selected texture's state.
    gfx_xdk_apply_texture_sampler(
        tile,
        texture);
}

// gfx_pc always hands us fully-decoded RGBA32 - all N64 texture format
// (CI4/CI8/IA16/etc) decoding already happened upstream. Our job is just
// upload, same as every other backend.
static void gfx_xdk_upload_texture(const uint8_t *rgba32_buf, uint32_t width, uint32_t height) {
    TextureXdk &tex = d3d.textures[d3d.bound_texture_id[d3d.current_tile]];

    if (tex.texture) {
        tex.texture->Release();
        tex.texture = NULL;
    }

    tex.width = width;
    tex.height = height;

    // Xbox A8R8G8B8 textures are tiled. Writing a conventional contiguous
    // RGBA stream directly into LockRect memory scrambles detailed textures.
    // Fill a pitch-aware linear image, then tile it with the XDK helper.
    d3d.device->SetTexture(d3d.current_tile, NULL);

    HRESULT hr = d3d.device->CreateTexture(
        width,
        height,
        1,
        0,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &tex.texture,
        NULL);

    if (FAILED(hr) || tex.texture == NULL) {
        g_xdk_debug_status = XDK_DEBUG_TEXTURE_CREATE_FAILED;
        tex.texture = NULL;
        return;
    }

    D3DLOCKED_RECT rect;
    hr = tex.texture->LockRect(0, &rect, NULL, 0);

    if (FAILED(hr) || rect.pBits == NULL) {
        g_xdk_debug_status = XDK_DEBUG_TEXTURE_LOCK_FAILED;
        tex.texture->Release();
        tex.texture = NULL;
        return;
    }

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t *dstRow = reinterpret_cast<uint32_t *>(
            reinterpret_cast<uint8_t *>(rect.pBits) + y * rect.Pitch);
        const uint8_t *srcRow = rgba32_buf + y * width * 4;

        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t r = srcRow[x * 4 + 0];
            const uint8_t g = srcRow[x * 4 + 1];
            const uint8_t b = srcRow[x * 4 + 2];
            const uint8_t a = srcRow[x * 4 + 3];
            dstRow[x] = D3DCOLOR_ARGB(a, r, g, b);
        }
    }

    DWORD tileFlags = 0;
    if (XGIsBorderTexture(tex.texture)) {
        tileFlags |= XGTILE_BORDER;
    }
    if (!XGIsPackedTexture(tex.texture)) {
        tileFlags |= XGTILE_NONPACKED;
    }

    XGTileTextureLevel(
        width,
        height,
        0,
        XGGetGpuFormat(D3DFMT_A8R8G8B8),
        tileFlags,
        rect.pBits,
        NULL,
        rect.pBits,
        rect.Pitch,
        NULL);

    tex.texture->UnlockRect(0);
    d3d.device->SetTexture(d3d.current_tile, tex.texture);
}

static void gfx_xdk_set_sampler_parameters(
    int tile,
    bool linear_filter,
    uint32_t cms,
    uint32_t cmt) {

    TextureXdk& texture =
        d3d.textures[d3d.bound_texture_id[tile]];

    // The shared Fast3D texture cache suppresses this callback when a cached
    // texture already has the requested parameters. Retain those parameters
    // on the backend texture so select_texture can restore the D3D9 sampler
    // stage after another texture (for example the clamped title logo) used it.
    texture.sampler_valid = true;
    texture.linear_filter = linear_filter;
    texture.cms = cms;
    texture.cmt = cmt;

    gfx_xdk_apply_texture_sampler(
        tile,
        texture);
}

// ---------------------------------------------------------------------
// Render state
// ---------------------------------------------------------------------

// Optional bounded render-state trace. Disabled by default.
#if OOT_SHOW_GFX_STATE_LOG
#define OOT_GFX_STATE_LOG_MAX_BLOCKS 60
static const char OOT_GFX_STATE_LOG_PATH[] =
    "cache:\\gfx_state_log.txt";
static int g_statelog_count = 0;
static bool g_statelog_cap_written = false;

static bool statelog_begin_block(void) {
    if (g_statelog_count >= OOT_GFX_STATE_LOG_MAX_BLOCKS) {
        if (!g_statelog_cap_written) {
            OotPlatform_AppendFile(
                OOT_GFX_STATE_LOG_PATH,
                "[state log bound reached: max_blocks=60, further records suppressed]\r\n");
            g_statelog_cap_written = true;
        }
        return false;
    }

    if (g_statelog_count == 0) {
        if (OotPlatform_ResetFile(OOT_GFX_STATE_LOG_PATH) != 0) {
            return false;
        }
    }

    g_statelog_count++;
    return true;
}

static void statelog_append(const char* format, ...) {
    char line[2048];
    va_list args;
    int count;

    va_start(args, format);
    count = _vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);

    if (count < 0 || count >= (int)sizeof(line)) {
        line[sizeof(line) - 1] = '\0';
    } else {
        line[count] = '\0';
    }

    OotPlatform_AppendFile(OOT_GFX_STATE_LOG_PATH, line);
}
#endif

static void gfx_xdk_set_depth_test_and_mask(bool depth_test, bool z_upd) {
#if OOT_SHOW_GFX_STATE_LOG
    if (statelog_begin_block()) {
        statelog_append(
            "set_depth_test_and_mask(%d, %d)\r\n",
            (int)depth_test,
            (int)z_upd);
    }
#endif
    d3d.device->SetRenderState(D3DRS_ZENABLE, depth_test ? D3DZB_TRUE : D3DZB_FALSE);
    d3d.device->SetRenderState(D3DRS_ZWRITEENABLE, z_upd);
}

static void gfx_xdk_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        // Use a named float so its bit pattern can be passed to D3D9.
        float bias = -2.0f;
        d3d.device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD*)&bias);
        d3d.device->SetRenderState(D3DRS_DEPTHBIAS, 0);
    } else {
        d3d.device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
        d3d.device->SetRenderState(D3DRS_DEPTHBIAS, 0);
    }
}

static void gfx_xdk_set_viewport(int x, int y, int width, int height) {
#if OOT_SHOW_GFX_STATE_LOG
    if (statelog_begin_block()) {
        statelog_append(
            "set_viewport(%d, %d, %d, %d) -> emulated via c254\r\n",
            x, y, width, height);
    }
#endif

    // Fast3D may request negative or oversized GL-style viewports, while
    // D3D9 uses unsigned viewport coordinates. Keep the hardware viewport
    // at the full render target and apply the requested viewport as a
    // clip-space scale/offset in the vertex shader.
    //
    // Derivation (Y follows the same form after origin conversion):
    //   pixel = vp.x + (ndc+1)/2 * vp.w        (intended mapping)
    //   pixel = (ndc'+1)/2 * RT.w              (full-RT mapping)
    //   => ndc' = ndc * (vp.w/RT.w) + (2*vp.x + vp.w)/RT.w - 1
    float sx = (float)width / (float)d3d.rt_width;
    float sy = (float)height / (float)d3d.rt_height;
    float ox = (2.0f * (float)x + (float)width) / (float)d3d.rt_width - 1.0f;
    float oy = (2.0f * (float)y + (float)height) / (float)d3d.rt_height - 1.0f;
    d3d.vp_transform[0] = sx;
    d3d.vp_transform[1] = sy;
    d3d.vp_transform[2] = ox;
    d3d.vp_transform[3] = oy;
    // The transform is uploaded per draw to the shader's selected constant register.
}

static void gfx_xdk_set_scissor(int x, int y, int width, int height) {
#if OOT_SHOW_GFX_STATE_LOG
    if (statelog_begin_block()) {
        statelog_append(
            "set_scissor(%d, %d, %d, %d)\r\n",
            x, y, width, height);
    }
#endif
    // Convert Fast3D's bottom-origin scissor to D3D top-origin coordinates
    // and clamp to the render-target bounds.
    LONG top = (LONG)d3d.rt_height - (LONG)y - (LONG)height;
    LONG bottom = (LONG)d3d.rt_height - (LONG)y;
    LONG left = (LONG)x;
    LONG right = (LONG)x + (LONG)width;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (LONG)d3d.rt_width) right = (LONG)d3d.rt_width;
    if (bottom > (LONG)d3d.rt_height) bottom = (LONG)d3d.rt_height;
    RECT r = { left, top, right, bottom };
    d3d.device->SetScissorRect(&r);
    d3d.device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
}

static void gfx_xdk_set_use_alpha(bool use_alpha) {
#if OOT_SHOW_GFX_STATE_LOG
    if (statelog_begin_block()) {
        statelog_append("set_use_alpha(%d)\r\n", (int)use_alpha);
    }
#endif
    d3d.device->SetRenderState(D3DRS_ALPHABLENDENABLE, use_alpha);
    if (use_alpha) {
        d3d.device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d3d.device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
}

// ---------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------

static void gfx_xdk_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!d3d.shader_program) {
        if (g_xdk_debug_status == XDK_DEBUG_UNTOUCHED) g_xdk_debug_status = XDK_DEBUG_DRAW_SKIPPED_NULL_SHADER;
        return;
    }

    d3d.device->SetVertexDeclaration(d3d.shader_program->vertex_decl);
    d3d.device->SetVertexShader(d3d.shader_program->vertex_shader);
    d3d.device->SetPixelShader(d3d.shader_program->pixel_shader);

    // D3D9 sampler state belongs to the texture stage, not the texture
    // object. Rebind both the selected texture and the sampler state stored
    // for that cached texture before every draw that uses the stage.
    for (int texture_index = 0;
         texture_index < 2;
         ++texture_index) {

        if (d3d.shader_program->used_textures[texture_index]) {
            TextureXdk& texture =
                d3d.textures[
                    d3d.bound_texture_id[texture_index]];

            d3d.device->SetTexture(
                texture_index,
                texture.texture);

            gfx_xdk_apply_texture_sampler(
                texture_index,
                texture);
        }
    }

    // Viewport-emulation constant, uploaded to this shader's ACTUAL
    // register (from its constant table at creation time).
#if OOT_SHOW_GFX_STATE_LOG
    HRESULT hrConst = d3d.device->SetVertexShaderConstantF(d3d.shader_program->vp_transform_reg, d3d.vp_transform, 1);
    if (statelog_begin_block()) {
        statelog_append(
            "draw: vpTransform reg=c%u hr=0x%08lX values=(%f, %f, %f, %f)\r\n",
            d3d.shader_program->vp_transform_reg,
            (unsigned long)hrConst,
            d3d.vp_transform[0],
            d3d.vp_transform[1],
            d3d.vp_transform[2],
            d3d.vp_transform[3]);
    }
#else
    d3d.device->SetVertexShaderConstantF(d3d.shader_program->vp_transform_reg, d3d.vp_transform, 1);
#endif

    // texSize constants for clamp math (see gfx_xdk_shader.cpp - c2/c3 in
    // the PS constant file). Set every draw since different draws may
    // bind different textures; cheap relative to a texture bind anyway.
    for (int i = 0; i < 2; i++) {
        if (d3d.shader_program->used_textures[i] && (d3d.shader_program->has_clamp[i][0] || d3d.shader_program->has_clamp[i][1])) {
            TextureXdk &tex = d3d.textures[d3d.bound_texture_id[i]];
            float texsize[4] = { (float)tex.width, (float)tex.height, 0, 0 };
            d3d.device->SetPixelShaderConstantF(2 + i, texsize, 1);
        }
    }

    // Fast3D supplies the interleaved vertex stream consumed by the current
    // declaration; stride is num_floats * sizeof(float).
    UINT stride = d3d.shader_program->num_floats * sizeof(float);
#if OOT_SHOW_GFX_STATE_LOG
    if (statelog_begin_block()) {
        statelog_append(
            "draw_triangles: num_tris=%u num_floats=%u stride=%u\r\n",
            (unsigned)buf_vbo_num_tris,
            (unsigned)d3d.shader_program->num_floats,
            (unsigned)stride);
        statelog_append("  v0:");
        for (unsigned i = 0;
             i < d3d.shader_program->num_floats && i < 16;
             i++) {
            statelog_append(" %f", buf_vbo[i]);
        }
        statelog_append("\r\n  v1:");
        for (unsigned i = 0;
             i < d3d.shader_program->num_floats && i < 16;
             i++) {
            statelog_append(
                " %f",
                buf_vbo[d3d.shader_program->num_floats + i]);
        }
        statelog_append("\r\n  v2:");
        for (unsigned i = 0;
             i < d3d.shader_program->num_floats && i < 16;
             i++) {
            statelog_append(
                " %f",
                buf_vbo[2 * d3d.shader_program->num_floats + i]);
        }
        statelog_append("\r\n");
    }
#endif
    // Normalize backend-owned fixed state while preserving per-batch depth
    // and blend state selected by the Fast3D callbacks.
    {
        D3DVIEWPORT9 vp = { 0, 0, d3d.rt_width, d3d.rt_height, 0.0f, 1.0f };
        d3d.device->SetViewport(&vp);
        // Preserve depth state selected by gfx_xdk_set_depth_test_and_mask().
        d3d.device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        // Do not override ALPHABLENDENABLE here. gfx_pc already selected
        // the required blend state through gfx_xdk_set_use_alpha().
        d3d.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d3d.device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    }

    d3d.device->SetRenderState(
        D3DRS_ALPHATESTENABLE,
        d3d.shader_program->alpha_test_enabled ? TRUE : FALSE);

    if (d3d.shader_program->alpha_test_enabled) {
        d3d.device->SetRenderState(
            D3DRS_ALPHAFUNC,
            d3d.shader_program->alpha_test_func);
        d3d.device->SetRenderState(
            D3DRS_ALPHAREF,
            d3d.shader_program->alpha_test_ref);
    }

    d3d.device->DrawPrimitiveUP(
        D3DPT_TRIANGLELIST,
        (UINT)buf_vbo_num_tris,
        buf_vbo,
        stride);
    g_xdk_debug_status = XDK_DEBUG_DRAW_CALLED_OK;

    // DrawPrimitiveUP is simple but not optimal; a persistent dynamic
    // vertex-buffer ring is the natural replacement if submission cost matters.
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

static void gfx_xdk_init(void) {
    // The host must provide a valid D3D9 device before Fast3D initialization.
    assert(d3d.device && "gfx_xdk_set_device() must be called before gfx_init()");
    d3d.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); // gfx_pc handles culling itself via combiner/geo mode, not D3D state

    // Start with depth disabled because Fast3D's 2D rectangle path may use
    // z=-1, outside D3D9's [0,1] clip range. Per-batch state re-enables depth
    // when required by 3D geometry.
    d3d.device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
}

static void gfx_xdk_on_resize(void) {
    // No-op for now: 360 output resolution is fixed per-boot (set once in
    // initDirect3D's presentParameters), no runtime window resize exists
    // on a console the way it does on PC.
}

#if OOT_SHOW_GFX_STATE_LOG
static int g_beginCount = 0, g_endCount = 0;
#endif

static void gfx_xdk_start_frame(void) {
    // Hardware viewport stays pinned to the full render target - the
    // logical (gfx_pc-requested) viewport is applied in the vertex
    // shader via constant c254 instead (see gfx_xdk_set_viewport).
    D3DVIEWPORT9 vp = { 0, 0, d3d.rt_width, d3d.rt_height, 0.0f, 1.0f };
    d3d.device->SetViewport(&vp);

    d3d.device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

#if OOT_SHOW_GFX_STATE_LOG
    HRESULT hrBegin = d3d.device->BeginScene();
    g_beginCount++;
    if (statelog_begin_block()) {
        statelog_append(
            "BeginScene #%d hr=0x%08lX (endCount so far=%d)\r\n",
            g_beginCount,
            (unsigned long)hrBegin,
            g_endCount);
    }
#else
    d3d.device->BeginScene();
#endif
}

static void gfx_xdk_end_frame(void) {
#if OOT_SHOW_GFX_STATE_LOG
    HRESULT hrEnd = d3d.device->EndScene();
    g_endCount++;
    if (statelog_begin_block()) {
        statelog_append(
            "EndScene #%d hr=0x%08lX (beginCount so far=%d)\r\n",
            g_endCount,
            (unsigned long)hrEnd,
            g_beginCount);
    }
#else
    d3d.device->EndScene();
#endif
}

static void gfx_xdk_finish_render(void) {
    // Presentation is owned by the window-manager swap path. Keeping this
    // callback empty avoids a second Present() on a discard backbuffer.
}

// ---------------------------------------------------------------------
// Framebuffer / offscreen / depth-readback path (not implemented).
// ---------------------------------------------------------------------

static int gfx_xdk_create_framebuffer(void) {
    assert(false && "gfx_xdk_create_framebuffer: not implemented yet");
    return 0;
}

static void gfx_xdk_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level, bool opengl_invert_y, bool render_target, bool has_depth_buffer, bool can_extract_depth) {
    // Parameter updates are ignored until framebuffer creation is implemented.
}

static void gfx_xdk_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    assert(false && "gfx_xdk_start_draw_to_framebuffer: not implemented yet");
}

static void gfx_xdk_clear_framebuffer(void) {
    d3d.device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
}

static void gfx_xdk_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    assert(false && "gfx_xdk_resolve_msaa_color_buffer: not implemented yet (no MSAA path yet)");
}

static std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> gfx_xdk_get_pixel_depth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    assert(false && "gfx_xdk_get_pixel_depth: not implemented yet (needed for lens-of-truth-style effects, not core rendering)");
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> empty;
    return empty;
}

static void *gfx_xdk_get_framebuffer_texture_id(int fb_id) {
    assert(false && "gfx_xdk_get_framebuffer_texture_id: not implemented yet");
    return nullptr;
}

static void gfx_xdk_select_texture_fb(int fb_id) {
    assert(false && "gfx_xdk_select_texture_fb: not implemented yet");
}

static void gfx_xdk_set_texture_filter(FilteringMode mode) {
    d3d.current_filter_mode = mode;
}

static FilteringMode gfx_xdk_get_texture_filter(void) {
    return d3d.current_filter_mode;
}

// ---------------------------------------------------------------------
// API table
// ---------------------------------------------------------------------

struct GfxRenderingAPI gfx_xdk_api = {
    gfx_xdk_get_clip_parameters,
    gfx_xdk_unload_shader,
    gfx_xdk_load_shader,
    gfx_xdk_create_and_load_new_shader,
    gfx_xdk_lookup_shader,
    gfx_xdk_shader_get_info,
    gfx_xdk_new_texture,
    gfx_xdk_select_texture,
    gfx_xdk_upload_texture,
    gfx_xdk_set_sampler_parameters,
    gfx_xdk_set_depth_test_and_mask,
    gfx_xdk_set_zmode_decal,
    gfx_xdk_set_viewport,
    gfx_xdk_set_scissor,
    gfx_xdk_set_use_alpha,
    gfx_xdk_draw_triangles,
    gfx_xdk_init,
    gfx_xdk_on_resize,
    gfx_xdk_start_frame,
    gfx_xdk_end_frame,
    gfx_xdk_finish_render,
    gfx_xdk_create_framebuffer,
    gfx_xdk_update_framebuffer_parameters,
    gfx_xdk_start_draw_to_framebuffer,
    gfx_xdk_clear_framebuffer,
    gfx_xdk_resolve_msaa_color_buffer,
    gfx_xdk_get_pixel_depth,
    gfx_xdk_get_framebuffer_texture_id,
    gfx_xdk_select_texture_fb,
    gfx_xdk_delete_texture,
    gfx_xdk_set_texture_filter,
    gfx_xdk_get_texture_filter,
};
