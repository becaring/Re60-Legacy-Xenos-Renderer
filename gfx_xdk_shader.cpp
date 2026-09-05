// Re60 legacy Xenos renderer (historical / unsupported).
// Fast3D interface and combiner lineage: n64-fast3d-engine / Ship of Harkinian.
// Xbox 360 backend and SM3 adaptation are from the retired pre-RT64 Re60 renderer.
// See README.md and LICENSE.txt for scope and provenance.

// SM3.0 shader generator for the legacy XDK backend. It adapts the shared
// Direct3D combiner generator to D3D9/Xenos: flat constant registers,
// POSITION/COLOR semantics, sampler2D/tex2D, and explicit texture-size
// constants for clamp math. Root signatures are not applicable.
// Three-point filtering and noise/VPOS variants are not implemented here.

#include <cstdio>
#include <cstring>

#include "gfx_cc.h"

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\r';
    buf[(*len)++] = '\n';
}

// Identical to the DX11 version - this part is pure combiner-formula logic,
// not API-specific, so it ports verbatim.
static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            default:
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "float4(1.0, 1.0, 1.0, 1.0)" : "float3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "input.input1" : "input.input1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "input.input2" : "input.input2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "input.input3" : "input.input3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "input.input4" : "input.input4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" : (with_alpha ? "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)" : "float3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a" : (with_alpha ? "float4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)" : "float3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
        }
    } else {
        switch (item) {
            default:
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "input.input1.a";
            case SHADER_INPUT_2:
                return "input.input2.a";
            case SHADER_INPUT_3:
                return "input.input3.a";
            case SHADER_INPUT_4:
                return "input.input4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
        }
    }
}

// Identical to the DX11 version - pure formula assembly, not API-specific.
static void append_formula(char *buf, size_t *len, const uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "lerp(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

// Signature intentionally matches gfx_direct3d_common_build_shader() so
// gfx_xdk.cpp can call it as a drop-in. include_root_signature and
// three_point_filtering are accepted but currently ignored (see notes
// at top of file) - kept so the call site doesn't need special-casing.
void gfx_xdk_build_shader(char buf[4096], size_t& len, size_t& num_floats, const CCFeatures& cc_features, bool include_root_signature, bool three_point_filtering) {
    len = 0;
    num_floats = 4; // position (x,y,z,w)

    // SM3 POSITION is a vertex-shader output semantic, not a pixel-shader
    // input. Keep position in VSOut and expose only interpolants through PSIn.
    append_line(buf, &len, "struct PSIn {");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            len += sprintf(buf + len, "    float2 uv%d : TEXCOORD%d;\r\n", i, i);
            num_floats += 2;
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    len += sprintf(buf + len, "    float texClamp%s%d : TEXCOORD%d;\r\n", j == 0 ? "S" : "T", i, 2 + i * 2 + j);
                    num_floats += 1;
                }
            }
        }
    }
    // NOTE: texClamp semantics above reuse TEXCOORD slots starting at 2 to
    // avoid inventing custom semantic names, which pre-SM4 HLSL is pickier
    // about accepting on arbitrary struct fields. Slot numbering must stay
    // consistent between the VS output and PS input struct - it is, since
    // both are generated from this same loop structure.
    int next_texcoord = 6; // 0,1 = uv0/uv1 ; 2-5 reserved for up to 4 clamp floats above
    if (cc_features.opt_fog) {
        len += sprintf(buf + len, "    float4 fog : TEXCOORD%d;\r\n", next_texcoord++);
        num_floats += 4;
    }
    if (cc_features.opt_grayscale) {
        len += sprintf(buf + len, "    float4 grayscale : TEXCOORD%d;\r\n", next_texcoord++);
        num_floats += 4;
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        len += sprintf(buf + len, "    float%d input%d : COLOR%d;\r\n", cc_features.opt_alpha ? 4 : 3, i + 1, i);
        num_floats += cc_features.opt_alpha ? 4 : 3;
    }
    append_line(buf, &len, "};");

    // D3D9 cannot represent Fast3D's negative/oversized logical viewports
    // directly, so apply the viewport transform in clip space. The offset
    // is multiplied by w because this occurs before perspective division.
    // Pin the transform to c0; XDK constant-table name lookup is unreliable
    // on compiled microcode.
    append_line(buf, &len, "float4 vpTransform : register(c0);");

    // VSOut: position (POSITION is a legal *vertex shader output*
    // semantic in SM3) + the same interpolants, nested. HLSL flattens
    // nested structs with semantics.
    append_line(buf, &len, "struct VSOut {");
    append_line(buf, &len, "    float4 position : POSITION;");
    append_line(buf, &len, "    PSIn i;");
    append_line(buf, &len, "};");

    // ---- Textures and samplers (old D3D9 HLSL style) ----
    if (cc_features.used_textures[0]) {
        append_line(buf, &len, "sampler2D g_texture0 : register(s0);");
    }
    if (cc_features.used_textures[1]) {
        append_line(buf, &len, "sampler2D g_texture1 : register(s1);");
    }

    // ---- Flat constant registers (replaces cbuffers) ----
    // Vertex shader constants live in c0-c... of the VS constant file;
    // pixel shader constants are a SEPARATE register file in D3D9 (unlike
    // SM4's unified space), so PS-only values below use the PS register
    // file starting at c0 as well - no collision with VS registers.
    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(buf, &len, "float noise_scale : register(c0);");
        append_line(buf, &len, "float noise_frame : register(c1);");
        append_line(buf, &len, "float random(float3 value) {");
        append_line(buf, &len, "    float random = frac(sin(dot(value, float3(12.9898, 78.233, 37.719))) * 143758.5453);");
        append_line(buf, &len, "    return random;");
        append_line(buf, &len, "}");
    }

    // Texture dimensions, needed for clamp math since ps_3_0 has no
    // GetDimensions(). Backend sets these per draw when clamp is active.
    // Packed as one float4 per texture stage: (widthS, heightS, 0, 0) -
    // only x/y used, z/w reserved for future (e.g. 3-point filtering re-add).
    bool need_texsize0 = cc_features.used_textures[0] && (cc_features.clamp[0][0] || cc_features.clamp[0][1]);
    bool need_texsize1 = cc_features.used_textures[1] && (cc_features.clamp[1][0] || cc_features.clamp[1][1]);
    if (need_texsize0) {
        append_line(buf, &len, "float4 texSize0 : register(c2);");
    }
    if (need_texsize1) {
        append_line(buf, &len, "float4 texSize1 : register(c3);");
    }

    // ---- Vertex shader ----
    append_str(buf, &len, "VSOut VSMain(float4 position : POSITION");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            len += sprintf(buf + len, ", float2 uv%d : TEXCOORD%d", i, i);
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    len += sprintf(buf + len, ", float texClamp%s%d : TEXCOORD%d", j == 0 ? "S" : "T", i, 2 + i * 2 + j);
                }
            }
        }
    }
    {
        int nt = 6;
        if (cc_features.opt_fog) {
            len += sprintf(buf + len, ", float4 fog : TEXCOORD%d", nt++);
        }
        if (cc_features.opt_grayscale) {
            len += sprintf(buf + len, ", float4 grayscale : TEXCOORD%d", nt++);
        }
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        len += sprintf(buf + len, ", float%d input%d : COLOR%d", cc_features.opt_alpha ? 4 : 3, i + 1, i);
    }
    append_line(buf, &len, ") {");
    append_line(buf, &len, "    VSOut result;");
    // Build the transformed position in a temporary and export the full
    // float4 at once; this avoids partial oPos writes in Xenos microcode.
    append_line(buf, &len, "    float4 vpPos = position;");
    append_line(buf, &len, "    vpPos.xy = position.xy * vpTransform.xy + position.w * vpTransform.zw;");
    append_line(buf, &len, "    result.position = vpPos;");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            len += sprintf(buf + len, "    result.i.uv%d = uv%d;\r\n", i, i);
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    len += sprintf(buf + len, "    result.i.texClamp%s%d = texClamp%s%d;\r\n", j == 0 ? "S" : "T", i, j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(buf, &len, "    result.i.fog = fog;");
    }
    if (cc_features.opt_grayscale) {
        append_line(buf, &len, "    result.i.grayscale = grayscale;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        len += sprintf(buf + len, "    result.i.input%d = input%d;\r\n", i + 1, i + 1);
    }
    append_line(buf, &len, "    return result;");
    append_line(buf, &len, "}");

    // ---- Pixel shader ----
    // Takes PSIn (no position - illegal as a ps_3_0 input semantic).
    append_line(buf, &len, "float4 PSMain(PSIn input) : COLOR0 {");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            len += sprintf(buf + len, "    float2 tc%d = input.uv%d;\r\n", i, i);
            bool s = cc_features.clamp[i][0], t = cc_features.clamp[i][1];
            const char *texsize = i == 0 ? "texSize0" : "texSize1";
            if (s && t) {
                len += sprintf(buf + len, "    tc%d = clamp(tc%d, 0.5 / %s.xy, float2(input.texClampS%d, input.texClampT%d));\r\n", i, i, texsize, i, i);
            } else if (s) {
                len += sprintf(buf + len, "    tc%d = float2(clamp(tc%d.x, 0.5 / %s.x, input.texClampS%d), tc%d.y);\r\n", i, i, texsize, i, i);
            } else if (t) {
                len += sprintf(buf + len, "    tc%d = float2(tc%d.x, clamp(tc%d.y, 0.5 / %s.y, input.texClampT%d));\r\n", i, i, i, texsize, i);
            }
            len += sprintf(buf + len, "    float4 texVal%d = tex2D(g_texture%d, tc%d);\r\n", i, i, i);
        }
    }

    append_str(buf, &len, cc_features.opt_alpha ? "    float4 texel;" : "    float3 texel;");
    for (int c = 0; c < (cc_features.opt_2cyc ? 2 : 1); c++) {
        append_str(buf, &len, "texel = ");
        if (!cc_features.color_alpha_same[c] && cc_features.opt_alpha) {
            append_str(buf, &len, "float4(");
            append_formula(buf, &len, cc_features.c[c], cc_features.do_single[c][0], cc_features.do_multiply[c][0], cc_features.do_mix[c][0], false, false, true);
            append_str(buf, &len, ", ");
            append_formula(buf, &len, cc_features.c[c], cc_features.do_single[c][1], cc_features.do_multiply[c][1], cc_features.do_mix[c][1], true, true, true);
            append_str(buf, &len, ")");
        } else {
            append_formula(buf, &len, cc_features.c[c], cc_features.do_single[c][0], cc_features.do_multiply[c][0], cc_features.do_mix[c][0], cc_features.opt_alpha, false, cc_features.opt_alpha);
        }
        append_line(buf, &len, ";");
    }

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        // Xenos microcode compiler does not implement HLSL discard.
        // Convert coverage into binary alpha; gfx_xdk.cpp performs the
        // equivalent rejection with fixed-function alpha testing.
        append_line(buf, &len, "    texel.a = texel.a > 0.19 ? 1.0 : 0.0;");
    }
    if (cc_features.opt_fog) {
        if (cc_features.opt_alpha) {
            append_line(buf, &len, "    texel = float4(lerp(texel.rgb, input.fog.rgb, input.fog.a), texel.a);");
        } else {
            append_line(buf, &len, "    texel = lerp(texel, input.fog.rgb, input.fog.a);");
        }
    }

    if (cc_features.opt_grayscale) {
        append_line(buf, &len, "    float intensity = (texel.r + texel.g + texel.b) / 3.0;");
        append_line(buf, &len, "    float3 new_texel = input.grayscale.rgb * intensity;");
        append_line(buf, &len, "    texel.rgb = lerp(texel.rgb, new_texel, input.grayscale.a);");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        // Noise requires a VPOS pixel-coordinate input on ps_3_0.
        append_line(buf, &len, "    float2 coords = vpos.xy * noise_scale;");
        append_line(buf, &len, "    texel.a *= round(saturate(random(float3(floor(coords), noise_frame)) + texel.a - 0.5));");
    }

    if (cc_features.opt_alpha) {
        // opt_alpha_threshold is implemented by gfx_xdk.cpp with
        // D3DRS_ALPHATESTENABLE / ALPHAREF / ALPHAFUNC.
        if (cc_features.opt_invisible) {
            append_line(buf, &len, "    texel.a = 0.0;");
        }
        append_line(buf, &len, "    return texel;");
    } else {
        append_line(buf, &len, "    return float4(texel, 1.0);");
    }
    append_line(buf, &len, "}");

    // Known limitation: opt_noise still requires VPOS to be threaded into
    // PSMain, so noise-enabled variants are unsupported in this snapshot.
}