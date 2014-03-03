/*
 * Rut
 *
 * Copyright (C) 2012  Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <math.h>

#include <rut.h>

#include "rig-dof-effect.h"
#include "rig-downsampler.h"
#include "rig-engine.h"

struct _RigDepthOfField
{
  RigEngine *engine;

  /* The size of our depth_pass and normal_pass textuers */
  int width;
  int height;

  /* A texture to hold depth-of-field blend factors based
   * on the distance of the geometry from the focal plane.
   */
  CoglTexture *depth_pass;
  CoglFramebuffer *depth_pass_fb;

  /* This is our normal, pristine render of the color buffer */
  CoglTexture *color_pass;
  CoglFramebuffer *color_pass_fb;

  /* This is our color buffer reduced in size and blurred */
  CoglTexture *blur_pass;

  CoglPipeline *pipeline;

  RigDownsampler *downsampler;
  RutGaussianBlurrer *blurrer;
};

RigDepthOfField *
rig_dof_effect_new (RigEngine *engine)
{
  RigDepthOfField *dof = g_slice_new0 (RigDepthOfField);
  CoglPipeline *pipeline;
  CoglSnippet *snippet;

  dof->engine = engine;

  pipeline = cogl_pipeline_new (engine->ctx->cogl_context);
  dof->pipeline = pipeline;

  cogl_pipeline_set_layer_texture (pipeline, 0, NULL); /* depth */
  cogl_pipeline_set_layer_texture (pipeline, 1, NULL); /* blurred */
  cogl_pipeline_set_layer_texture (pipeline, 2, NULL); /* color */

  /* disable blending */
  cogl_pipeline_set_blend (pipeline, "RGBA=ADD(SRC_COLOR, 0)", NULL);

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                              NULL, /* definitions */
                              NULL  /* post */);

  cogl_snippet_set_replace (snippet,
      "cogl_texel0 = texture2D (cogl_sampler0, cogl_tex_coord0_in.st);\n"
      "cogl_texel1 = texture2D (cogl_sampler1, cogl_tex_coord1_in.st);\n"
      "cogl_texel2 = texture2D (cogl_sampler2, cogl_tex_coord2_in.st);\n"
      "cogl_color_out = mix (cogl_texel1, cogl_texel2, cogl_texel0.a);\n"
      "cogl_color_out.a = 1.0;\n");

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  dof->downsampler = rig_downsampler_new (engine);
  dof->blurrer = rut_gaussian_blurrer_new (engine->ctx, 7);

  return dof;
}

void
rig_dof_effect_free (RigDepthOfField *dof)
{
  rig_downsampler_free (dof->downsampler);
  rut_gaussian_blurrer_free (dof->blurrer);
  cogl_object_unref (dof->pipeline);

  g_slice_free (RigDepthOfField, dof);
}

void
rig_dof_effect_set_framebuffer_size (RigDepthOfField *dof,
                                     int width,
                                     int height)
{
  if (dof->width == width && dof->height == height)
    return;


  if (dof->color_pass_fb)
    {
      cogl_object_unref (dof->color_pass_fb);
      dof->color_pass_fb = NULL;
      cogl_object_unref (dof->color_pass);
      dof->color_pass = NULL;
    }

  if (dof->depth_pass_fb)
    {
      cogl_object_unref (dof->depth_pass_fb);
      dof->depth_pass_fb = NULL;
      cogl_object_unref (dof->depth_pass);
      dof->depth_pass = NULL;
    }

  dof->width = width;
  dof->height = height;
}

CoglFramebuffer *
rig_dof_effect_get_depth_pass_fb (RigDepthOfField *dof)
{
  if (!dof->depth_pass)
    {
      /*
       * Offscreen render for post-processing
       */
      dof->depth_pass =
        cogl_texture_2d_new_with_size (dof->engine->ctx->cogl_context,
                                       dof->width,
                                       dof->height);

      dof->depth_pass_fb =
        cogl_offscreen_new_with_texture (dof->depth_pass);
    }

  return dof->depth_pass_fb;
}

CoglFramebuffer *
rig_dof_effect_get_color_pass_fb (RigDepthOfField *dof)
{
  if (!dof->color_pass)
    {
      /*
       * Offscreen render for post-processing
       */
      dof->color_pass =
        cogl_texture_2d_new_with_size (dof->engine->ctx->cogl_context,
                                       dof->width,
                                       dof->height);

      dof->color_pass_fb =
        cogl_offscreen_new_with_texture (dof->color_pass);
    }

  return dof->color_pass_fb;
}

void
rig_dof_effect_draw_rectangle (RigDepthOfField *dof,
                               CoglFramebuffer *fb,
                               float x1,
                               float y1,
                               float x2,
                               float y2)
{
  CoglTexture *downsampled =
    rig_downsampler_downsample (dof->downsampler, dof->color_pass, 4, 4);

  CoglTexture *blurred =
    rut_gaussian_blurrer_blur (dof->blurrer, downsampled);

  CoglPipeline *pipeline = cogl_pipeline_copy (dof->pipeline);

  cogl_pipeline_set_layer_texture (pipeline, 0, dof->depth_pass);
  cogl_pipeline_set_layer_texture (pipeline, 1, blurred);
  cogl_pipeline_set_layer_texture (pipeline, 2, dof->color_pass);

  cogl_framebuffer_draw_rectangle (fb, pipeline,
                                   x1, y1, x2, y2);

  cogl_object_unref (blurred);
  cogl_object_unref (downsampled);
}

