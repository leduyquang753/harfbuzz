/*
 * Copyright 2026 Behdad Esfahbod. All Rights Reserved.
 */

#ifndef DEMO_SHADERS_H
#define DEMO_SHADERS_H

#include "demo-common.h"
#include "demo-font.h"


struct glyph_vertex_t {
  /* Object-space position */
  GLfloat x;
  GLfloat y;
  /* Em-space texture coordinates */
  GLfloat tx;
  GLfloat ty;
  /* Object-space outward normal at this vertex */
  GLfloat nx;
  GLfloat ny;
  GLfloat color[4];
  /* Em units per object-space unit (upem / font_size) */
  GLfloat emPerPos;
  /* Atlas offset (constant across glyph) */
  GLuint dataOffset, gradientDataOffset, isGroup;
};

static inline void
demo_shader_add_glyph_vertices (const demo_point_t              &p,
				double                           font_size,
				glyph_info_t                    *gi,
				std::vector<glyph_vertex_t>     *vertices,
				demo_extents_t                  *extents)
{
	if (extents) demo_extents_clear(extents);
	for (const auto &layer : gi->layers) {
		if (layer.isEmpty) continue;
		double scale = font_size / gi->upem;
		glyph_vertex_t quad[4];
		for (int ci = 0; ci < 4; ci++) {
			int cx = (ci >> 1) & 1;
			int cy = ci & 1;
			double ex = (1 - cx) * layer.extents.min_x + cx * layer.extents.max_x;
			double ey = (1 - cy) * layer.extents.min_y + cy * layer.extents.max_y;
			auto &vertex = quad[ci];
			vertex.x = (float) (p.x + scale * ex);
			vertex.y = (float) (p.y - scale * ey);
			vertex.tx = (float) ex;
			vertex.ty = (float) ey;
			vertex.nx = cx ? 1.f : -1.f;
			vertex.ny = cy ? -1.f : 1.f;
			vertex.emPerPos = (float) (1.0 / scale);
			vertex.dataOffset = layer.dataOffset;
			vertex.gradientDataOffset = layer.gradientDataOffset;
			vertex.isGroup = layer.isGroup ? 1 : 0;
			vertex.color[0] = layer.r;
			vertex.color[1] = layer.g;
			vertex.color[2] = layer.b;
			vertex.color[3] = layer.a;
		}

		vertices->push_back(quad[0]);
		vertices->push_back(quad[1]);
		vertices->push_back(quad[2]);

		vertices->push_back(quad[1]);
		vertices->push_back(quad[2]);
		vertices->push_back(quad[3]);

		if (extents) for (int i = 0; i < 4; i++) {
			demo_point_t pt = {(double) quad[i].x, (double) quad[i].y};
			demo_extents_add(extents, &pt);
		}
	}
}

GLuint
demo_shader_create_program (void);


#endif /* DEMO_SHADERS_H */
