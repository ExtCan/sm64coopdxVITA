#ifdef TARGET_VITA

// PS Vita rendering backend.
//
// This is gfx_opengl.c ported to vitaGL. vitaGL exposes an OpenGL-like API on top of GXM, and
// compiles shaders on the console itself (libshacccg.suprx). GXM's native shading language is
// Cg, so the color combiner shaders are generated as Cg here instead of GLSL.
//
// Differences from the desktop backend, all for the sake of the SGX543:
//  - three point filtering is not emulated in the fragment shader, it falls back to bilinear
//  - the "world geometry" post effects (hue, saturation, ...) are compiled into a second
//    program variant that is only used while a mod actually enables one of them
//  - alpha dithering/noise is evaluated at 320x240 like on the N64 instead of per pixel
//  - oversized textures are downscaled before upload to save VRAM

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <vitaGL.h>

#include "pc/platform.h"
#include "pc/configfile.h"
#include "pc/gfx/gfx_cc.h"
#include "pc/gfx/gfx_rendering_api.h"
#include "pc/gfx/gfx_pc.h"
#include "vita_platform.h"

#define TEX_CACHE_STEP 512
#define MAX_ATTRIBS 13          // pos, 2x texcoord, fog, lightmap, 8 inputs
#define VITA_MAX_TEXTURE_DIM 1024

enum {
    VARIANT_PLAIN,   // no post effects
    VARIANT_FX,      // world geometry post effects enabled
    VARIANT_COUNT
};

struct VitaProgram {
    bool compiled;
    GLuint program_id;
    GLint attrib_locations[MAX_ATTRIBS];
    GLint u_frame_count;
    GLint u_lightmap_color;
    GLint u_shader_flags;
    GLint u_shader_flag_values;
};

struct ShaderProgram {
    uint64_t hash;
    struct ColorCombiner cc;    // kept around so variants can be compiled lazily
    struct CCFeatures ccf;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    uint8_t attrib_sizes[MAX_ATTRIBS];
    uint8_t num_attribs;
    bool used_noise;
    bool used_lightmap;
    bool world_geometry;
    struct VitaProgram variants[VARIANT_COUNT];
};

struct GLTexture {
    GLuint gltex;
    bool filter;
};

static struct ShaderProgram shader_program_pool[CC_MAX_SHADERS];
static uint8_t shader_program_pool_size = 0;
static uint8_t shader_program_pool_index = 0;
static GLuint opengl_vbo;

static int tex_cache_size = 0;
static int num_textures = 0;
static struct GLTexture *tex_cache = NULL;

static struct ShaderProgram *opengl_prg = NULL;
static int opengl_prg_variant = VARIANT_PLAIN;
static struct GLTexture *opengl_tex[2];
static int opengl_curtex = 0;

static uint32_t frame_count;
static bool sFxActive = false;

static bool gfx_vitagl_z_is_from_0_to_1(void) {
    return false;
}

  ///////////////////////
 // shader generation //
///////////////////////

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "float4(1.0, 1.0, 1.0, 1.0)" : "float3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_INPUT_5:
                return with_alpha || !inputs_have_alpha ? "vInput5" : "vInput5.rgb";
            case SHADER_INPUT_6:
                return with_alpha || !inputs_have_alpha ? "vInput6" : "vInput6.rgb";
            case SHADER_INPUT_7:
                return with_alpha || !inputs_have_alpha ? "vInput7" : "vInput7.rgb";
            case SHADER_INPUT_8:
                return with_alpha || !inputs_have_alpha ? "vInput8" : "vInput8.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)" : "float3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a" :
                    (with_alpha ? "float4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)" : "float3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_COMBINEDA:
                return hint_single_element ? "texel.a" :
                    (with_alpha ? "float4(texel.a, texel.a, texel.a, texel.a)" : "float3(texel.a, texel.a, texel.a)");
            case SHADER_NOISE:
                return hint_single_element ? "noise" :
                    (with_alpha ? "float4(noise, noise, noise, noise)" : "float3(noise, noise, noise)");
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_INPUT_5:
                return "vInput5.a";
            case SHADER_INPUT_6:
                return "vInput6.a";
            case SHADER_INPUT_7:
                return "vInput7.a";
            case SHADER_INPUT_8:
                return "vInput8.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_COMBINEDA:
                return "texel.a";
            case SHADER_NOISE:
                return "noise";
        }
    }
    return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : (only_alpha ? "0.0" : "float3(0.0, 0.0, 0.0)");
}

static void append_formula(char *buf, size_t *len, uint8_t* cmd, bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "lerp(");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 3], with_alpha, only_alpha, opt_alpha, false));
    }
}

// Interpolators available to a GXM fragment program: TEXCOORD0-9 and COLOR0-1.
// TEXCOORD0 = both texture coordinates, TEXCOORD1 = fog, TEXCOORD2 = lightmap coordinates,
// TEXCOORD3-9 = inputs 1-7, COLOR0 = input 8.
static const char *input_semantic(int i) {
    static const char *sSemantics[8] = {
        "TEXCOORD3", "TEXCOORD4", "TEXCOORD5", "TEXCOORD6", "TEXCOORD7", "TEXCOORD8", "TEXCOORD9", "COLOR0",
    };
    return sSemantics[i & 7];
}

static void gfx_vitagl_generate_shaders(struct ShaderProgram *prg, int variant, char *vs_buf, size_t *out_vs_len, char *fs_buf, size_t *out_fs_len) {
    struct ColorCombiner *cc = &prg->cc;
    struct CCFeatures *ccf = &prg->ccf;

    bool opt_alpha = cc->cm.use_alpha;
    bool opt_fog = cc->cm.use_fog;
    bool opt_texture_edge = cc->cm.texture_edge;
    bool opt_2cycle = cc->cm.use_2cycle;
    bool opt_light_map = cc->cm.light_map;
    bool opt_dither = cc->cm.use_dither;
    bool opt_fx = (variant == VARIANT_FX);
    bool opt_noise = (opt_alpha && opt_dither) || ccf->do_noise;
    bool any_tex = ccf->used_textures[0] || ccf->used_textures[1];
    int input_size = opt_alpha ? 4 : 3;

    size_t vs_len = 0;
    size_t fs_len = 0;

      ///////////////////
     // vertex shader //
    ///////////////////

    append_line(vs_buf, &vs_len, "void main(");
    append_line(vs_buf, &vs_len, "float4 aVtxPos,");
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            vs_len += sprintf(vs_buf + vs_len, "float2 aTexCoord%d,\n", t);
        }
    }
    if (opt_fog)       { append_line(vs_buf, &vs_len, "float4 aFog,"); }
    if (opt_light_map) { append_line(vs_buf, &vs_len, "float2 aLightMap,"); }
    for (int i = 0; i < ccf->num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "float%d aInput%d,\n", input_size, i + 1);
    }
    if (any_tex)       { append_line(vs_buf, &vs_len, "out float4 vTexCoord : TEXCOORD0,"); }
    if (opt_fog)       { append_line(vs_buf, &vs_len, "out float4 vFog : TEXCOORD1,"); }
    if (opt_light_map) { append_line(vs_buf, &vs_len, "out float2 vLightMap : TEXCOORD2,"); }
    for (int i = 0; i < ccf->num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "out float%d vInput%d : %s,\n", input_size, i + 1, input_semantic(i));
    }
    append_line(vs_buf, &vs_len, "out float4 vPosition : POSITION");
    append_line(vs_buf, &vs_len, ") {");
    if (any_tex) {
        vs_len += sprintf(vs_buf + vs_len, "vTexCoord = float4(%s, %s);\n",
            ccf->used_textures[0] ? "aTexCoord0" : "float2(0.0, 0.0)",
            ccf->used_textures[1] ? "aTexCoord1" : "float2(0.0, 0.0)");
    }
    if (opt_fog)       { append_line(vs_buf, &vs_len, "vFog = aFog;"); }
    if (opt_light_map) { append_line(vs_buf, &vs_len, "vLightMap = aLightMap;"); }
    for (int i = 0; i < ccf->num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    append_line(vs_buf, &vs_len, "vPosition = aVtxPos;");
    append_line(vs_buf, &vs_len, "}");

      /////////////////////
     // fragment shader //
    /////////////////////

    if (opt_fx) {
        append_line(fs_buf, &fs_len, "float dither4x4(float2 position, float brightness) {");
        append_line(fs_buf, &fs_len, "    float x = floor(fmod(position.x, 4.0));");
        append_line(fs_buf, &fs_len, "    float y = floor(fmod(position.y, 4.0));");
        append_line(fs_buf, &fs_len, "    float index = x + y * 4.0;");
        append_line(fs_buf, &fs_len, "    float limit = 0.0;");
        append_line(fs_buf, &fs_len, "    if (index < 0.5) limit = 0.0625;");
        append_line(fs_buf, &fs_len, "    else if (index < 1.5) limit = 0.5625;");
        append_line(fs_buf, &fs_len, "    else if (index < 2.5) limit = 0.1875;");
        append_line(fs_buf, &fs_len, "    else if (index < 3.5) limit = 0.6875;");
        append_line(fs_buf, &fs_len, "    else if (index < 4.5) limit = 0.8125;");
        append_line(fs_buf, &fs_len, "    else if (index < 5.5) limit = 0.3125;");
        append_line(fs_buf, &fs_len, "    else if (index < 6.5) limit = 0.9375;");
        append_line(fs_buf, &fs_len, "    else if (index < 7.5) limit = 0.4375;");
        append_line(fs_buf, &fs_len, "    else if (index < 8.5) limit = 0.25;");
        append_line(fs_buf, &fs_len, "    else if (index < 9.5) limit = 0.75;");
        append_line(fs_buf, &fs_len, "    else if (index < 10.5) limit = 0.125;");
        append_line(fs_buf, &fs_len, "    else if (index < 11.5) limit = 0.625;");
        append_line(fs_buf, &fs_len, "    else if (index < 12.5) limit = 1.0;");
        append_line(fs_buf, &fs_len, "    else if (index < 13.5) limit = 0.5;");
        append_line(fs_buf, &fs_len, "    else if (index < 14.5) limit = 0.875;");
        append_line(fs_buf, &fs_len, "    else limit = 0.375;");
        append_line(fs_buf, &fs_len, "    return brightness < limit ? 0.0 : 1.0;");
        append_line(fs_buf, &fs_len, "}");

        append_line(fs_buf, &fs_len, "float3 rgb2hsv(float3 c) {");
        append_line(fs_buf, &fs_len, "    float4 K = float4(0.0, -1.0/3.0, 2.0/3.0, -1.0);");
        append_line(fs_buf, &fs_len, "    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));");
        append_line(fs_buf, &fs_len, "    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));");
        append_line(fs_buf, &fs_len, "    float d = q.x - min(q.w, q.y);");
        append_line(fs_buf, &fs_len, "    float e = 1.0e-10;");
        append_line(fs_buf, &fs_len, "    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);");
        append_line(fs_buf, &fs_len, "}");

        append_line(fs_buf, &fs_len, "float3 hsv2rgb(float3 c) {");
        append_line(fs_buf, &fs_len, "    float3 p = abs(frac(c.xxx + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);");
        append_line(fs_buf, &fs_len, "    return c.z * lerp(float3(1.0, 1.0, 1.0), clamp(p - 1.0, 0.0, 1.0), c.y);");
        append_line(fs_buf, &fs_len, "}");
    }

    if (opt_noise) {
        append_line(fs_buf, &fs_len, "float random(float3 value) {");
        append_line(fs_buf, &fs_len, "    float r = dot(sin(value), float3(12.9898, 78.233, 37.719));");
        append_line(fs_buf, &fs_len, "    return frac(sin(r) * 143758.5453);");
        append_line(fs_buf, &fs_len, "}");
    }

    // every parameter line starts with a comma so that optional ones can simply be skipped
    append_line(fs_buf, &fs_len, "float4 main(");
    append_line(fs_buf, &fs_len, "float4 vFragCoord : WPOS");
    if (any_tex)       { append_line(fs_buf, &fs_len, ", float4 vTexCoord : TEXCOORD0"); }
    if (opt_fog)       { append_line(fs_buf, &fs_len, ", float4 vFog : TEXCOORD1"); }
    if (opt_light_map) { append_line(fs_buf, &fs_len, ", float2 vLightMap : TEXCOORD2"); }
    for (int i = 0; i < ccf->num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, ", float%d vInput%d : %s\n", input_size, i + 1, input_semantic(i));
    }
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            fs_len += sprintf(fs_buf + fs_len, ", uniform sampler2D uTex%d : TEXUNIT%d\n", t, t);
        }
    }
    if (opt_noise)     { append_line(fs_buf, &fs_len, ", uniform float uFrameCount"); }
    if (opt_light_map) { append_line(fs_buf, &fs_len, ", uniform float3 uLightmapColor"); }
    if (opt_fx) {
        fs_len += sprintf(fs_buf + fs_len, ", uniform float uShaderFlags[%d]\n", SHADER_FLAG_MAX);
        fs_len += sprintf(fs_buf + fs_len, ", uniform float uShaderFlagValues[%d]\n", SHADER_FLAG_MAX);
    }
    append_line(fs_buf, &fs_len, ") : COLOR {");

    if (opt_noise) {
        // 544 lines / 240 lines: one noise cell per N64 pixel
        append_line(fs_buf, &fs_len, "float noise = floor(random(float3(floor(vFragCoord.xy * (240.0 / 544.0)), uFrameCount)) + 0.5);");
    }

    if (ccf->used_textures[0]) {
        append_line(fs_buf, &fs_len, "float4 texVal0 = tex2D(uTex0, vTexCoord.xy);");
    }
    if (ccf->used_textures[1]) {
        if (opt_light_map) {
            append_line(fs_buf, &fs_len, "float4 texVal1 = tex2D(uTex1, vLightMap);");
            if (ccf->used_textures[0]) {
                append_line(fs_buf, &fs_len, "texVal0.rgb *= uLightmapColor.rgb;");
            }
            append_line(fs_buf, &fs_len, "texVal1.rgb = texVal1.rgb * texVal1.rgb + texVal1.rgb;");
        } else {
            append_line(fs_buf, &fs_len, "float4 texVal1 = tex2D(uTex1, vTexCoord.zw);");
        }
    }

    append_str(fs_buf, &fs_len, (opt_alpha) ? "float4 texel = " : "float3 texel = ");
    for (int i = 0; i < (opt_2cycle + 1); i++) {
        uint8_t *cmd = &cc->shader_commands[i * 8];
        if (!ccf->color_alpha_same[i] && opt_alpha) {
            append_str(fs_buf, &fs_len, "float4(");
            append_formula(fs_buf, &fs_len, cmd, ccf->do_single[i*2+0], ccf->do_multiply[i*2+0], ccf->do_mix[i*2+0], false, false, true);
            append_str(fs_buf, &fs_len, ", ");
            append_formula(fs_buf, &fs_len, cmd, ccf->do_single[i*2+1], ccf->do_multiply[i*2+1], ccf->do_mix[i*2+1], true, true, true);
            append_str(fs_buf, &fs_len, ")");
        } else {
            append_formula(fs_buf, &fs_len, cmd, ccf->do_single[i*2+0], ccf->do_multiply[i*2+0], ccf->do_mix[i*2+0], opt_alpha, false, opt_alpha);
        }
        append_line(fs_buf, &fs_len, ";");

        if (i == 0 && opt_2cycle) {
            append_str(fs_buf, &fs_len, "texel = ");
        }
    }

    if (opt_texture_edge && opt_alpha) {
        append_line(fs_buf, &fs_len, "if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }

    if (opt_fx) {
        // hue
        append_line(fs_buf, &fs_len, "if (uShaderFlags[0] > 0.5) {");
        append_line(fs_buf, &fs_len, "float3 hsv = rgb2hsv(texel.rgb);");
        append_line(fs_buf, &fs_len, "hsv.x = frac(hsv.x + uShaderFlagValues[0]);");
        append_line(fs_buf, &fs_len, "texel.rgb = hsv2rgb(hsv);");
        append_line(fs_buf, &fs_len, "}");

        // saturation
        append_line(fs_buf, &fs_len, "if (uShaderFlags[1] > 0.5) {");
        append_line(fs_buf, &fs_len, "float intensity = dot(texel.rgb, float3(0.2125, 0.7154, 0.0721));");
        append_line(fs_buf, &fs_len, "texel.rgb = lerp(float3(intensity, intensity, intensity), texel.rgb, uShaderFlagValues[1]);");
        append_line(fs_buf, &fs_len, "}");

        // brightness
        append_line(fs_buf, &fs_len, "if (uShaderFlags[2] > 0.5) {");
        append_line(fs_buf, &fs_len, "texel.rgb *= uShaderFlagValues[2];");
        append_line(fs_buf, &fs_len, "}");

        // contrast
        append_line(fs_buf, &fs_len, "if (uShaderFlags[3] > 0.5) {");
        append_line(fs_buf, &fs_len, "texel.rgb = 0.5 + uShaderFlagValues[3] * (texel.rgb - 0.5);");
        append_line(fs_buf, &fs_len, "}");

        // exposure
        append_line(fs_buf, &fs_len, "if (uShaderFlags[4] > 0.5) {");
        append_line(fs_buf, &fs_len, "texel.rgb = texel.rgb + (uShaderFlagValues[4] - 2.0) * texel.rgb + texel.rgb;");
        append_line(fs_buf, &fs_len, "}");

        // dithering
        append_line(fs_buf, &fs_len, "if (uShaderFlags[5] > 0.5) {");
        append_line(fs_buf, &fs_len, "texel.rgb *= dither4x4(vFragCoord.xy, dot(texel.rgb, float3(0.299, 0.587, 0.114)));");
        append_line(fs_buf, &fs_len, "}");

        // posterization
        append_line(fs_buf, &fs_len, "if (uShaderFlags[6] > 0.5) {");
        append_line(fs_buf, &fs_len, "float levels = floor(max(1.0, uShaderFlagValues[6]));");
        append_line(fs_buf, &fs_len, "texel.rgb = floor(texel.rgb * levels) / levels;");
        append_line(fs_buf, &fs_len, "}");

        // scan lines
        append_line(fs_buf, &fs_len, "if (uShaderFlags[7] > 0.5) {");
        append_line(fs_buf, &fs_len, "float scan = sin(vFragCoord.y * 1.5) * 0.04;");
        append_line(fs_buf, &fs_len, "texel.rgb -= scan * uShaderFlagValues[7];");
        append_line(fs_buf, &fs_len, "}");
    }

    if (opt_fog) {
        if (opt_alpha) {
            append_line(fs_buf, &fs_len, "texel = float4(lerp(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "texel = lerp(texel, vFog.rgb, vFog.a);");
        }
    }

    if (opt_alpha && opt_dither) {
        append_line(fs_buf, &fs_len, "texel.a *= noise;");
    }

    if (opt_alpha) {
        append_line(fs_buf, &fs_len, "return texel;");
    } else {
        append_line(fs_buf, &fs_len, "return float4(texel, 1.0);");
    }
    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';
    *out_vs_len = vs_len;
    *out_fs_len = fs_len;
}

static GLuint gfx_vitagl_compile(GLenum type, const char *source, size_t length) {
    const GLchar *sources[1] = { source };
    const GLint lengths[1] = { (GLint)length };
    GLint success = GL_FALSE;

    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, sources, lengths);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success && vita_debug_no_shaders()) {
        // debugging aid for emulators without the shader compiler module: render nothing
        glDeleteShader(shader);
        return 0;
    }
    if (!success) {
        char error_log[1024] = "";
        GLsizei log_length = 0;
        glGetShaderInfoLog(shader, sizeof(error_log) - 1, &log_length, error_log);
        const char *kind = (type == GL_CG_VERTEX_SHADER_EXT) ? "Vertex" : "Fragment";
        printf("%s shader compilation failed:\n%s\n---- source ----\n%s\n----------------\n", kind, error_log, source);
        fflush(stdout);
        if (error_log[0] == '\0') {
            // no diagnostics at all: the compiler did not even run
            sys_fatal("The shader compiler could not be started.\n\n"
                      "Make sure libshacccg.suprx is installed in ur0:data/ .");
        }
        // show the beginning of the compiler output, the rest is in the log file
        error_log[160] = '\0';
        sys_fatal("%s shader compilation failed:\n%s\n\n"
                  "Details were written to " VITA_USER_PATH "/" VITA_LOG_FILE, kind, error_log);
    }
    return shader;
}

static void gfx_vitagl_compile_variant(struct ShaderProgram *prg, int variant) {
    static char vs_buf[4096];
    static char fs_buf[8192];
    size_t vs_len = 0;
    size_t fs_len = 0;

    struct VitaProgram *vp = &prg->variants[variant];
    unsigned long long buildStart = vita_perf_now();
    gfx_vitagl_generate_shaders(prg, variant, vs_buf, &vs_len, fs_buf, &fs_len);

    if (vita_debug_no_shaders()) {
        // debugging aid: leave the generated sources in the log so they can be reviewed offline
        printf("---- shader %016llx variant %d: vertex ----\n%s---- fragment ----\n%s----\n",
            (unsigned long long) prg->hash, variant, vs_buf, fs_buf);
    }

    GLuint vertex_shader = gfx_vitagl_compile(GL_CG_VERTEX_SHADER_EXT, vs_buf, vs_len);
    GLuint fragment_shader = (vertex_shader != 0) ? gfx_vitagl_compile(GL_CG_FRAGMENT_SHADER_EXT, fs_buf, fs_len) : 0;
    if (vertex_shader == 0 || fragment_shader == 0) {
        // only reachable with vita_debug_no_shaders(): leave the variant without a program
        for (int i = 0; i < MAX_ATTRIBS; i++) { vp->attrib_locations[i] = -1; }
        vp->u_frame_count = vp->u_lightmap_color = vp->u_shader_flags = vp->u_shader_flag_values = -1;
        vp->program_id = 0;
        vp->compiled = true;
        return;
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    struct CCFeatures *ccf = &prg->ccf;
    size_t cnt = 0;
    char name[32];

    vp->attrib_locations[cnt++] = glGetAttribLocation(shader_program, "aVtxPos");
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            snprintf(name, sizeof(name), "aTexCoord%d", t);
            vp->attrib_locations[cnt++] = glGetAttribLocation(shader_program, name);
        }
    }
    if (prg->cc.cm.use_fog)   { vp->attrib_locations[cnt++] = glGetAttribLocation(shader_program, "aFog"); }
    if (prg->cc.cm.light_map) { vp->attrib_locations[cnt++] = glGetAttribLocation(shader_program, "aLightMap"); }
    for (int i = 0; i < ccf->num_inputs; i++) {
        snprintf(name, sizeof(name), "aInput%d", i + 1);
        vp->attrib_locations[cnt++] = glGetAttribLocation(shader_program, name);
    }

    glUseProgram(shader_program);
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            snprintf(name, sizeof(name), "uTex%d", t);
            glUniform1i(glGetUniformLocation(shader_program, name), t);
        }
    }

    vp->u_frame_count        = prg->used_noise    ? glGetUniformLocation(shader_program, "uFrameCount")    : -1;
    vp->u_lightmap_color     = prg->used_lightmap ? glGetUniformLocation(shader_program, "uLightmapColor") : -1;
    vp->u_shader_flags       = (variant == VARIANT_FX) ? glGetUniformLocation(shader_program, "uShaderFlags")      : -1;
    vp->u_shader_flag_values = (variant == VARIANT_FX) ? glGetUniformLocation(shader_program, "uShaderFlagValues") : -1;

    vp->program_id = shader_program;
    vp->compiled = true;
    vita_perf_add(VITA_PERF_SHADER_BUILD, vita_perf_now() - buildStart);
    vita_perf_add(VITA_PERF_SHADER_BUILDS, 1);
}

  /////////////////////
 // shader handling //
/////////////////////

static inline int gfx_vitagl_wanted_variant(struct ShaderProgram *prg) {
    return (prg->world_geometry && sFxActive) ? VARIANT_FX : VARIANT_PLAIN;
}

static void gfx_vitagl_vertex_array_set_attribs(struct ShaderProgram *prg, struct VitaProgram *vp) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        if (vp->attrib_locations[i] >= 0) {
            glEnableVertexAttribArray(vp->attrib_locations[i]);
            glVertexAttribPointer(vp->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE, num_floats * sizeof(float), (void *) (pos * sizeof(float)));
        }
        pos += prg->attrib_sizes[i];
    }
}

static inline void gfx_vitagl_set_shader_uniforms(struct ShaderProgram *prg, struct VitaProgram *vp) {
    if (prg->used_noise) {
        glUniform1f(vp->u_frame_count, (float)(frame_count & 0xFF));
    }
    if (prg->used_lightmap) {
        glUniform3f(vp->u_lightmap_color, gVertexColor[0] / 255.0f, gVertexColor[1] / 255.0f, gVertexColor[2] / 255.0f);
    }
    if (vp->u_shader_flags != -1) {
        float flags[SHADER_FLAG_MAX];
        for (int i = 0; i < SHADER_FLAG_MAX; i++) { flags[i] = gShaderFlags[i] ? 1.0f : 0.0f; }
        glUniform1fv(vp->u_shader_flags, SHADER_FLAG_MAX, flags);
        glUniform1fv(vp->u_shader_flag_values, SHADER_FLAG_MAX, gShaderFlagValues);
    }
}

static void gfx_vitagl_unload_shader(struct ShaderProgram *old_prg) {
    if (old_prg != NULL) {
        struct VitaProgram *vp = &old_prg->variants[(old_prg == opengl_prg) ? opengl_prg_variant : VARIANT_PLAIN];
        if (vp->compiled) {
            for (int i = 0; i < old_prg->num_attribs; i++) {
                if (vp->attrib_locations[i] >= 0) { glDisableVertexAttribArray(vp->attrib_locations[i]); }
            }
        }
        if (old_prg == opengl_prg) {
            opengl_prg = NULL;
        }
    } else {
        opengl_prg = NULL;
    }
}

static void gfx_vitagl_load_shader(struct ShaderProgram *new_prg) {
    int variant = gfx_vitagl_wanted_variant(new_prg);
    struct VitaProgram *vp = &new_prg->variants[variant];
    if (!vp->compiled) {
        gfx_vitagl_compile_variant(new_prg, variant);
    }

    opengl_prg = new_prg;
    opengl_prg_variant = variant;
    if (vp->program_id == 0) { return; }
    glUseProgram(vp->program_id);
    gfx_vitagl_vertex_array_set_attribs(new_prg, vp);
    gfx_vitagl_set_shader_uniforms(new_prg, vp);
}

static struct ShaderProgram *gfx_vitagl_create_and_load_new_shader(struct ColorCombiner* cc) {
    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_index];
    shader_program_pool_index = (shader_program_pool_index + 1) % CC_MAX_SHADERS;
    if (shader_program_pool_size < CC_MAX_SHADERS) { shader_program_pool_size++; }

    // this slot is being recycled, release its GPU programs
    for (int v = 0; v < VARIANT_COUNT; v++) {
        if (prg->variants[v].compiled && prg->variants[v].program_id != 0) {
            glDeleteProgram(prg->variants[v].program_id);
        }
    }
    if (prg == opengl_prg) { opengl_prg = NULL; }
    memset(prg, 0, sizeof(*prg));

    prg->cc = *cc;
    gfx_cc_get_features(cc, &prg->ccf);
    struct CCFeatures *ccf = &prg->ccf;

    bool opt_alpha = cc->cm.use_alpha;

    size_t cnt = 0;
    size_t num_floats = 4;
    prg->attrib_sizes[cnt++] = 4;
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            prg->attrib_sizes[cnt++] = 2;
            num_floats += 2;
        }
    }
    if (cc->cm.use_fog) {
        prg->attrib_sizes[cnt++] = 4;
        num_floats += 4;
    }
    if (cc->cm.light_map) {
        prg->attrib_sizes[cnt++] = 2;
        num_floats += 2;
    }
    for (int i = 0; i < ccf->num_inputs; i++) {
        prg->attrib_sizes[cnt++] = opt_alpha ? 4 : 3;
        num_floats += opt_alpha ? 4 : 3;
    }

    prg->hash = cc->hash;
    prg->num_inputs = ccf->num_inputs;
    prg->used_textures[0] = ccf->used_textures[0];
    prg->used_textures[1] = ccf->used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;
    prg->used_noise = (opt_alpha && cc->cm.use_dither) || ccf->do_noise;
    prg->used_lightmap = cc->cm.light_map;
    prg->world_geometry = cc->cm.world_geometry;

    gfx_vitagl_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_vitagl_lookup_shader(struct ColorCombiner* cc) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].hash == cc->hash) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_vitagl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

  //////////////
 // textures //
//////////////

static GLuint gfx_vitagl_new_texture(void) {
    if (num_textures >= tex_cache_size) {
        tex_cache_size += TEX_CACHE_STEP;
        tex_cache = realloc(tex_cache, sizeof(struct GLTexture) * tex_cache_size);
        if (!tex_cache) sys_fatal("out of memory allocating texture cache");
        // invalidate these because they might be pointing to garbage now
        opengl_tex[0] = NULL;
        opengl_tex[1] = NULL;
    }
    glGenTextures(1, &tex_cache[num_textures].gltex);
    tex_cache[num_textures].filter = false;
    return num_textures++;
}

static void gfx_vitagl_select_texture(int tile, GLuint texture_id) {
    opengl_tex[tile] = tex_cache + texture_id;
    opengl_curtex = tile;
    glActiveTexture(GL_TEXTURE0 + tile);
    glBindTexture(GL_TEXTURE_2D, opengl_tex[tile]->gltex);
}

// Halves a texture with a 2x2 box filter. Used for the few huge UI textures (the 2048x1024 logo).
static uint8_t *gfx_vitagl_halve_texture(const uint8_t *src, int width, int height, int *out_width, int *out_height) {
    int nw = (width  > 1) ? width  / 2 : 1;
    int nh = (height > 1) ? height / 2 : 1;
    uint8_t *dst = malloc((size_t)nw * nh * 4);
    if (!dst) { return NULL; }

    for (int y = 0; y < nh; y++) {
        int y0 = (y * 2 < height) ? y * 2 : height - 1;
        int y1 = (y * 2 + 1 < height) ? y * 2 + 1 : height - 1;
        for (int x = 0; x < nw; x++) {
            int x0 = (x * 2 < width) ? x * 2 : width - 1;
            int x1 = (x * 2 + 1 < width) ? x * 2 + 1 : width - 1;
            const uint8_t *p00 = &src[(y0 * width + x0) * 4];
            const uint8_t *p01 = &src[(y0 * width + x1) * 4];
            const uint8_t *p10 = &src[(y1 * width + x0) * 4];
            const uint8_t *p11 = &src[(y1 * width + x1) * 4];
            uint8_t *out = &dst[(y * nw + x) * 4];
            for (int c = 0; c < 4; c++) {
                out[c] = (uint8_t)((p00[c] + p01[c] + p10[c] + p11[c] + 2) >> 2);
            }
        }
    }

    *out_width = nw;
    *out_height = nh;
    return dst;
}

static void gfx_vitagl_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    unsigned long long start = vita_perf_now();
    uint8_t *scaled = NULL;
    while (width > VITA_MAX_TEXTURE_DIM || height > VITA_MAX_TEXTURE_DIM) {
        int nw, nh;
        uint8_t *next = gfx_vitagl_halve_texture(rgba32_buf, width, height, &nw, &nh);
        if (!next) { break; }
        free(scaled);
        scaled = next;
        rgba32_buf = scaled;
        width = nw;
        height = nh;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    free(scaled);
    vita_perf_add(VITA_PERF_TEX_UPLOAD, vita_perf_now() - start);
    vita_perf_add(VITA_PERF_TEX_UPLOADS, 1);
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_vitagl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const GLenum filter = linear_filter ? GL_LINEAR : GL_NEAREST;
    glActiveTexture(GL_TEXTURE0 + tile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
    opengl_curtex = tile;
    if (opengl_tex[tile]) {
        opengl_tex[tile]->filter = linear_filter;
    }
}

  ///////////////////
 // render states //
///////////////////

static void gfx_vitagl_set_depth_test(bool depth_test) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_vitagl_set_depth_mask(bool z_upd) {
    glDepthMask(z_upd ? GL_TRUE : GL_FALSE);
}

static void gfx_vitagl_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        glPolygonOffset(-2, -2);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

static void gfx_vitagl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

static void gfx_vitagl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_vitagl_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static void gfx_vitagl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!opengl_prg) { return; }

    // a mod toggled the post effects while this shader was loaded
    if (gfx_vitagl_wanted_variant(opengl_prg) != opengl_prg_variant) {
        struct ShaderProgram *prg = opengl_prg;
        gfx_vitagl_unload_shader(prg);
        gfx_vitagl_load_shader(prg);
    }

    if (opengl_prg->variants[opengl_prg_variant].program_id == 0) { return; }

    unsigned long long start = vita_perf_now();
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
    vita_perf_add(VITA_PERF_DRAW, vita_perf_now() - start);
    vita_perf_add(VITA_PERF_DRAW_CALLS, 1);
    vita_perf_add(VITA_PERF_TRIANGLES, buf_vbo_num_tris);
}

static void gfx_vitagl_init(void) {
    tex_cache_size = TEX_CACHE_STEP;
    tex_cache = calloc(tex_cache_size, sizeof(struct GLTexture));
    if (!tex_cache) sys_fatal("out of memory allocating texture cache");

    glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);

    glDisable(GL_CULL_FACE);
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gfx_vitagl_on_resize(void) {
}

static void gfx_vitagl_start_frame(void) {
    frame_count++;

    sFxActive = false;
    for (int i = 0; i < SHADER_FLAG_MAX; i++) {
        if (gShaderFlags[i]) { sFxActive = true; break; }
    }

    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE); // Must be set to clear Z-buffer
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

static void gfx_vitagl_end_frame(void) {
}

static void gfx_vitagl_finish_render(void) {
}

static const char* gfx_vitagl_get_name(void) {
    return "vitaGL";
}

static void gfx_vitagl_shutdown(void) {
}

struct GfxRenderingAPI gfx_vitagl_api = {
    gfx_vitagl_z_is_from_0_to_1,
    gfx_vitagl_unload_shader,
    gfx_vitagl_load_shader,
    gfx_vitagl_create_and_load_new_shader,
    gfx_vitagl_lookup_shader,
    gfx_vitagl_shader_get_info,
    gfx_vitagl_new_texture,
    gfx_vitagl_select_texture,
    gfx_vitagl_upload_texture,
    gfx_vitagl_set_sampler_parameters,
    gfx_vitagl_set_depth_test,
    gfx_vitagl_set_depth_mask,
    gfx_vitagl_set_zmode_decal,
    gfx_vitagl_set_viewport,
    gfx_vitagl_set_scissor,
    gfx_vitagl_set_use_alpha,
    gfx_vitagl_draw_triangles,
    gfx_vitagl_init,
    gfx_vitagl_on_resize,
    gfx_vitagl_start_frame,
    gfx_vitagl_end_frame,
    gfx_vitagl_finish_render,
    gfx_vitagl_get_name,
    gfx_vitagl_shutdown
};

#endif // TARGET_VITA
