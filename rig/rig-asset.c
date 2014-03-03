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

#include <stdlib.h>
#include <glib.h>

#include <cogl/cogl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>

#include <rut.h>

#include "components/rig-model.h"
#include "rig-asset.h"

#if 0
enum {
  ASSET_N_PROPS
};
#endif

struct _RigAsset
{
  RutObjectBase _base;

  RutContext *ctx;

#if 0
  RutIntrospectableProps introspectable;
  RutProperty props[ASSET_N_PROPS];
#endif

  RigAssetType type;

  /* NB: either the path or data will be valid but not both */
  char *path;

  uint8_t *data;
  size_t data_len;

  CoglTexture *texture;

  RutMesh *mesh;
  bool has_tex_coords;
  bool has_normals;

  bool is_video;

  GList *inferred_tags;

  RutList thumbnail_cb_list;
};

#if 0
static RutPropertySpec _asset_prop_specs[] = {
  { 0 }
};
#endif

#if 0
#endif

static void
_rig_asset_free (void *object)
{
  RigAsset *asset = object;

  if (asset->texture)
    cogl_object_unref (asset->texture);

  if (asset->path)
    g_free (asset->path);

  //rut_introspectable_destroy (asset);

  rut_object_free (RigAsset, asset);
}

void
rig_asset_reap (RigAsset *asset, RigEngine *engine)
{
  /* Assets don't currently contain any other objects that would need
   * to be explicitly unregistered */

  rig_engine_queue_delete (engine, asset);
}

/* This is for copy & paste where we don't currently want a deep copy */
static RutObject *
_rig_asset_copy (RutObject *mimable)
{
  return rut_object_ref (mimable);
}

static bool
_rig_asset_has (RutObject *mimable, RutMimableType type)
{
  if (type == RUT_MIMABLE_TYPE_OBJECT)
    return TRUE;
  else
    return FALSE;
}

static void *
_rig_asset_get (RutObject *mimable, RutMimableType type)
{
  if (type == RUT_MIMABLE_TYPE_OBJECT)
    return mimable;
  else
    return NULL;
}

RutType rig_asset_type;

void
_rig_asset_init_type (void)
{
  static RutMimableVTable mimable_vtable = {
    .copy = _rig_asset_copy,
    .has = _rig_asset_has,
    .get = _rig_asset_get,
  };

  RutType *type = &rig_asset_type;
#define TYPE RigAsset

  rut_type_init (&rig_asset_type, G_STRINGIFY (TYPE), _rig_asset_free);
  rut_type_add_trait (type,
                      RUT_TRAIT_ID_MIMABLE,
                      0, /* no associated properties */
                      &mimable_vtable);

#if 0
  rut_type_add_trait (&_asset_type,
                      RUT_TRAIT_ID_INTROSPECTABLE,
                      offsetof (Asset, introspectable),
                      NULL); /* no implied vtable */
#endif

#undef TYPE
}

/* These should be sorted in descending order of size to
 * avoid gaps due to attributes being naturally aligned. */
static RutPLYAttribute ply_attributes[] =
{
  {
    .name = "cogl_position_in",
    .properties = {
      { "x" },
      { "y" },
      { "z" },
    },
    .n_properties = 3,
    .min_components = 1,
  },
  {
    .name = "cogl_normal_in",
    .properties = {
      { "nx" },
      { "ny" },
      { "nz" },
    },
    .n_properties = 3,
    .min_components = 3,
    .pad_n_components = 3,
    .pad_type = RUT_ATTRIBUTE_TYPE_FLOAT,
  },
  {
    .name = "cogl_tex_coord0_in",
    .properties = {
      { "s" },
      { "t" },
      { "r" },
    },
    .n_properties = 3,
    .min_components = 2,
    .pad_n_components = 2,
    .pad_type = RUT_ATTRIBUTE_TYPE_FLOAT,
  },
  {
    .name = "tangent_in",
    .properties = {
      { "tanx" },
      { "tany" },
      { "tanz" }
    },
    .n_properties = 3,
    .min_components = 3,
    .pad_n_components = 3,
    .pad_type = RUT_ATTRIBUTE_TYPE_FLOAT,
  },
  {
    .name = "cogl_color_in",
    .properties = {
      { "red" },
      { "green" },
      { "blue" },
      { "alpha" }
    },
    .n_properties = 4,
    .normalized = TRUE,
    .min_components = 3,
  }
};

typedef struct _RigThumbnailGenerator
{
  CoglContext *ctx;
  CoglPipeline *cogl_pipeline;
  RigAsset *video;
  GstElement *pipeline;
  GstElement *bin;
  CoglGstVideoSink *sink;
  bool seek_done;
}RigThumbnailGenerator;

static void
video_thumbnailer_grab (void *instance, void *user_data)
{
  RigThumbnailGenerator *generator = user_data;
  CoglOffscreen *offscreen;
  CoglFramebuffer *fbo;
  int tex_width;
  int tex_height;

  generator->cogl_pipeline = cogl_gst_video_sink_get_pipeline (generator->sink);

  tex_height = 200;
  tex_width = cogl_gst_video_sink_get_width_for_height (generator->sink,
                                                        tex_height);

  if (generator->video->texture)
    cogl_object_unref (generator->video->texture);

  generator->video->texture =
    cogl_texture_2d_new_with_size (generator->ctx,
                                   tex_width,
                                   tex_height);

  offscreen = cogl_offscreen_new_with_texture (generator->video->texture);
  fbo = offscreen;

  cogl_framebuffer_clear4f (fbo, COGL_BUFFER_BIT_COLOR, 0, 0, 0, 0);
  cogl_framebuffer_orthographic (fbo, 0, 0, tex_width, tex_height, 1, -1);
  cogl_framebuffer_draw_textured_rectangle (fbo, generator->cogl_pipeline,
                                            0, 0, tex_width, tex_height,
                                            0, 0, 1, 1);

  cogl_object_unref (offscreen);
  gst_element_set_state (generator->pipeline, GST_STATE_NULL);
  g_object_unref (generator->sink);

  rut_closure_list_invoke (&generator->video->thumbnail_cb_list,
                           RutThumbnailCallback,
                           generator->video);

  g_free (generator);
}

static gboolean
video_thumbnailer_seek (GstBus *bus,
                        GstMessage *msg,
                        void *user_data)
{
  RigThumbnailGenerator *generator = (RigThumbnailGenerator*) user_data;
  int64_t duration, seek;

  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ASYNC_DONE && !generator->seek_done)
    {
      gst_element_query_duration (generator->bin, GST_FORMAT_TIME, &duration);
      seek = (rand () % (duration / (GST_SECOND))) * GST_SECOND;
      gst_element_seek_simple (generator->pipeline, GST_FORMAT_TIME,
          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, seek);

      gst_element_get_state (generator->bin, NULL, 0,
                              0.2 * GST_SECOND);
      generator->seek_done = TRUE;
    }

  return TRUE;
}

static void
generate_video_thumbnail (RigAsset *asset)
{
  RigThumbnailGenerator *generator = g_new (RigThumbnailGenerator, 1);
  RutContext *ctx = asset->ctx;
  char *filename;
  char *uri;
  GstBus *bus;

  generator->seek_done = FALSE;
  generator->ctx = ctx->cogl_context;
  generator->video = asset;
  generator->sink = cogl_gst_video_sink_new (ctx->cogl_context);
  generator->pipeline = gst_pipeline_new ("thumbnailer");
  generator->bin = gst_element_factory_make ("playbin", NULL);

  filename = g_build_filename (ctx->assets_location, asset->path, NULL);
  uri = gst_filename_to_uri (filename, NULL);
  g_free (filename);

  g_object_set (G_OBJECT (generator->bin), "video-sink",
                GST_ELEMENT (generator->sink),NULL);
  g_object_set (G_OBJECT (generator->bin), "uri", uri, NULL);
  gst_bin_add (GST_BIN (generator->pipeline), generator->bin);

  gst_element_set_state (generator->pipeline, GST_STATE_PAUSED);

  bus = gst_element_get_bus (generator->pipeline);
  gst_bus_add_watch (bus, video_thumbnailer_seek, generator);

  g_signal_connect (generator->sink, "new-frame",
                    G_CALLBACK (video_thumbnailer_grab), generator);

  g_free (uri);
}

static CoglTexture *
generate_mesh_thumbnail (RigAsset *asset)
{
  RutContext *ctx = asset->ctx;
  RigModel *model = rig_model_new_from_asset (ctx, asset);
  RutMesh *mesh;
  CoglTexture *thumbnail;
  CoglOffscreen *offscreen;
  CoglFramebuffer *frame_buffer;
  CoglPipeline *pipeline;
  CoglPrimitive *primitive;
  CoglSnippet *snippet;
  CoglDepthState depth_state;
  CoglMatrix view;
  int tex_width = 800;
  int tex_height = 800;
  float fovy = 60;
  float aspect = (float)tex_width / (float)tex_height;
  float z_near = 0.1;
  float z_2d = 1000;
  float z_far = 2000;
  float translate_x = 0;
  float translate_y = 0;
  float translate_z = 0;
  float rec_scale = 800;
  float scale_facor = 1;
  float model_scale;
  float width = model->max_x - model->min_x;
  float height = model->max_y - model->min_y;
  float length = model->max_z - model->min_z;
  float light_pos[3] = { model->max_x, model->max_y, model->max_z};
  float light_amb[4] = { 0.2, 0.2, 0.2, 1.0 };
  float light_diff[4] = { 0.5, 0.5, 0.5, 1.0 };
  float light_spec[4] = { 0.5, 0.5, 0.5, 1.0 };
  float mat_amb[4] = { 0.2, 0.2, 0.2, 1.0 };
  float mat_diff[4] = { 0.39, 0.64, 0.62, 1.0};
  float mat_spec[4] = {0.5, 0.5, 0.5, 1.0};
  int location;

  /* XXX: currently we don't just directly refer to asset->mesh
   * since this may be missing normals and texture coordinates
   */
  mesh = rig_model_get_mesh (model);

  thumbnail =
    cogl_texture_2d_new_with_size (ctx->cogl_context,
                                   tex_width,
                                   tex_height);

  offscreen = cogl_offscreen_new_with_texture (thumbnail);
  frame_buffer = offscreen;

  cogl_framebuffer_perspective (frame_buffer, fovy, aspect, z_near, z_far);
  cogl_matrix_init_identity (&view);
  cogl_matrix_view_2d_in_perspective (&view, fovy, aspect, z_near, z_2d,
                                      tex_width, tex_height);
  cogl_framebuffer_set_modelview_matrix (frame_buffer, &view);

  pipeline = cogl_pipeline_new (ctx->cogl_context);

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_VERTEX,
           "attribute vec3 tangent_in;\n"
           "attribute vec2 cogl_tex_coord0_in;\n"
           "attribute vec2 cogl_tex_coord1_in;\n"
           "attribute vec2 cogl_tex_coord2_in;\n"
           "attribute vec2 cogl_tex_coord5_in;\n"
           "uniform vec3 light_pos;\n"
           "uniform vec4 light_amb;\n"
           "uniform vec4 light_diff;\n"
           "uniform vec4 light_spec;\n"
           "uniform vec4 mat_amb;\n"
           "uniform vec4 mat_diff;\n"
           "uniform vec4 mat_spec;\n"
           "varying vec3 trans_light;\n"
           "varying vec3 eye;\n"
           "varying vec3 normal;\n",
           "normal = vec3 (normalize (cogl_modelview_matrix * \
                                      vec4 (cogl_normal_in.x, cogl_normal_in.y,\
                                      cogl_normal_in.z, 1.0)));\n"
           "eye = -vec3 (cogl_modelview_matrix * cogl_position_in);\n"
           "trans_light = vec3 (normalize (cogl_modelview_matrix *\
                                           vec4 (light_pos.x, light_pos.y,\
                                           light_pos.z, 1.0)));\n"
           );

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                              "uniform vec3 light_pos;\n"
                              "uniform vec4 light_amb;\n"
                              "uniform vec4 light_diff;\n"
                              "uniform vec4 light_spec;\n"
                              "uniform vec4 mat_amb;\n"
                              "uniform vec4 mat_diff;\n"
                              "uniform vec4 mat_spec;\n"
                              "varying vec3 trans_light;\n"
                              "varying vec3 eye;\n"
                              "varying vec3 normal;\n",
                              "vec4 final_color;\n"
                              "vec3 L = normalize (trans_light);\n"
                              "vec3 N = normalize (normal);\n"
                              "vec4 ambient = light_amb * mat_amb;\n"
                              "float lambert = dot (N, L);\n"
                              "if (lambert > 0.0)\n"
                              "{\n"
                              "vec4 diffuse = light_diff * mat_diff;\n"
                              "vec4 spec = light_spec * mat_spec;\n"
                              "final_color = ambient;\n"
                              "final_color += diffuse * lambert;\n"
                              "vec3 E = normalize (eye);\n"
                              "vec3 R = reflect (-L, N);\n"
                              "float spec_factor = pow (max (dot (R, E), 0.0),\
                                                        1000.0);\n"
                              "final_color += spec * spec_factor;\n"
                              "}\n"
                              "cogl_color_out = final_color;\n"
                              );

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  location = cogl_pipeline_get_uniform_location (pipeline, "light_pos");
  cogl_pipeline_set_uniform_float (pipeline, location, 3, 1, light_pos);
  location = cogl_pipeline_get_uniform_location (pipeline, "light_amb");
  cogl_pipeline_set_uniform_float (pipeline, location, 4, 1, light_amb);
  location = cogl_pipeline_get_uniform_location (pipeline, "light_diff");
  cogl_pipeline_set_uniform_float (pipeline, location, 4, 1, light_diff);
  location = cogl_pipeline_get_uniform_location (pipeline, "light_spec");
  cogl_pipeline_set_uniform_float (pipeline, location, 4, 1, light_spec);
  location = cogl_pipeline_get_uniform_location (pipeline, "mat_amb");
  cogl_pipeline_set_uniform_float (pipeline, location, 4, 1, mat_amb);
  location = cogl_pipeline_get_uniform_location (pipeline, "mat_diff");
  cogl_pipeline_set_uniform_float (pipeline, location, 4, 1, mat_diff);
  location = cogl_pipeline_get_uniform_location (pipeline, "mat_spec");
  cogl_pipeline_set_uniform_float (pipeline, location, 4, 1, mat_spec);

  cogl_depth_state_init (&depth_state);
  cogl_depth_state_set_test_enabled (&depth_state, TRUE);
  cogl_pipeline_set_depth_state (pipeline, &depth_state, NULL);

  primitive = rut_mesh_create_primitive (ctx, mesh);

  if (width > height)
    model_scale = width;
  else
    model_scale = height;

  if (rec_scale > model_scale)
    scale_facor = rec_scale / model_scale;

  if (model->max_x < 0)
    translate_x = -1 * (width * 0.5) - model->min_x;
  else if (model->min_x > 0)
    translate_x = model->min_x - (-1 * (width * 0.5));

  if (model->max_y < 0)
    translate_y = -1 * (height * 0.5) - model->min_y;
  else if (model->min_y > 0)
    translate_y = model->min_y - (-1 * (height * 0.5));

  if (model->max_z < 0)
    translate_z = -1 * (length * 0.5) - model->min_z;
  else if (model->min_z > 0)
    translate_z = model->min_z - (-1 * (length * 0.5));

  cogl_framebuffer_clear4f (frame_buffer,
                            COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                            0, 0, 0, 0);

  cogl_framebuffer_translate (frame_buffer, tex_width / 2.0, tex_height / 2.0,
                              0);
  cogl_framebuffer_push_matrix (frame_buffer);
  cogl_framebuffer_translate (frame_buffer, translate_x, translate_y, translate_z);
  cogl_framebuffer_scale (frame_buffer, scale_facor, scale_facor, scale_facor);
  cogl_primitive_draw (primitive, frame_buffer, pipeline);
  cogl_framebuffer_pop_matrix (frame_buffer);

  cogl_object_unref (primitive);
  cogl_object_unref (pipeline);
  cogl_object_unref (frame_buffer);

  rut_object_unref (model);

  return thumbnail;
}

static RigAsset *
rig_asset_new_full (RutContext *ctx,
                    const char *path,
                    const GList *inferred_tags,
                    RigAssetType type)
{
  RigAsset *asset =
    rut_object_alloc0 (RigAsset, &rig_asset_type, _rig_asset_init_type);
  const char *real_path;
  char *full_path;

#ifndef __ANDROID__
  if (type == RIG_ASSET_TYPE_BUILTIN)
    {
      full_path = rut_find_data_file (path);
      if (full_path == NULL)
        full_path = g_strdup (path);
    }
  else
    full_path = g_build_filename (ctx->assets_location, path, NULL);
  real_path = full_path;
#else
  real_path = path;
#endif



  asset->ctx = ctx;

  asset->type = type;

  rig_asset_set_inferred_tags (asset, inferred_tags);
  asset->is_video = rut_util_find_tag (inferred_tags, "video");

  rut_list_init (&asset->thumbnail_cb_list);

  switch (type)
    {
    case RIG_ASSET_TYPE_BUILTIN:
    case RIG_ASSET_TYPE_TEXTURE:
    case RIG_ASSET_TYPE_NORMAL_MAP:
    case RIG_ASSET_TYPE_ALPHA_MASK:
      {
        CoglError *error = NULL;

        if (!asset->is_video)
          asset->texture = rut_load_texture (ctx, real_path, &error);
        else
          asset->texture =
            rut_load_texture (ctx,
                              rut_find_data_file ("thumb-video.png"), &error);

        if (!asset->texture)
          {
            rut_object_free (RigAsset, asset);
            g_warning ("Failed to load asset texture: %s", error->message);
            cogl_error_free (error);
            asset = NULL;
            goto DONE;
          }

        break;
      }
    case RIG_ASSET_TYPE_MESH:
      {
        RutPLYAttributeStatus padding_status[G_N_ELEMENTS (ply_attributes)];
        GError *error = NULL;

        asset->mesh = rut_mesh_new_from_ply (ctx,
                                             real_path,
                                             ply_attributes,
                                             G_N_ELEMENTS (ply_attributes),
                                             padding_status,
                                             &error);

        if (!asset->mesh)
          {
            rut_object_free (RigAsset, asset);
            g_warning ("could not load model %s: %s", path, error->message);
            g_error_free (error);
            asset = NULL;
            goto DONE;
          }

        if (padding_status[1] == RUT_PLY_ATTRIBUTE_STATUS_PADDED)
          asset->has_normals = false;
        else
          asset->has_normals = true;

        if (padding_status[2] == RUT_PLY_ATTRIBUTE_STATUS_PADDED)
          asset->has_tex_coords = false;
        else
          asset->has_tex_coords = true;

        asset->texture = generate_mesh_thumbnail (asset);

        break;
      }
    }
  asset->path = g_strdup (path);

  //rut_introspectable_init (asset);

DONE:

#ifndef __ANDROID__
  g_free (full_path);
#endif

  return asset;
}

static CoglBitmap *
bitmap_new_from_pixbuf (CoglContext *ctx,
                        GdkPixbuf *pixbuf)
{
  bool has_alpha;
  GdkColorspace color_space;
  CoglPixelFormat pixel_format;
  int width;
  int height;
  int rowstride;
  int bits_per_sample;
  int n_channels;
  CoglBitmap *bmp;

  /* Get pixbuf properties */
  has_alpha       = gdk_pixbuf_get_has_alpha (pixbuf);
  color_space     = gdk_pixbuf_get_colorspace (pixbuf);
  width           = gdk_pixbuf_get_width (pixbuf);
  height          = gdk_pixbuf_get_height (pixbuf);
  rowstride       = gdk_pixbuf_get_rowstride (pixbuf);
  bits_per_sample = gdk_pixbuf_get_bits_per_sample (pixbuf);
  n_channels      = gdk_pixbuf_get_n_channels (pixbuf);

  /* According to current docs this should be true and so
   * the translation to cogl pixel format below valid */
  g_assert (bits_per_sample == 8);

  if (has_alpha)
    g_assert (n_channels == 4);
  else
    g_assert (n_channels == 3);

  /* Translate to cogl pixel format */
  switch (color_space)
    {
    case GDK_COLORSPACE_RGB:
      /* The only format supported by GdkPixbuf so far */
      pixel_format = has_alpha ?
	COGL_PIXEL_FORMAT_RGBA_8888 :
	COGL_PIXEL_FORMAT_RGB_888;
      break;

    default:
      /* Ouch, spec changed! */
      g_object_unref (pixbuf);
      return FALSE;
    }

  /* We just use the data directly from the pixbuf so that we don't
   * have to copy to a seperate buffer.
   */
  bmp = cogl_bitmap_new_for_data (ctx,
                                  width,
                                  height,
                                  pixel_format,
                                  rowstride,
                                  gdk_pixbuf_get_pixels (pixbuf));

  return bmp;
}

RigAsset *
rig_asset_new_from_data (RutContext *ctx,
                         const char *name,
                         RigAssetType type,
                         bool is_video,
                         const uint8_t *data,
                         size_t len)
{
  RigAsset *asset =
    rut_object_alloc0 (RigAsset, &rig_asset_type, _rig_asset_init_type);



  asset->ctx = ctx;

  asset->type = type;

  asset->path = g_strdup (name);

  asset->is_video = is_video;
  if (is_video)
    {
      asset->data = g_memdup (data, len);
      asset->data_len = len;
    }
  else
    {
      asset->data = NULL;

      switch (type)
        {
        case RIG_ASSET_TYPE_BUILTIN:
        case RIG_ASSET_TYPE_TEXTURE:
        case RIG_ASSET_TYPE_NORMAL_MAP:
        case RIG_ASSET_TYPE_ALPHA_MASK:
            {
              GInputStream *istream =
                g_memory_input_stream_new_from_data (data, len, NULL);
              GError *error = NULL;
              GdkPixbuf *pixbuf =
                gdk_pixbuf_new_from_stream (istream, NULL, &error);
              CoglBitmap *bitmap;
              CoglError *cogl_error = NULL;

              if (!pixbuf)
                {
                  rut_object_free (RigAsset, asset);
                  g_warning ("Failed to load asset texture: %s", error->message);
                  g_error_free (error);
                  return NULL;
                }

              g_object_unref (istream);

              bitmap = bitmap_new_from_pixbuf (ctx->cogl_context, pixbuf);

              asset->texture = cogl_texture_2d_new_from_bitmap (bitmap);

              /* Allocate now so we can simply free the data
               * TODO: allow asynchronous upload. */
              cogl_texture_allocate (asset->texture, NULL);

              cogl_object_unref (bitmap);
              g_object_unref (pixbuf);

              if (!asset->texture)
                {
                  rut_object_free (RigAsset, asset);
                  g_warning ("Failed to load asset texture: %s",
                             cogl_error->message);
                  cogl_error_free (cogl_error);
                  return NULL;
                }

              break;
            }
        case RIG_ASSET_TYPE_MESH:
            {
              RutPLYAttributeStatus padding_status[G_N_ELEMENTS (ply_attributes)];
              GError *error = NULL;

              asset->mesh =
                rut_mesh_new_from_ply_data (ctx,
                                            data,
                                            len,
                                            ply_attributes,
                                            G_N_ELEMENTS (ply_attributes),
                                            padding_status,
                                            &error);
              if (!asset->mesh)
                {
                  rut_object_free (RigAsset, asset);
                  g_warning ("could not load model %s: %s",
                             name, error->message);
                  g_error_free (error);
                  return NULL;
                }

              if (padding_status[1] == RUT_PLY_ATTRIBUTE_STATUS_PADDED)
                asset->has_normals = false;
              else
                asset->has_normals = true;

              if (padding_status[2] == RUT_PLY_ATTRIBUTE_STATUS_PADDED)
                asset->has_tex_coords = false;
              else
                asset->has_tex_coords = true;

              asset->texture = generate_mesh_thumbnail (asset);

              break;
            }
        }
    }

  return asset;
}

RigAsset *
rig_asset_new_from_mesh (RutContext *ctx,
                         RutMesh *mesh)
{
  RigAsset *asset =
    rut_object_alloc0 (RigAsset, &rig_asset_type, _rig_asset_init_type);
  int i;

  asset->ctx = ctx;

  asset->type = RIG_ASSET_TYPE_MESH;

  asset->mesh = rut_object_ref (mesh);
  asset->has_normals = false;
  asset->has_tex_coords = false;

  for (i = 0; i < mesh->n_attributes; i++)
    {
      if (strcmp (mesh->attributes[i]->name, "cogl_normal_in") == 0)
        asset->has_normals = true;
      else if (strcmp (mesh->attributes[i]->name, "cogl_tex_coord0_in") == 0)
        asset->has_tex_coords = true;
    }

  /* XXX: for ply mesh handling the needs_normals/tex_coords refers
   * to needing to initialize these attributes, since we guarantee
   * that the mesh itself will always have cogl_normal_in and
   * cogl_tex_coord0_in attributes.
   */
#warning "fixme: not consistent with ply mesh handling where we guarantee at least padded normals/tex_coords"

  /* FIXME: assets should only be used in the Rig editor so we
   * shouldn't have to consider this... */
  if (!asset->ctx->headless)
    {
      asset->texture = generate_mesh_thumbnail (asset);
    }

  return asset;
}

RigAsset *
rig_asset_new_builtin (RutContext *ctx,
                       const char *path)
{
  return rig_asset_new_full (ctx, path, NULL, RIG_ASSET_TYPE_BUILTIN);
}

/* We should possibly report a GError here so we can report human
 * readable errors to the user... */
RigAsset *
rig_asset_new_texture (RutContext *ctx,
                       const char *path,
                       const GList *inferred_tags)
{
  return rig_asset_new_full (ctx, path, inferred_tags, RIG_ASSET_TYPE_TEXTURE);
}

/* We should possibly report a GError here so we can report human
 * readable errors to the user... */
RigAsset *
rig_asset_new_normal_map (RutContext *ctx,
                          const char *path,
                          const GList *inferred_tags)
{
  return rig_asset_new_full (ctx, path, inferred_tags,
                             RIG_ASSET_TYPE_NORMAL_MAP);
}

/* We should possibly report a GError here so we can report human
 * readable errors to the user... */
RigAsset *
rig_asset_new_alpha_mask (RutContext *ctx,
                          const char *path,
                          const GList *inferred_tags)
{
  return rig_asset_new_full (ctx, path, inferred_tags,
                             RIG_ASSET_TYPE_ALPHA_MASK);
}

RigAsset *
rig_asset_new_ply_model (RutContext *ctx,
                         const char *path,
                         const GList *inferred_tags)
{
  return rig_asset_new_full (ctx, path, inferred_tags,
                             RIG_ASSET_TYPE_MESH);
}

RigAssetType
rig_asset_get_type (RigAsset *asset)
{
  return asset->type;
}

const char *
rig_asset_get_path (RigAsset *asset)
{
  return asset->path;
}

RutContext *
rig_asset_get_context (RigAsset *asset)
{
  return asset->ctx;
}

CoglTexture *
rig_asset_get_texture (RigAsset *asset)
{
  return asset->texture;
}

RutMesh *
rig_asset_get_mesh (RigAsset *asset)
{
  return asset->mesh;
}

bool
rig_asset_get_is_video (RigAsset *asset)
{
  return asset->is_video;
}

static GList *
copy_tags (const GList *tags)
{
  const GList *l;
  GList *copy = NULL;
  for (l = tags; l; l = l->next)
    {
      const char *tag = g_intern_string (l->data);
      copy = g_list_prepend (copy, (char *)tag);
    }
  return copy;
}

void
rig_asset_set_inferred_tags (RigAsset *asset,
                             const GList *inferred_tags)
{
  asset->inferred_tags = g_list_concat (asset->inferred_tags,
                                        copy_tags (inferred_tags));
}

const GList *
rig_asset_get_inferred_tags (RigAsset *asset)
{
  return asset->inferred_tags;
}

bool
rig_asset_has_tag (RigAsset *asset, const char *tag)
{
  GList *l;

  for (l = asset->inferred_tags; l; l = l->next)
    if (strcmp (tag, l->data) == 0)
      return TRUE;
  return FALSE;
}

static const char *
get_extension (const char *path)
{
  const char *ext = strrchr (path, '.');
  return ext ? ext + 1 : NULL;
}

bool
rut_file_info_is_asset (GFileInfo *info, const char *name)
{
  const char *content_type = g_file_info_get_content_type (info);
  char *mime_type = g_content_type_get_mime_type (content_type);
  const char *ext;
  if (mime_type)
    {
      if (strncmp (mime_type, "image/", 6) == 0)
        {
          g_free (mime_type);
          return TRUE;
        }
      else if (strncmp (mime_type, "video/", 6) == 0)
        {
          g_free (mime_type);
          return TRUE;
        }
      g_free (mime_type);
    }

  ext = get_extension (name);
  if (ext && strcmp (ext, "ply") == 0)
    return TRUE;

  return FALSE;
}

GList *
rut_infer_asset_tags (RutContext *ctx, GFileInfo *info, GFile *asset_file)
{
  GFile *assets_dir = g_file_new_for_path (ctx->assets_location);
  GFile *dir = g_file_get_parent (asset_file);
  char *basename;
  const char *content_type = g_file_info_get_content_type (info);
  char *mime_type = g_content_type_get_mime_type (content_type);
  const char *ext;
  GList *inferred_tags = NULL;

  while (dir && !g_file_equal (assets_dir, dir))
    {
      basename = g_file_get_basename (dir);
      inferred_tags =
        g_list_prepend (inferred_tags, (char *)g_intern_string (basename));
      g_free (basename);
      dir = g_file_get_parent (dir);
    }

  if (mime_type)
    {
      if (strncmp (mime_type, "image/", 6) == 0)
        inferred_tags =
          g_list_prepend (inferred_tags, (char *)g_intern_string ("image"));

      if (strncmp (mime_type, "video/", 6) == 0)
        inferred_tags =
          g_list_prepend (inferred_tags, (char*) g_intern_string ("video"));

      inferred_tags =
        g_list_prepend (inferred_tags, (char *)g_intern_string ("img"));

      if (rut_util_find_tag (inferred_tags, "normal-maps"))
        {
          inferred_tags =
            g_list_prepend (inferred_tags,
                            (char *)g_intern_string ("map"));
          inferred_tags =
            g_list_prepend (inferred_tags,
                            (char *)g_intern_string ("normal-map"));
          inferred_tags =
            g_list_prepend (inferred_tags,
                            (char *)g_intern_string ("bump-map"));
        }
      else if (rut_util_find_tag (inferred_tags, "alpha-masks"))
        {
          inferred_tags =
            g_list_prepend (inferred_tags,
                            (char *)g_intern_string ("alpha-mask"));
          inferred_tags =
            g_list_prepend (inferred_tags,
                            (char *)g_intern_string ("mask"));
        }
      else if (rut_util_find_tag (inferred_tags, "image") ||
               rut_util_find_tag (inferred_tags, "video"))
        {
          inferred_tags =
            g_list_prepend (inferred_tags,
                           (char *)g_intern_string ("texture"));
        }
    }

  basename = g_file_get_basename (asset_file);
  ext = get_extension (basename);
  if (ext && strcmp (ext, "ply") == 0)
    {
      inferred_tags =
        g_list_prepend (inferred_tags, (char *)g_intern_string ("ply"));
      inferred_tags =
        g_list_prepend (inferred_tags, (char *)g_intern_string ("mesh"));
      inferred_tags =
        g_list_prepend (inferred_tags, (char *)g_intern_string ("model"));
      inferred_tags =
        g_list_prepend (inferred_tags, (char *)g_intern_string ("geometry"));
      inferred_tags =
        g_list_prepend (inferred_tags, (char *)g_intern_string ("geom"));
    }
  g_free (basename);

  return inferred_tags;
}

void
rig_asset_add_inferred_tag (RigAsset *asset,
                            const char *tag)
{
  asset->inferred_tags =
    g_list_prepend (asset->inferred_tags, (char *)g_intern_string (tag));
}

bool
rig_asset_needs_thumbnail (RigAsset *asset)
{
  return asset->is_video ? TRUE : FALSE;
}

RutClosure *
rig_asset_thumbnail (RigAsset *asset,
                     RutThumbnailCallback ready_callback,
                     void *user_data,
                     RutClosureDestroyCallback destroy_cb)
{
  RutClosure *closure;

  g_return_val_if_fail (rig_asset_needs_thumbnail (asset), NULL);

  closure = rut_closure_list_add (&asset->thumbnail_cb_list,
                                  ready_callback,
                                  user_data,
                                  destroy_cb);

  generate_video_thumbnail (asset);

  /* Make sure the thumnail wasn't simply generated synchronously to
   * make sure the closure is still valid. */
  g_warn_if_fail (!rut_list_empty (&asset->thumbnail_cb_list));

  return closure;
}

void *
rig_asset_get_data (RigAsset *asset)
{
  return asset->data;
}

size_t
rig_asset_get_data_len (RigAsset *asset)
{
  return asset->data_len;
}

bool
rig_asset_get_mesh_has_tex_coords (RigAsset *asset)
{
  return asset->has_tex_coords;
}

bool
rig_asset_get_mesh_has_normals (RigAsset *asset)
{
  return asset->has_normals;
}