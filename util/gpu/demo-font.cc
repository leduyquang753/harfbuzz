/*
 * Copyright 2026 Behdad Esfahbod. All Rights Reserved.
 */

#include "demo-font.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <hb-ot.h>

typedef std::map<unsigned int, glyph_info_t> glyph_cache_t;

struct demo_font_t {
  hb_face_t       *face;
  hb_font_t       *font;
  glyph_cache_t   *glyph_cache;
  demo_atlas_t    *atlas;
  hb_gpu_draw_t  *g;

  unsigned int num_glyphs;
  unsigned int sum_bytes;
	std::vector<hb_ot_color_layer_t> hbLayerBuffer;
	std::vector<hb_color_t> hbColorBuffer;
};

static LayerData demo_font_upload_layer(
	demo_font_t *font, unsigned int glyph_index,
	const std::optional<hb_color_t> color, const std::optional<glm::mat3> transform
);

namespace {
	int getCompositeModeValue(const hb_paint_composite_mode_t hbMode) {
		switch (hbMode) {
			case HB_PAINT_COMPOSITE_MODE_CLEAR:
				return 0;
			case HB_PAINT_COMPOSITE_MODE_SRC:
				return 1;
			case HB_PAINT_COMPOSITE_MODE_DEST:
				return 2;
			default: // HB_PAINT_COMPOSITE_MODE_SRC_OVER
				return 3;
			case HB_PAINT_COMPOSITE_MODE_DEST_OVER:
				return 4;
			case HB_PAINT_COMPOSITE_MODE_SRC_IN:
				return 5;
			case HB_PAINT_COMPOSITE_MODE_DEST_IN:
				return 6;
			case HB_PAINT_COMPOSITE_MODE_SRC_OUT:
				return 7;
			case HB_PAINT_COMPOSITE_MODE_DEST_OUT:
				return 8;
			case HB_PAINT_COMPOSITE_MODE_SRC_ATOP:
				return 9;
			case HB_PAINT_COMPOSITE_MODE_DEST_ATOP:
				return 10;
			case HB_PAINT_COMPOSITE_MODE_XOR:
				return 11;
			case HB_PAINT_COMPOSITE_MODE_PLUS:
				return 12;
			case HB_PAINT_COMPOSITE_MODE_SCREEN:
				return 13;
			case HB_PAINT_COMPOSITE_MODE_OVERLAY:
				return 14;
			case HB_PAINT_COMPOSITE_MODE_DARKEN:
				return 15;
			case HB_PAINT_COMPOSITE_MODE_LIGHTEN:
				return 16;
			case HB_PAINT_COMPOSITE_MODE_COLOR_DODGE:
				return 17;
			case HB_PAINT_COMPOSITE_MODE_COLOR_BURN:
				return 18;
			case HB_PAINT_COMPOSITE_MODE_HARD_LIGHT:
				return 19;
			case HB_PAINT_COMPOSITE_MODE_SOFT_LIGHT:
				return 20;
			case HB_PAINT_COMPOSITE_MODE_DIFFERENCE:
				return 21;
			case HB_PAINT_COMPOSITE_MODE_EXCLUSION:
				return 22;
			case HB_PAINT_COMPOSITE_MODE_MULTIPLY:
				return 23;
			case HB_PAINT_COMPOSITE_MODE_HSL_HUE:
				return 24;
			case HB_PAINT_COMPOSITE_MODE_HSL_SATURATION:
				return 25;
			case HB_PAINT_COMPOSITE_MODE_HSL_COLOR:
				return 26;
			case HB_PAINT_COMPOSITE_MODE_HSL_LUMINOSITY:
				return 27;
		}
	}
}

struct ColorGlyphEncoder {
	using Component = std::int16_t;
	demo_font_t *font;
	std::vector<LayerData> *dataLayers;
	hb_paint_funcs_t *paint;
	std::vector<hb_color_stop_t> colorStopBuffer;
	std::vector<Component> buffer;
	std::vector<Component> groupBuffer;
	std::vector<glm::mat3> transforms;
	hb_font_t *clipGlyphFont;
	hb_codepoint_t clipGlyphCodepoint;
	glm::mat3 clipGlyphTransform;
	int groupCount;
	demo_extents_t groupExtents;

	ColorGlyphEncoder(demo_font_t *font): font(font) {
		paint = hb_paint_funcs_create();
		hb_paint_funcs_set_push_transform_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, float xx, float yx, float xy, float yy, float dx, float dy,
				void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				e.transforms.push_back(e.transforms.back() * glm::mat3{xx, yx, 0, xy, yy, 0, dx, dy, 1});
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_pop_transform_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				e.transforms.pop_back();
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_color_glyph_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, hb_codepoint_t codepoint, hb_font_t *font, void *userData
			) -> hb_bool_t { return true; },
			nullptr, nullptr
		);
		hb_paint_funcs_set_push_clip_glyph_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, hb_codepoint_t codepoint, hb_font_t *font, void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				e.clipGlyphFont = font;
				e.clipGlyphCodepoint = codepoint;
				e.clipGlyphTransform = e.transforms.back();
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_push_clip_rectangle_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, float minX, float minY, float maxX, float maxY, void *userData
			) {},
			nullptr, nullptr
		);
		hb_paint_funcs_set_pop_clip_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, void *userData
			) {},
			nullptr, nullptr
		);
		hb_paint_funcs_set_color_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, hb_bool_t isForeground, hb_color_t color, void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				e.dataLayers->push_back(
					demo_font_upload_layer(e.font, e.clipGlyphCodepoint, color, e.clipGlyphTransform)
				);
				e.dataLayers->back().gradientDataOffset = -1;
				e.maybeEncodeGroupLayer();
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_image_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData,
				hb_blob_t *image, unsigned width, unsigned height, hb_tag_t format, float slant,
				hb_glyph_extents_t *extents, void *userData
			) -> hb_bool_t { return true; },
			nullptr, nullptr
		);
		hb_paint_funcs_set_linear_gradient_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, hb_color_line_t *colorLine,
				float x0, float y0, float x1, float y1, float x2, float y2, void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				e.buffer.clear();
				e.encodeInverseTransform();
				e.buffer.push_back(0);
				const auto extendMode = hb_color_line_get_extend(colorLine);
				e.buffer.push_back(
					extendMode == HB_PAINT_EXTEND_REPEAT ? 1
					: extendMode == HB_PAINT_EXTEND_REFLECT ? 2
					: 0
				);
				const glm::vec2
					p0 = {x0, y0},
					p1 = {x1, y1},
					p2 = {x2, y2},
					p0p1 = p1 - p0,
					p0p2 = p2 - p0,
					p0p3 = {-p0p2.y, p0p2.x},
					p3 = p0 + p0p3 * glm::dot(p0p3, p0p1) / glm::dot(p0p3, p0p3);
				e.buffer.push_back(0);
				e.buffer.push_back(0);
				e.buffer.push_back(x0);
				e.buffer.push_back(y0);
				e.buffer.push_back(p3.x);
				e.buffer.push_back(p3.y);
				e.encodeColorStops(colorLine);
				e.dataLayers->push_back(
					demo_font_upload_layer(e.font, e.clipGlyphCodepoint, std::nullopt, e.clipGlyphTransform)
				);
				e.finalizeGradientLayer();
				e.maybeEncodeGroupLayer();
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_radial_gradient_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, hb_color_line_t *colorLine,
				float x0, float y0, float r0, float x1, float y1, float r1, void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				e.buffer.clear();
				e.encodeInverseTransform();
				e.buffer.push_back(1);
				const auto extendMode = hb_color_line_get_extend(colorLine);
				e.buffer.push_back(
					extendMode == HB_PAINT_EXTEND_REPEAT ? 1
					: extendMode == HB_PAINT_EXTEND_REFLECT ? 2
					: 0
				);
				e.buffer.push_back(x0);
				e.buffer.push_back(y0);
				e.buffer.push_back(x1);
				e.buffer.push_back(y1);
				e.buffer.push_back(r0);
				e.buffer.push_back(r1);
				e.encodeColorStops(colorLine);
				e.dataLayers->push_back(
					demo_font_upload_layer(e.font, e.clipGlyphCodepoint, std::nullopt, e.clipGlyphTransform)
				);
				e.finalizeGradientLayer();
				e.maybeEncodeGroupLayer();
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_sweep_gradient_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, hb_color_line_t *colorLine,
				float x0, float y0, float startAngle, float endAngle, void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				e.buffer.clear();
				e.encodeInverseTransform();
				e.buffer.push_back(2);
				const auto extendMode = hb_color_line_get_extend(colorLine);
				e.buffer.push_back(
					extendMode == HB_PAINT_EXTEND_REPEAT ? 1
					: extendMode == HB_PAINT_EXTEND_REFLECT ? 2
					: 0
				);
				e.buffer.push_back(0);
				e.buffer.push_back(0);
				e.buffer.push_back(x0);
				e.buffer.push_back(y0);
				e.buffer.push_back(glm::degrees(startAngle) / 180.f * (1 << 14));
				e.buffer.push_back(glm::degrees(endAngle) / 180.f * (1 << 14));
				e.encodeColorStops(colorLine);
				e.dataLayers->push_back(
					demo_font_upload_layer(e.font, e.clipGlyphCodepoint, std::nullopt, e.clipGlyphTransform)
				);
				e.finalizeGradientLayer();
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_push_group_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				if (e.groupCount == 0) e.groupBuffer.clear();
				++e.groupCount;
				e.groupBuffer.push_back(1);
				e.groupBuffer.push_back(0);
				e.groupBuffer.push_back(0);
				e.groupBuffer.push_back(0);
				demo_extents_clear(&e.groupExtents);
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_pop_group_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, hb_paint_composite_mode_t mode, void *userData
			) {
				auto &e = *static_cast<ColorGlyphEncoder*>(paintData);
				--e.groupCount;
				e.groupBuffer.push_back(2);
				e.groupBuffer.push_back(getCompositeModeValue(mode));
				e.groupBuffer.push_back(0);
				e.groupBuffer.push_back(0);
				if (e.groupCount == 0) e.finalizeGroupLayer();
			},
			nullptr, nullptr
		);
		hb_paint_funcs_set_custom_palette_color_func(
			paint,
			[](
				hb_paint_funcs_t *funcs, void *paintData, unsigned colorIndex, hb_color_t *color, void *userData
			) -> hb_bool_t { return false; },
			nullptr, nullptr
		);
		hb_paint_funcs_make_immutable(paint);
	}

	void encodeFixed(const float value) {
		float i;
		const float f = std::modf(value, &i);
		buffer.push_back(static_cast<std::int16_t>(i));
		buffer.push_back(static_cast<std::int16_t>(f * (1 << 15)));
	}

	void encodeInverseTransform() {
		const auto inverseTransform = glm::affineInverse(transforms.back());
		encodeFixed(inverseTransform[0][0]);
		encodeFixed(inverseTransform[0][1]);
		encodeFixed(inverseTransform[1][0]);
		encodeFixed(inverseTransform[1][1]);
		encodeFixed(inverseTransform[2][0]);
		encodeFixed(inverseTransform[2][1]);
	}

	void encodeColorStops(hb_color_line_t *colorLine) {
		unsigned returnedCount = 0;
		unsigned colorStopCount = hb_color_line_get_color_stops(colorLine, 0, &returnedCount, nullptr);
		if (colorStopBuffer.size() < colorStopCount) colorStopBuffer.resize(colorStopCount);
		returnedCount = colorStopCount;
		hb_color_line_get_color_stops(colorLine, 0, &returnedCount, colorStopBuffer.data());
		for (unsigned i = 0; i < colorStopCount; ++i) {
			const auto &stop = colorStopBuffer[i];
			buffer.push_back(stop.offset * (1 << 8));
			buffer.push_back(stop.is_foreground ? 1 : 0);
			buffer.push_back(hb_color_get_red(stop.color) << 8 | hb_color_get_green(stop.color));
			buffer.push_back(hb_color_get_blue(stop.color) << 8 | hb_color_get_alpha(stop.color));
		}
		buffer.end()[-3] |= 2;
	}

	void finalizeGradientLayer() {
		const unsigned encodedDataSize = buffer.size() * 2;
		auto &dataLayer = dataLayers->back();
		dataLayer.gradientDataOffset
			= demo_atlas_alloc(font->atlas, reinterpret_cast<char*>(buffer.data()), encodedDataSize);
		dataLayer.dataSize += encodedDataSize;
	}

	void maybeEncodeGroupLayer() {
		if (groupCount == 0) return;
		const auto &dataLayer = dataLayers->back();
		groupBuffer.push_back(0);
		groupBuffer.push_back(dataLayer.a < 0.f ? 1 : 0);
		groupBuffer.push_back(
			static_cast<unsigned>(std::round(dataLayer.r * 255.f)) << 8
			| static_cast<unsigned>(std::round(dataLayer.g * 255.f))
		);
		groupBuffer.push_back(
			static_cast<unsigned>(std::round(dataLayer.b * 255.f)) << 8
			| static_cast<unsigned>(std::round(dataLayer.a * 255.f))
		);
		groupBuffer.push_back(dataLayer.dataOffset >> 16);
		groupBuffer.push_back(dataLayer.dataOffset & 0xFFFF);
		groupBuffer.push_back(dataLayer.gradientDataOffset >> 16);
		groupBuffer.push_back(dataLayer.gradientDataOffset & 0xFFFF);
		demo_extents_extend(&groupExtents, &dataLayer.extents);
		dataLayers->pop_back();
	}

	void finalizeGroupLayer() {
		auto &dataLayer = dataLayers->emplace_back();
		const unsigned encodedDataSize = groupBuffer.size() * 2;
		dataLayer.dataOffset
			= demo_atlas_alloc(font->atlas, reinterpret_cast<char*>(groupBuffer.data()), encodedDataSize);
		dataLayer.gradientDataOffset = 0;
		dataLayer.dataSize = encodedDataSize;
		dataLayer.extents = groupExtents;
		dataLayer.r = -1.f;
		dataLayer.g = -1.f;
		dataLayer.b = -1.f;
		dataLayer.a = -1.f;
		dataLayer.isGroup = true;
	}

	void encode(
		std::vector<LayerData> &dataLayersIn, hb_font_t *const hbFont, const hb_codepoint_t codepoint
	) {
		dataLayers = &dataLayersIn;
		transforms.clear();
		transforms.push_back(glm::identity<glm::mat3>());
		groupCount = 0;
		hb_font_paint_glyph(hbFont, codepoint, paint, this, 0, HB_COLOR(0, 0, 0, 255));
	}
};

struct TransformedGlyphEncoder {
	hb_gpu_draw_t *hbGpuDraw;
	hb_draw_funcs_t *draw;
	hb_draw_funcs_t *hbGpuDrawFuncs = hb_gpu_draw_get_funcs();
	glm::mat3 transform;
	hb_draw_state_t drawState;

	TransformedGlyphEncoder(hb_gpu_draw_t *const hbGpuDraw): hbGpuDraw(hbGpuDraw) {
		draw = hb_draw_funcs_create();
		hb_draw_funcs_set_move_to_func(
			draw,
			[](
				hb_draw_funcs_t *funcs, void *drawData,
				hb_draw_state_t *originalDrawState, float originalToX, float originalToY, void *userData
			) {
				auto &e = *static_cast<TransformedGlyphEncoder*>(drawData);
				float
					toX = originalToX,
					toY = originalToY;
				e.transformPoint(toX, toY);
				hb_draw_move_to(e.hbGpuDrawFuncs, e.hbGpuDraw, &e.drawState, toX, toY);
			},
			nullptr, nullptr
		);
		hb_draw_funcs_set_line_to_func(
			draw,
			[](
				hb_draw_funcs_t *funcs, void *drawData,
				hb_draw_state_t *originalDrawState, float originalToX, float originalToY, void *userData
			) {
				auto &e = *static_cast<TransformedGlyphEncoder*>(drawData);
				float
					toX = originalToX,
					toY = originalToY;
				e.transformPoint(toX, toY);
				hb_draw_line_to(e.hbGpuDrawFuncs, e.hbGpuDraw, &e.drawState, toX, toY);
			},
			nullptr, nullptr
		);
		hb_draw_funcs_set_quadratic_to_func(
			draw,
			[](
				hb_draw_funcs_t *funcs, void *drawData, hb_draw_state_t *originalDrawState,
				float originalControlX, float originalControlY, float originalToX, float originalToY, void *userData
			) {
				auto &e = *static_cast<TransformedGlyphEncoder*>(drawData);
				float
					controlX = originalControlX,
					controlY = originalControlY,
					toX = originalToX,
					toY = originalToY;
				e.transformPoint(controlX, controlY);
				e.transformPoint(toX, toY);
				hb_draw_quadratic_to(e.hbGpuDrawFuncs, e.hbGpuDraw, &e.drawState, controlX, controlY, toX, toY);
			},
			nullptr, nullptr
		);
		hb_draw_funcs_set_cubic_to_func(
			draw,
			[](
				hb_draw_funcs_t *funcs, void *drawData, hb_draw_state_t *originalDrawState,
				float originalControl1X, float originalControl1Y, float originalControl2X, float originalControl2Y,
				float originalToX, float originalToY, void *userData
			) {
				auto &e = *static_cast<TransformedGlyphEncoder*>(drawData);
				float
					control1X = originalControl1X,
					control1Y = originalControl1Y,
					control2X = originalControl2X,
					control2Y = originalControl2Y,
					toX = originalToX,
					toY = originalToY;
				e.transformPoint(control1X, control1Y);
				e.transformPoint(control2X, control2Y);
				e.transformPoint(toX, toY);
				hb_draw_cubic_to(
					e.hbGpuDrawFuncs, e.hbGpuDraw, &e.drawState, control1X, control1Y, control2X, control2Y, toX, toY
				);
			},
			nullptr, nullptr
		);
		hb_draw_funcs_set_close_path_func(
			draw,
			[](hb_draw_funcs_t *funcs, void *drawData, hb_draw_state_t *originalDrawState, void *userData) {
				auto &e = *static_cast<TransformedGlyphEncoder*>(drawData);
				hb_draw_close_path(e.hbGpuDrawFuncs, e.hbGpuDraw, &e.drawState);
			},
			nullptr, nullptr
		);
		hb_draw_funcs_make_immutable(draw);
	}

	void transformPoint(float &x, float &y) {
		const auto transformedPoint = transform * glm::vec3(x, y, 1.f);
		x = transformedPoint.x;
		y = transformedPoint.y;
	}

	void encode(hb_font_t *const hbFont, const hb_codepoint_t codepoint, const glm::mat3 transformIn) {
		transform = transformIn;
		drawState = {};
		hb_font_draw_glyph(hbFont, codepoint, draw, this);
	}
};

demo_font_t *
demo_font_create (hb_font_t    *hb_font,
		  demo_atlas_t *atlas)
{
  demo_font_t *font = (demo_font_t *) calloc (1, sizeof (demo_font_t));

  font->face = hb_face_reference (hb_font_get_face (hb_font));
  font->font = hb_font_reference (hb_font);
  font->glyph_cache = new glyph_cache_t ();
  font->atlas = demo_atlas_reference (atlas);
  font->g = hb_gpu_draw_create_or_fail ();

  return font;
}

void
demo_font_destroy (demo_font_t *font)
{
  if (!font)
    return;

  hb_gpu_draw_destroy (font->g);
  demo_atlas_destroy (font->atlas);
  delete font->glyph_cache;
  hb_font_destroy (font->font);
  hb_face_destroy (font->face);
  free (font);
}


hb_face_t *
demo_font_get_face (demo_font_t *font)
{
  return font->face;
}

hb_font_t *
demo_font_get_font (demo_font_t *font)
{
  return font->font;
}

static LayerData demo_font_upload_layer(
	demo_font_t *font, unsigned glyphIndex,
	const std::optional<hb_color_t> color, const std::optional<glm::mat3> transform
) {
	int xScale, yScale;
	hb_font_get_scale(font->font, &xScale, &yScale);
	hb_gpu_draw_set_scale(font->g, xScale, yScale);
	if (transform) TransformedGlyphEncoder{font->g}.encode(font->font, glyphIndex, *transform);
	else hb_font_draw_glyph(font->font, glyphIndex, hb_gpu_draw_get_funcs(), font->g);
	hb_glyph_extents_t hb_ext;
	hb_blob_t *blob = hb_gpu_draw_encode(font->g, &hb_ext);
	if (!blob) die("Failed to encode glyph layer.");
	unsigned int len = hb_blob_get_length (blob);

	/* Get extents in font design units */
	LayerData layerData = {};
	layerData.extents.min_x = hb_ext.x_bearing;
	layerData.extents.max_x = hb_ext.x_bearing + hb_ext.width;
	layerData.extents.max_y = hb_ext.y_bearing;
	layerData.extents.min_y = hb_ext.y_bearing + hb_ext.height;
	layerData.dataSize = len;
	layerData.isEmpty = len == 0;
	if (!layerData.isEmpty)
		layerData.dataOffset = demo_atlas_alloc(font->atlas, hb_blob_get_data(blob, NULL), len);
	layerData.gradientDataOffset = -1;
	hb_gpu_draw_recycle_blob(font->g, blob);
	if (color) {
		layerData.r = hb_color_get_red(*color) / 255.f;
		layerData.g = hb_color_get_green(*color) / 255.f;
		layerData.b = hb_color_get_blue(*color) / 255.f;
		layerData.a = hb_color_get_alpha(*color) / 255.f;
	} else {
		layerData.r = -1.f;
		layerData.g = -1.f;
		layerData.b = -1.f;
		layerData.a = -1.f;
	}
	return layerData;
}

static void demo_font_upload_glyph(demo_font_t *font, unsigned glyphIndex, glyph_info_t *glyphInfo) {
	glyphInfo->advance = hb_font_get_glyph_h_advance(font->font, glyphIndex);
	int xScale, yScale;
	hb_font_get_scale(font->font, &xScale, &yScale);
	glyphInfo->upem = yScale;

	if (hb_ot_color_glyph_has_paint(font->face, glyphIndex)) {
		ColorGlyphEncoder{font}.encode(glyphInfo->layers, font->font, glyphIndex);
	} else {
		unsigned returnCount = 0;
		const unsigned layerCount = hb_ot_color_glyph_get_layers(font->face, glyphIndex, 0, &returnCount, nullptr);
		if (layerCount == 0) {
			glyphInfo->layers.push_back(demo_font_upload_layer(font, glyphIndex, std::nullopt, std::nullopt));
		} else {
			const unsigned paletteCount = hb_ot_color_palette_get_count(font->face);
			unsigned paletteIndex = 0;
			for (unsigned i = 0; i < paletteCount; ++i) {
				if (hb_ot_color_palette_get_flags(font->face, i) | HB_OT_COLOR_PALETTE_FLAG_DEFAULT) {
					paletteIndex = i;
					break;
				}
			}
			if (layerCount > font->hbLayerBuffer.size()) font->hbLayerBuffer.resize(layerCount);
			returnCount = layerCount;
			hb_ot_color_glyph_get_layers(font->face, glyphIndex, 0, &returnCount, font->hbLayerBuffer.data());
			returnCount = 0;
			const unsigned colorCount = hb_ot_color_palette_get_colors(
				font->face, paletteIndex, 0, &returnCount, nullptr
			);
			if (colorCount > font->hbColorBuffer.size()) font->hbColorBuffer.resize(colorCount);
			returnCount = colorCount;
			hb_ot_color_palette_get_colors(font->face, 0, 0, &returnCount, font->hbColorBuffer.data());
			for (unsigned i = 0; i < layerCount; ++i) {
				const auto &hbLayer = font->hbLayerBuffer[i];
				glyphInfo->layers.push_back(demo_font_upload_layer(
					font, glyphIndex,
					hbLayer.color_index == 0xFFFF
						? std::nullopt
						: std::optional<hb_color_t>{font->hbColorBuffer[hbLayer.color_index]},
					std::nullopt
				));
			}
		}
	}

	font->num_glyphs++;
	for (const auto &layer : glyphInfo->layers) font->sum_bytes += layer.dataSize;
}

void
demo_font_lookup_glyph (demo_font_t  *font,
			unsigned int  glyph_index,
			glyph_info_t *glyph_info)
{
  if (font->glyph_cache->find (glyph_index) == font->glyph_cache->end ()) {
    demo_font_upload_glyph (font, glyph_index, glyph_info);
    (*font->glyph_cache)[glyph_index] = *glyph_info;
  } else
    *glyph_info = (*font->glyph_cache)[glyph_index];
}

void
demo_font_clear_cache (demo_font_t *font)
{
  font->glyph_cache->clear ();
  font->num_glyphs = 0;
  font->sum_bytes = 0;
}

void
demo_font_print_stats (demo_font_t *font)
{
  double atlas_used_kb = demo_atlas_get_used (font->atlas) * 8 / 1024.;

  if (!font->num_glyphs)
    return;

  LOGI ("%3d glyphs; avg %5.2fkb per glyph; atlas used %5.2fkb\n",
	font->num_glyphs,
	font->sum_bytes / 1024. / font->num_glyphs,
	atlas_used_kb);
}
