/*
 * Copyright 2026 Behdad Esfahbod. All Rights Reserved.
 */

#include "demo-shader.h"

static const char *demo_vertex_glsl = R"glsl(
uniform mat4 u_matViewProjection;
uniform vec2 u_viewport;

in vec2 a_position;
in vec2 a_texcoord;
in vec2 a_normal;
in vec4 a_color;
in float a_emPerPos;
in uint a_dataOffset;
in uint a_gradientDataOffset;
in uint a_isGroup;

out vec2 v_texcoord;
flat out vec4 v_color;
flat out uint v_dataOffset;
flat out uint v_gradientDataOffset;
flat out uint v_isGroup;

void main ()
{
  vec2 pos = a_position;
  vec2 tex = a_texcoord;

  vec4 jac = vec4 (a_emPerPos, 0.0, 0.0, -a_emPerPos);

  hb_gpu_dilate (pos, tex, a_normal, jac,
		 u_matViewProjection, u_viewport);

  gl_Position = u_matViewProjection * vec4 (pos, 0.0, 1.0);
  v_texcoord = tex;
  v_color = a_color;
  v_dataOffset = a_dataOffset;
  v_gradientDataOffset = a_gradientDataOffset;
  v_isGroup = a_isGroup;
}
)glsl";

static const char *demo_fragment_glsl = R"glsl(
uniform float u_gamma;
uniform float u_debug;
uniform float u_stem_darkening;
uniform vec4 u_foreground;

in vec2 v_texcoord;
flat in vec4 v_color;
flat in uint v_dataOffset;
flat in uint v_gradientDataOffset;
flat in uint v_isGroup;

out vec4 fragColor;

vec4 getGradientColor(int dataOffset, vec4 defaultColor, float t) {
	int cursor = dataOffset;
	vec4 previousColor = vec4(0., 0., 0., 1.);
	float previousPosition = -1.;
	vec4 color = vec4(0., 0., 0., 1.);
	for (int i = 0; i < 16; ++i) {
		ivec4 stop = hb_gpu_fetch(cursor);
		++cursor;
		uint rg = uint(stop.z) & 0xFFFFu;
		uint ba = uint(stop.w) & 0xFFFFu;
		color = (stop.y & 1) == 1 ? defaultColor : vec4(
			float(rg >> 8) / 255.,
			float(rg & 0xFFu) / 255.,
			float(ba >> 8) / 255.,
			float(ba & 0xFFu) / 255.
		);
		if (i == 0) previousColor = color;
		float position = float(stop.x) / 256.f;
		if (t < position) {
			float p = (t - previousPosition) / (position - previousPosition);
			return (1. - p) * previousColor + p * color;
		}
		previousPosition = position;
		previousColor = color;
		if ((stop.y & 2) == 2) break;
	}
	return color;
}

float evaluateLinearGradient(ivec4 gradient, vec2 c) {
	vec2 a = vec2(gradient.xy);
	vec2 b = vec2(gradient.zw);
	vec2 ab = b - a;
	vec2 ac = c - a;
	return dot(ab, ac) / dot(ab, ab);
}

float evaluateRadialGradient(ivec4 gradient1, ivec4 gradient2, vec2 uv) {
	vec2 x0y0 = vec2(gradient1.zw);
	vec2 x1y1 = vec2(gradient2.xy);
	float r0 = float(gradient2.z);
	float r1 = float(gradient2.w);
	vec2 dxdy = x1y1 - x0y0;
	float dr = r1 - r0;
	vec2 p2 = uv - x0y0;
	float A = dxdy.x * dxdy.x + dxdy.y * dxdy.y - dr * dr;
	float B = dot(p2, dxdy) + r0 * dr;
	float C = dot(p2, p2) - r0 * r0;
	float t;
	if (abs(A) > 0.0000001) {
		float D = (B * B) - (A * C);
		if (D < 0.0) return -100.;
		t = (B + sqrt(D)) / A;
		float r = (dr * t) + r0;
		if (r < 0.0) {
			t = (B - sqrt(D)) / A;
			r = (dr * t) + r0;
			if (r < 0.0) return -100.;
		}
	} else {
		t = 0.5 * C / B;
		float r = (dr * t) + r0;
		if (r < 0.0) return -100.;
	}
	return t;
}

float readFixed(int i, int f) {
	return float(i) + float(f) / float(1 << 15);
}

vec4 getColor(int dataOffset, vec4 defaultColor) {
	if (dataOffset == -1) return defaultColor;
	ivec4 xxyx = hb_gpu_fetch(dataOffset);
	ivec4 xyyy = hb_gpu_fetch(dataOffset + 1);
	ivec4 dxdy = hb_gpu_fetch(dataOffset + 2);
	vec2 uv = (mat3(
		readFixed(xxyx.x, xxyx.y), readFixed(xxyx.z, xxyx.w), 0.,
		readFixed(xyyy.x, xyyy.y), readFixed(xyyy.z, xyyy.w), 0.,
		readFixed(dxdy.x, dxdy.y), readFixed(dxdy.z, dxdy.w), 1.
	) * vec3(v_texcoord, 1.)).xy;
	ivec4 gradient1 = hb_gpu_fetch(dataOffset + 3);
	ivec4 gradient2 = hb_gpu_fetch(dataOffset + 4);
	return getGradientColor(
		dataOffset + 5,
		defaultColor,
		gradient1.x == 0 ? evaluateLinearGradient(gradient2, uv)
		: gradient1.x == 1 ? evaluateRadialGradient(gradient1, gradient2, uv)
		: 0.5
	);
}

vec4 blend(vec4 s, vec4 d, int mode) {
	switch (mode) {
		case 5:
			return vec4(
				s.r * s.a * d.a,
				s.g * s.a * d.a,
				s.b * s.a * d.a,
				s.a * d.a
			);
		default: {
			float isa = 1. - s.a;
			return vec4(
				s.r * s.a + d.r * d.a * isa,
				s.g * s.a + d.g * d.a * isa,
				s.b * s.a + d.b * d.a * isa,
				s.a + s.b * isa
			);
		}
	}
}

vec4 getGroupColor() {
	int groupIndex = -1;
	vec4 colorStack[8];
	int cursor = int(v_dataOffset);
	for (int i = 0; i < 64; ++i) {
		ivec4 command = hb_gpu_fetch(cursor);
		++cursor;
		switch (command.x) {
			case 0: {
				uint rg = uint(command.z) & 0xFFFFu;
				uint ba = uint(command.w) & 0xFFFFu;
				vec4 workingColor = command.y == 1 ? u_foreground : vec4(
					float(rg >> 8) / 255.,
					float(rg & 0xFFu) / 255.,
					float(ba >> 8) / 255.,
					float(ba & 0xFFu) / 255.
				);
				ivec4 offsets = hb_gpu_fetch(cursor);
				++cursor;
				workingColor = getColor(int(uint(offsets.z) << 16 | uint(offsets.w)), workingColor);
				workingColor.a *= hb_gpu_draw(v_texcoord, uint(offsets.x) << 16 | uint(offsets.y));
				colorStack[groupIndex] = blend(colorStack[groupIndex], workingColor, 3);
				break;
			}
			case 1: {
				++groupIndex;
				colorStack[groupIndex] = vec4(0., 0., 0., 0.);
				break;
			}
			case 2: {
				--groupIndex;
				if (groupIndex != -1)
					colorStack[groupIndex] = blend(colorStack[groupIndex], colorStack[groupIndex + 1], command.y);
				break;
			}
		}
		if (groupIndex == -1) break;
	}
	return colorStack[0];
}

void main() {
	if (v_isGroup == 1u) {
		fragColor = getGroupColor();
	} else {
		float a = hb_gpu_draw(v_texcoord, v_dataOffset);
		if (u_stem_darkening > 0.) a = hb_gpu_stem_darken(
			a, dot(u_foreground.rgb, vec3(1.0 / 3.0)), hb_gpu_ppem(v_texcoord, v_dataOffset)
		);
		if (u_gamma != 1.) a = pow(a, u_gamma);
		if (u_debug > 0.) {
			ivec2 counts = _hb_gpu_curve_counts(v_texcoord, v_dataOffset);
			float r = clamp(float(counts.x) / 8.0, 0.0, 1.0);
			float g = clamp(float(counts.y) / 8.0, 0.0, 1.0);
			fragColor = vec4(r, g, a, max(max(r, g), a));
			return;
		}
		fragColor = getColor(int(v_gradientDataOffset), v_color.a < 0. ? u_foreground : v_color);
		fragColor.a *= a;
	}
//  float coverage = hb_gpu_draw (v_texcoord, v_glyphLoc);
//
//  /* Stem darkening / thinning at small sizes.
//   * Light text on dark: stems get too fat → thin them (exponent > 1).
//   * Dark text on light: stems get too thin → darken them (exponent < 1).
//   * The foreground brightness tells us which mode we're in. */
//  if (u_stem_darkening > 0.0)
//    coverage = hb_gpu_stem_darken (coverage,
//      dot (u_foreground.rgb, vec3 (1.0 / 3.0)),
//      hb_gpu_ppem (v_texcoord, v_glyphLoc));
//
//  if (u_gamma != 1.0)
//    coverage = pow (coverage, u_gamma);
//
//  if (u_debug > 0.0)
//  {
//    ivec2 counts = _hb_gpu_curve_counts (v_texcoord, v_glyphLoc);
//    float r = clamp (float (counts.x) / 8.0, 0.0, 1.0);
//    float g = clamp (float (counts.y) / 8.0, 0.0, 1.0);
//    fragColor = vec4 (r, g, coverage, max (max (r, g), coverage));
//    return;
//  }
//
//  fragColor = vec4 (u_foreground.rgb, u_foreground.a * coverage);
}
)glsl";



static GLuint
compile_shader (GLenum         type,
		GLsizei        count,
		const GLchar** sources)
{
  GLuint shader;
  GLint compiled;

  if (!(shader = glCreateShader (type)))
    return shader;

  glShaderSource (shader, count, sources, 0);
  glCompileShader (shader);

  glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    GLint info_len = 0;
    LOGW ("%s shader failed to compile\n",
	     type == GL_VERTEX_SHADER ? "Vertex" : "Fragment");
    glGetShaderiv (shader, GL_INFO_LOG_LENGTH, &info_len);

    if (info_len > 0) {
      char *info_log = (char*) malloc (info_len);
      glGetShaderInfoLog (shader, info_len, NULL, info_log);

      LOGW ("%s\n", info_log);
      free (info_log);
    }

    abort ();
  }

  return shader;
}

static GLuint
link_program (GLuint vertex_shader,
	      GLuint fragment_shader)
{
  GLuint program;
  GLint linked;

  program = glCreateProgram ();
  glAttachShader (program, vertex_shader);
  glAttachShader (program, fragment_shader);
  glLinkProgram (program);
  glDeleteShader (vertex_shader);
  glDeleteShader (fragment_shader);

  glGetProgramiv (program, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLint info_len = 0;
    LOGW ("Program failed to link\n");
    glGetProgramiv (program, GL_INFO_LOG_LENGTH, &info_len);

    if (info_len > 0) {
      char *info_log = (char*) malloc (info_len);
      glGetProgramInfoLog (program, info_len, NULL, info_log);

      LOGW ("%s\n", info_log);
      free (info_log);
    }

    abort ();
  }

  return program;
}

GLuint
demo_shader_create_program (void)
{
  GLuint vertex_shader, fragment_shader, program;

#ifdef HB_GPU_ATLAS_2D
  const GLchar *preamble = "#version 300 es\nprecision highp float;\nprecision highp int;\n#define HB_GPU_ATLAS_2D\n";
#else
  const GLchar *preamble = "#version 330\n";
#endif

  const GLchar *vert_sources[] = {preamble,
				  hb_gpu_draw_shader_source (HB_GPU_SHADER_STAGE_VERTEX, HB_GPU_SHADER_LANG_GLSL),
				  demo_vertex_glsl};
  vertex_shader = compile_shader (GL_VERTEX_SHADER,
				  ARRAY_LEN (vert_sources),
				  vert_sources);

  const GLchar *frag_sources[] = {preamble,
				  hb_gpu_draw_shader_source (HB_GPU_SHADER_STAGE_FRAGMENT, HB_GPU_SHADER_LANG_GLSL),
				  demo_fragment_glsl};
  fragment_shader = compile_shader (GL_FRAGMENT_SHADER,
				    ARRAY_LEN (frag_sources),
				    frag_sources);

  program = link_program (vertex_shader, fragment_shader);
  return program;
}
