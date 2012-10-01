#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>

#include <cogl/cogl.h>

#include <rut.h>

#include "rig-data.h"
#include "rig-transition.h"
#include "rig-load-save.h"
#include "rig-undo-journal.h"

//#define DEVICE_WIDTH 480.0
//#define DEVICE_HEIGHT 800.0
#define DEVICE_WIDTH 720.0
#define DEVICE_HEIGHT 1280.0

/*
 * Note: The size and padding for this circle texture have been carefully
 * chosen so it has a power of two size and we have enough padding to scale
 * down the circle to a size of 2 pixels and still have a 1 texel transparent
 * border which we rely on for anti-aliasing.
 */
#define CIRCLE_TEX_RADIUS 16
#define CIRCLE_TEX_PADDING 16

#define N_CUBES 5


typedef enum _Pass
{
  PASS_COLOR,
  PASS_SHADOW,
  PASS_DOF_DEPTH
} Pass;

typedef struct _PaintContext
{
  RutPaintContext _parent;

  RigData *data;

  GList *camera_stack;

  Pass pass;

} PaintContext;

static RutPropertySpec rut_data_property_specs[] = {
  {
    .name = "width",
    .type = RUT_PROPERTY_TYPE_FLOAT,
    .data_offset = offsetof (RigData, width)
  },
  {
    .name = "height",
    .type = RUT_PROPERTY_TYPE_FLOAT,
    .data_offset = offsetof (RigData, height)
  },
  { 0 }
};

#ifndef __ANDROID__

#ifdef RIG_EDITOR_ENABLED
CoglBool _rig_in_device_mode = FALSE;
#endif

static char **_rig_handset_remaining_args = NULL;

static const GOptionEntry rut_handset_entries[] =
{
#ifdef RIG_EDITOR_ENABLED
  { "device-mode", 'd', 0, 0,
    &_rig_in_device_mode, "Run in Device Mode" },
#endif
  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY,
    &_rig_handset_remaining_args, "Project" },
  { 0 }
};

static char *_rut_project_dir = NULL;

#endif /* __ANDROID__ */

static const float jitter_offsets[32] =
{
  0.375f, 0.4375f,
  0.625f, 0.0625f,
  0.875f, 0.1875f,
  0.125f, 0.0625f,

  0.375f, 0.6875f,
  0.875f, 0.4375f,
  0.625f, 0.5625f,
  0.375f, 0.9375f,

  0.625f, 0.3125f,
  0.125f, 0.5625f,
  0.125f, 0.8125f,
  0.375f, 0.1875f,

  0.875f, 0.9375f,
  0.875f, 0.6875f,
  0.125f, 0.3125f,
  0.625f, 0.8125f
};

/* XXX: This assumes that the primitive is being drawn in pixel coordinates,
 * since we jitter the modelview not the projection.
 */
static void
draw_jittered_primitive4f (RigData *data,
                           CoglFramebuffer *fb,
                           CoglPrimitive *prim,
                           float red,
                           float green,
                           float blue)
{
  CoglPipeline *pipeline = cogl_pipeline_new (data->ctx->cogl_context);
  int i;

  cogl_pipeline_set_color4f (pipeline,
                             red / 16.0f,
                             green / 16.0f,
                             blue / 16.0f,
                             1.0f / 16.0f);

  for (i = 0; i < 16; i++)
    {
      const float *offset = jitter_offsets + 2 * i;

      cogl_framebuffer_push_matrix (fb);
      cogl_framebuffer_translate (fb, offset[0], offset[1], 0);
      cogl_framebuffer_draw_primitive (fb, pipeline, prim);
      cogl_framebuffer_pop_matrix (fb);
    }

  cogl_object_unref (pipeline);
}

static void
camera_update_view (RigData *data, RutEntity *camera, Pass pass)
{
  RutCamera *camera_component =
    rut_entity_get_component (camera, RUT_COMPONENT_TYPE_CAMERA);
  CoglMatrix transform;
  CoglMatrix inverse_transform;
  CoglMatrix view;

  /* translate to z_2d and scale */
  if (pass != PASS_SHADOW)
    view = data->main_view;
  else
    view = data->identity;

  /* apply the camera viewing transform */
  rut_graphable_get_transform (camera, &transform);
  cogl_matrix_get_inverse (&transform, &inverse_transform);
  cogl_matrix_multiply (&view, &view, &inverse_transform);

  if (pass == PASS_SHADOW)
    {
      CoglMatrix flipped_view;
      cogl_matrix_init_identity (&flipped_view);
      cogl_matrix_scale (&flipped_view, 1, -1, 1);
      cogl_matrix_multiply (&flipped_view, &flipped_view, &view);
      rut_camera_set_view_transform (camera_component, &flipped_view);
    }
  else
    rut_camera_set_view_transform (camera_component, &view);
}

static void
get_normal_matrix (const CoglMatrix *matrix,
                   float *normal_matrix)
{
  CoglMatrix inverse_matrix;

  /* Invert the matrix */
  cogl_matrix_get_inverse (matrix, &inverse_matrix);

  /* Transpose it while converting it to 3x3 */
  normal_matrix[0] = inverse_matrix.xx;
  normal_matrix[1] = inverse_matrix.xy;
  normal_matrix[2] = inverse_matrix.xz;

  normal_matrix[3] = inverse_matrix.yx;
  normal_matrix[4] = inverse_matrix.yy;
  normal_matrix[5] = inverse_matrix.yz;

  normal_matrix[6] = inverse_matrix.zx;
  normal_matrix[7] = inverse_matrix.zy;
  normal_matrix[8] = inverse_matrix.zz;
}

static void
set_focal_parameters (CoglPipeline *pipeline,
                      float focal_distance,
                      float depth_of_field)
{
  int location;
  float distance;

  /* I want to have the focal distance as positive when it's in front of the
   * camera (it seems more natural, but as, in OpenGL, the camera is facing
   * the negative Ys, the actual value to give to the shader has to be
   * negated */
  distance = -focal_distance;

  location = cogl_pipeline_get_uniform_location (pipeline,
                                                 "dof_focal_distance");
  cogl_pipeline_set_uniform_float (pipeline,
                                   location,
                                   1 /* n_components */, 1 /* count */,
                                   &distance);

  location = cogl_pipeline_get_uniform_location (pipeline,
                                                 "dof_depth_of_field");
  cogl_pipeline_set_uniform_float (pipeline,
                                   location,
                                   1 /* n_components */, 1 /* count */,
                                   &depth_of_field);
}

static void
get_light_modelviewprojection (const CoglMatrix *model_transform,
                               RutEntity  *light,
                               const CoglMatrix *light_projection,
                               CoglMatrix *light_mvp)
{
  const CoglMatrix *light_transform;
  CoglMatrix light_view;

  /* TODO: cache the bias * light_projection * light_view matrix! */

  /* Move the unit data from [-1,1] to [0,1], column major order */
  float bias[16] = {
    .5f, .0f, .0f, .0f,
    .0f, .5f, .0f, .0f,
    .0f, .0f, .5f, .0f,
    .5f, .5f, .5f, 1.f
  };

  light_transform = rut_entity_get_transform (light);
  cogl_matrix_get_inverse (light_transform, &light_view);

  cogl_matrix_init_from_array (light_mvp, bias);
  cogl_matrix_multiply (light_mvp, light_mvp, light_projection);
  cogl_matrix_multiply (light_mvp, light_mvp, &light_view);

  cogl_matrix_multiply (light_mvp, light_mvp, model_transform);
}

CoglPipeline *
get_entity_pipeline (RigData *data,
                     RutEntity *entity,
                     RutComponent *geometry,
                     Pass pass)
{
  CoglSnippet *snippet;
  CoglDepthState depth_state;
  RutMaterial *material =
    rut_entity_get_component (entity, RUT_COMPONENT_TYPE_MATERIAL);
  CoglPipeline *pipeline;
  CoglFramebuffer *shadow_fb;

  if (pass == PASS_COLOR)
    {
      pipeline = rut_entity_get_pipeline_cache (entity);
      if (pipeline)
        {
          cogl_object_ref (pipeline);
          goto FOUND;
        }
    }
  else if (pass == PASS_DOF_DEPTH || pass == PASS_SHADOW)
    {
      if (!data->dof_pipeline_template)
        {
          CoglPipeline *pipeline;
          CoglDepthState depth_state;
          CoglSnippet *snippet;

          pipeline = cogl_pipeline_new (data->ctx->cogl_context);

          cogl_pipeline_set_color_mask (pipeline, COGL_COLOR_MASK_ALPHA);

          cogl_pipeline_set_blend (pipeline, "RGBA=ADD(SRC_COLOR, 0)", NULL);

          cogl_depth_state_init (&depth_state);
          cogl_depth_state_set_test_enabled (&depth_state, TRUE);
          cogl_pipeline_set_depth_state (pipeline, &depth_state, NULL);

          snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_VERTEX,

              /* definitions */
              "uniform float dof_focal_distance;\n"
              "uniform float dof_depth_of_field;\n"

              "varying float dof_blur;\n",
              //"varying vec4 world_pos;\n",

              /* compute the amount of bluriness we want */
              "vec4 world_pos = cogl_modelview_matrix * cogl_position_in;\n"
              //"world_pos = cogl_modelview_matrix * cogl_position_in;\n"
              "dof_blur = 1.0 - clamp (abs (world_pos.z - dof_focal_distance) /\n"
              "                  dof_depth_of_field, 0.0, 1.0);\n"
          );

          cogl_pipeline_add_snippet (pipeline, snippet);
          cogl_object_unref (snippet);

          /* This was used to debug the focal distance and bluriness amount in the DoF
           * effect: */
#if 0
          cogl_pipeline_set_color_mask (pipeline, COGL_COLOR_MASK_ALL);
          snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
              "varying vec4 world_pos;\n"
              "varying float dof_blur;",

             "cogl_color_out = vec4(dof_blur,0,0,1);\n"
             //"cogl_color_out = vec4(1.0, 0.0, 0.0, 1.0);\n"
             //"if (world_pos.z < -30.0) cogl_color_out = vec4(0,1,0,1);\n"
             //"if (abs (world_pos.z + 30.f) < 0.1) cogl_color_out = vec4(0,1,0,1);\n"
             "cogl_color_out.a = dof_blur;\n"
             //"cogl_color_out.a = 1.0;\n"
          );

          cogl_pipeline_add_snippet (pipeline, snippet);
          cogl_object_unref (snippet);
#endif

          data->dof_pipeline_template = pipeline;
        }

      if (rut_object_get_type (geometry) == &rut_diamond_type)
        {
          if (!data->dof_diamond_pipeline)
            {
              CoglPipeline *dof_diamond_pipeline =
                cogl_pipeline_copy (data->dof_pipeline_template);
              CoglSnippet *snippet;

              rut_diamond_apply_mask (RUT_DIAMOND (geometry), dof_diamond_pipeline);

              snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                                          /* declarations */
                                          "varying float dof_blur;",

                                          /* post */
                                          "if (cogl_color_out.a <= 0.0)\n"
                                          "  discard;\n"
                                          "\n"
                                          "cogl_color_out.a = dof_blur;\n");

              cogl_pipeline_add_snippet (dof_diamond_pipeline, snippet);
              cogl_object_unref (snippet);

              set_focal_parameters (dof_diamond_pipeline, 30.f, 3.0f);

              data->dof_diamond_pipeline = dof_diamond_pipeline;
            }

          return cogl_object_ref (data->dof_diamond_pipeline);
        }
      else
        {
          if (!data->dof_pipeline)
            {
              CoglPipeline *dof_pipeline =
                cogl_pipeline_copy (data->dof_pipeline_template);
              CoglSnippet *snippet;

              /* store the bluriness in the alpha channel */
              snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                  "varying float dof_blur;",

                  "cogl_color_out.a = dof_blur;\n"
              );
              cogl_pipeline_add_snippet (dof_pipeline, snippet);
              cogl_object_unref (snippet);

              set_focal_parameters (dof_pipeline, 30.f, 3.0f);

              data->dof_pipeline = dof_pipeline;
            }

          return cogl_object_ref (data->dof_pipeline);
        }
    }

  pipeline = cogl_pipeline_new (data->ctx->cogl_context);

#if 0
  /* NB: Our texture colours aren't premultiplied */
  cogl_pipeline_set_blend (pipeline,
                           "RGB = ADD(SRC_COLOR*(SRC_COLOR[A]), DST_COLOR*(1-SRC_COLOR[A]))"
                           "A   = ADD(SRC_COLOR, DST_COLOR*(1-SRC_COLOR[A]))",
                           NULL);
#endif

#if 0
  if (rut_object_get_type (geometry) == &rut_diamond_type)
    rut_geometry_component_update_pipeline (geometry, pipeline);

  for (l = data->lights; l; l = l->next)
    light_update_pipeline (l->data, pipeline);

  pipeline = cogl_pipeline_new (rut_cogl_context);
#endif

  cogl_pipeline_set_color4f (pipeline, 0.8f, 0.8f, 0.8f, 1.f);

  /* enable depth testing */
  cogl_depth_state_init (&depth_state);
  cogl_depth_state_set_test_enabled (&depth_state, TRUE);
  cogl_pipeline_set_depth_state (pipeline, &depth_state, NULL);

  /* Vertex shader setup for lighting */
  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_VERTEX,

      /* definitions */
      "uniform mat3 normal_matrix;\n"
      "varying vec3 normal_direction, eye_direction;\n",

      /* post */
      "normal_direction = normalize(normal_matrix * cogl_normal_in);\n"
      //"normal_direction = cogl_normal_in;\n"
      "eye_direction    = -vec3(cogl_modelview_matrix * cogl_position_in);\n"
  );

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  /* Vertex shader setup for shadow mapping */
  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_VERTEX,

      /* definitions */
      "uniform mat4 light_shadow_matrix;\n"
      "varying vec4 shadow_coords;\n",

      /* post */
      "shadow_coords = light_shadow_matrix * cogl_position_in;\n"
  );

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  /* and fragment shader */
  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
      /* definitions */
      //"varying vec3 normal_direction;\n",
      "varying vec3 normal_direction, eye_direction;\n",
      /* post */
      "");
  //cogl_snippet_set_pre (snippet, "cogl_color_out = cogl_color_in;\n");

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
      /* definitions */
      "uniform vec4 light0_ambient, light0_diffuse, light0_specular;\n"
      "uniform vec3 light0_direction_norm;\n",

      /* post */
      "vec4 final_color;\n"

      "vec3 L = light0_direction_norm;\n"
      "vec3 N = normalize(normal_direction);\n"

      "if (cogl_color_out.a <= 0.0)\n"
      "  discard;\n"

      "final_color = light0_ambient * cogl_color_out;\n"
      "float lambert = dot(N, L);\n"
      //"float lambert = 1.0;\n"

      "if (lambert > 0.0)\n"
      "{\n"
      "  final_color += cogl_color_out * light0_diffuse * lambert;\n"
      //"  final_color +=  vec4(1.0, 0.0, 0.0, 1.0) * light0_diffuse * lambert;\n"

      "  vec3 E = normalize(eye_direction);\n"
      "  vec3 R = reflect (-L, N);\n"
      "  float specular = pow (max(dot(R, E), 0.0),\n"
      "                        2.);\n"
      "  final_color += light0_specular * vec4(.6, .6, .6, 1.0) * specular;\n"
      "}\n"

      "cogl_color_out = final_color;\n"
      //"cogl_color_out = vec4(1.0, 0.0, 0.0, 1.0);\n"
  );

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);


  /* Hook the shadow map sampling */

  cogl_pipeline_set_layer_texture (pipeline, 7, data->shadow_map);
  /* For debugging the shadow mapping... */
  //cogl_pipeline_set_layer_texture (pipeline, 7, data->shadow_color);
  //cogl_pipeline_set_layer_texture (pipeline, 7, data->gradient);

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_TEXTURE_LOOKUP,
                              /* declarations */
                              "varying vec4 shadow_coords;\n",
                              /* post */
                              "");

  cogl_snippet_set_replace (snippet,
                            "cogl_texel = texture2D(cogl_sampler7, cogl_tex_coord.st);\n");

  cogl_pipeline_add_layer_snippet (pipeline, 7, snippet);
  cogl_object_unref (snippet);

  /* Handle shadow mapping */

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
      /* declarations */
      "",

      /* post */
      "cogl_texel7 =  cogl_texture_lookup7 (cogl_sampler7, shadow_coords);\n"
      "float distance_from_light = cogl_texel7.z + 0.0005;\n"
      "float shadow = 1.0;\n"
      "if (distance_from_light < shadow_coords.z)\n"
      "  shadow = 0.5;\n"

      "cogl_color_out = shadow * cogl_color_out;\n"
  );

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

#if 1
  {
    RutLight *light = rut_entity_get_component (data->light, RUT_COMPONENT_TYPE_LIGHT);
    rut_light_set_uniforms (light, pipeline);
  }
#endif

#if 1
  if (rut_object_get_type (geometry) == &rut_diamond_type)
    {
      //pipeline = cogl_pipeline_new (data->ctx->cogl_context);

      //cogl_pipeline_set_depth_state (pipeline, &depth_state, NULL);

      rut_diamond_apply_mask (RUT_DIAMOND (geometry), pipeline);

      //cogl_pipeline_set_color4f (pipeline, 1, 0, 0, 1);

#if 1
      if (material)
        {
          RutAsset *asset = rut_material_get_asset (material);
          CoglTexture *texture;
          if (asset)
            texture = rut_asset_get_texture (asset);
          else
            texture = NULL;

          if (texture)
            cogl_pipeline_set_layer_texture (pipeline, 1, texture);
        }
#endif
    }
#endif

  rut_entity_set_pipeline_cache (entity, pipeline);

FOUND:

  /* FIXME: there's lots to optimize about this! */
#if 1
  shadow_fb = COGL_FRAMEBUFFER (data->shadow_fb);

  /* update uniforms in pipelines */
  {
    CoglMatrix light_shadow_matrix, light_projection;
    CoglMatrix model_transform;
    const float *light_matrix;
    int location;

    cogl_framebuffer_get_projection_matrix (shadow_fb, &light_projection);

    /* XXX: This is pretty bad that we are having to do this. It would
     * be nicer if cogl exposed matrix-stacks publicly so we could
     * maintain the entity model_matrix incrementally as we traverse
     * the scenegraph. */
    rut_graphable_get_transform (entity, &model_transform);

    get_light_modelviewprojection (&model_transform,
                                   data->light,
                                   &light_projection,
                                   &light_shadow_matrix);

    light_matrix = cogl_matrix_get_array (&light_shadow_matrix);

    location = cogl_pipeline_get_uniform_location (pipeline,
                                                   "light_shadow_matrix");
    cogl_pipeline_set_uniform_matrix (pipeline,
                                      location,
                                      4, 1,
                                      FALSE,
                                      light_matrix);
  }
#endif

  return pipeline;
}

static void
draw_entity_camera_frustum (RigData *data,
                            RutEntity *entity,
                            CoglFramebuffer *fb)
{
  RutCamera *camera =
    rut_entity_get_component (entity, RUT_COMPONENT_TYPE_CAMERA);
  CoglPrimitive *primitive = rut_camera_create_frustum_primitive (camera);
  CoglPipeline *pipeline = cogl_pipeline_new (rut_cogl_context);
  CoglDepthState depth_state;

  /* enable depth testing */
  cogl_depth_state_init (&depth_state);
  cogl_depth_state_set_test_enabled (&depth_state, TRUE);
  cogl_pipeline_set_depth_state (pipeline, &depth_state, NULL);

  cogl_framebuffer_draw_primitive (fb, pipeline, primitive);

  cogl_object_unref (primitive);
  cogl_object_unref (pipeline);
}

static RutTraverseVisitFlags
entitygraph_pre_paint_cb (RutObject *object,
                          int depth,
                          void *user_data)
{
  PaintContext *paint_ctx = user_data;
  RutPaintContext *rut_paint_ctx = user_data;
  RutCamera *camera = rut_paint_ctx->camera;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (camera);

  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      const CoglMatrix *matrix = rut_transformable_get_matrix (object);
      cogl_framebuffer_push_matrix (fb);
      cogl_framebuffer_transform (fb, matrix);
    }

  if (rut_object_get_type (object) == &rut_entity_type)
    {
      RutEntity *entity = RUT_ENTITY (object);
      RutComponent *geometry;
      CoglPipeline *pipeline;
      CoglPrimitive *primitive;
      CoglMatrix modelview_matrix;
      float normal_matrix[9];

      if (!rut_entity_get_visible (entity) ||
          (paint_ctx->pass == PASS_SHADOW && !rut_entity_get_cast_shadow (entity)))
        return RUT_TRAVERSE_VISIT_CONTINUE;

      geometry =
        rut_entity_get_component (object, RUT_COMPONENT_TYPE_GEOMETRY);
      if (!geometry)
        {
          if (!paint_ctx->data->play_mode &&
              object == paint_ctx->data->light)
            draw_entity_camera_frustum (paint_ctx->data, object, fb);
          return RUT_TRAVERSE_VISIT_CONTINUE;
        }

#if 1
      pipeline = get_entity_pipeline (paint_ctx->data,
                                      object,
                                      geometry,
                                      paint_ctx->pass);
#endif

      primitive = rut_primable_get_primitive (geometry);

#if 1
      cogl_framebuffer_get_modelview_matrix (fb, &modelview_matrix);
      get_normal_matrix (&modelview_matrix, normal_matrix);

      {
        int location = cogl_pipeline_get_uniform_location (pipeline, "normal_matrix");
        cogl_pipeline_set_uniform_matrix (pipeline,
                                          location,
                                          3, /* dimensions */
                                          1, /* count */
                                          FALSE, /* don't transpose again */
                                          normal_matrix);
      }
#endif

      cogl_framebuffer_draw_primitive (fb,
                                       pipeline,
                                       primitive);

      /* FIXME: cache the pipeline with the entity */
      cogl_object_unref (pipeline);

#if 0
      geometry = rut_entity_get_component (object, RUT_COMPONENT_TYPE_GEOMETRY);
      material = rut_entity_get_component (object, RUT_COMPONENT_TYPE_MATERIAL);
      if (geometry && material)
        {
          if (rut_object_get_type (geometry) == &rut_diamond_type)
            {
              PaintContext *paint_ctx = rut_paint_ctx;
              RigData *data = paint_ctx->data;
              RutDiamondSlice *slice = rut_diamond_get_slice (geometry);
              CoglPipeline *template = rut_diamond_slice_get_pipeline_template (slice);
              CoglPipeline *material_pipeline = rut_material_get_pipeline (material);
              CoglPipeline *pipeline = cogl_pipeline_copy (template);
              //CoglPipeline *pipeline = cogl_pipeline_new (data->ctx->cogl_context);

              /* FIXME: we should be combining the material and
               * diamond slice state together before now! */
              cogl_pipeline_set_layer_texture (pipeline, 1,
                                               cogl_pipeline_get_layer_texture (material_pipeline, 0));

              cogl_framebuffer_draw_primitive (fb,
                                               pipeline,
                                               slice->primitive);

              cogl_object_unref (pipeline);
            }
        }
#endif
      return RUT_TRAVERSE_VISIT_CONTINUE;
    }

  /* XXX:
   * How can we maintain state between the pre and post stages?  Is it
   * ok to just "sub-class" the paint context and maintain a stack of
   * state that needs to be shared with the post paint code.
   */

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static RutTraverseVisitFlags
entitygraph_post_paint_cb (RutObject *object,
                           int depth,
                           void *user_data)
{
  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      RutPaintContext *rut_paint_ctx = user_data;
      CoglFramebuffer *fb = rut_camera_get_framebuffer (rut_paint_ctx->camera);
      cogl_framebuffer_pop_matrix (fb);
    }

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static void
paint_scene (PaintContext *paint_ctx)
{
  RutPaintContext *rut_paint_ctx = &paint_ctx->_parent;
  RigData *data = paint_ctx->data;
  CoglContext *ctx = data->ctx->cogl_context;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (rut_paint_ctx->camera);

  if (paint_ctx->pass == PASS_COLOR)
    {
      CoglPipeline *pipeline = cogl_pipeline_new (ctx);
      cogl_pipeline_set_color4f (pipeline, 0, 0, 0, 1.0);
      cogl_framebuffer_draw_rectangle (fb,
                                       pipeline,
                                       0, 0, DEVICE_WIDTH, DEVICE_HEIGHT);
                                       //0, 0, data->pane_width, data->pane_height);
      cogl_object_unref (pipeline);
    }

  rut_graphable_traverse (data->scene,
                          RUT_TRAVERSE_DEPTH_FIRST,
                          entitygraph_pre_paint_cb,
                          entitygraph_post_paint_cb,
                          paint_ctx);

}

#if 1
static void
paint_camera_entity (RutEntity *camera, PaintContext *paint_ctx)
{
  RutPaintContext *rut_paint_ctx = &paint_ctx->_parent;
  RutCamera *save_camera = rut_paint_ctx->camera;
  RutCamera *camera_component =
    rut_entity_get_component (camera, RUT_COMPONENT_TYPE_CAMERA);
  RigData *data = paint_ctx->data;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (camera_component);
  //CoglFramebuffer *shadow_fb;

  rut_paint_ctx->camera = camera_component;

  if (rut_entity_get_component (camera, RUT_COMPONENT_TYPE_LIGHT))
    paint_ctx->pass = PASS_SHADOW;
  else
    paint_ctx->pass = PASS_COLOR;

  camera_update_view (data, camera, paint_ctx->pass);

  if (paint_ctx->pass != PASS_SHADOW &&
      data->enable_dof)
    {
      const float *viewport = rut_camera_get_viewport (camera_component);
      int width = viewport[2];
      int height = viewport[3];
      int save_viewport_x = viewport[0];
      int save_viewport_y = viewport[1];
      Pass save_pass = paint_ctx->pass;
      CoglFramebuffer *pass_fb;

      rut_camera_set_viewport (camera_component, 0, 0, width, height);

      rut_dof_effect_set_framebuffer_size (data->dof, width, height);

      pass_fb = rut_dof_effect_get_depth_pass_fb (data->dof);
      rut_camera_set_framebuffer (camera_component, pass_fb);

      rut_camera_flush (camera_component);
      cogl_framebuffer_clear4f (pass_fb,
                                COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                                1, 1, 1, 1);

      paint_ctx->pass = PASS_DOF_DEPTH;
      paint_scene (paint_ctx);
      paint_ctx->pass = save_pass;

      rut_camera_end_frame (camera_component);

      pass_fb = rut_dof_effect_get_color_pass_fb (data->dof);
      rut_camera_set_framebuffer (camera_component, pass_fb);

      rut_camera_flush (camera_component);
      cogl_framebuffer_clear4f (pass_fb,
                                COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                                0.22, 0.22, 0.22, 1);

      paint_ctx->pass = PASS_COLOR;
      paint_scene (paint_ctx);
      paint_ctx->pass = save_pass;

      rut_camera_end_frame (camera_component);

      rut_camera_set_framebuffer (camera_component, fb);
      rut_camera_set_clear (camera_component, FALSE);

      rut_camera_flush (camera_component);

      rut_camera_end_frame (camera_component);

      rut_camera_set_viewport (camera_component,
                               save_viewport_x,
                               save_viewport_y,
                               width, height);
      rut_paint_ctx->camera = save_camera;
      rut_camera_flush (save_camera);
      rut_dof_effect_draw_rectangle (data->dof,
                                     rut_camera_get_framebuffer (save_camera),
                                     data->main_x,
                                     data->main_y,
                                     data->main_x + data->main_width,
                                     data->main_y + data->main_height);
      rut_camera_end_frame (save_camera);
    }
  else
    {
      rut_camera_set_framebuffer (camera_component, fb);

      rut_camera_flush (camera_component);

      paint_scene (paint_ctx);

      rut_camera_end_frame (camera_component);
    }

  if (paint_ctx->pass == PASS_COLOR)
    {
      rut_camera_flush (camera_component);

      /* Use this to visualize the depth-of-field alpha buffer... */
#if 0
      CoglPipeline *pipeline = cogl_pipeline_new (data->ctx->cogl_context);
      cogl_pipeline_set_layer_texture (pipeline, 0, data->dof.depth_pass);
      cogl_pipeline_set_blend (pipeline, "RGBA=ADD(SRC_COLOR, 0)", NULL);
      cogl_framebuffer_draw_rectangle (fb,
                                       pipeline,
                                       0, 0,
                                       200, 200);
#endif

      /* Use this to visualize the shadow_map */
#if 0
      CoglPipeline *pipeline = cogl_pipeline_new (data->ctx->cogl_context);
      cogl_pipeline_set_layer_texture (pipeline, 0, data->shadow_map);
      //cogl_pipeline_set_layer_texture (pipeline, 0, data->shadow_color);
      cogl_pipeline_set_blend (pipeline, "RGBA=ADD(SRC_COLOR, 0)", NULL);
      cogl_framebuffer_draw_rectangle (fb,
                                       pipeline,
                                       0, 0,
                                       200, 200);
#endif

      if (data->debug_pick_ray && data->picking_ray)
      //if (data->picking_ray)
        {
          cogl_framebuffer_draw_primitive (fb,
                                           data->picking_ray_color,
                                           data->picking_ray);
        }

#ifdef RIG_EDITOR_ENABLED
      if (!_rig_in_device_mode)
        {
          draw_jittered_primitive4f (data, fb, data->grid_prim, 0.5, 0.5, 0.5);

          if (data->selected_entity)
            {
              rut_tool_update (data->tool, data->selected_entity);
              rut_tool_draw (data->tool, fb);
            }
        }
#endif /* RIG_EDITOR_ENABLED */

      rut_camera_end_frame (camera_component);
    }

  rut_paint_ctx->camera = save_camera;
}
#endif

typedef struct
{
  CoglPipeline *pipeline;
  RutEntity *entity;
  PaintContext *paint_ctx;

  float viewport_x, viewport_y;
  float viewport_t_scale;
  float viewport_y_scale;
  float viewport_t_offset;
  float viewport_y_offset;
} PaintTimelineData;

static void
paint_timeline_path_cb (RutProperty *property,
                        RigPath *path,
                        const RutBoxed *constant_value,
                        void *user_data)
{
  PaintTimelineData *paint_data = user_data;
  RutPaintContext *rut_paint_ctx = &paint_data->paint_ctx->_parent;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (rut_paint_ctx->camera);
  RigData *data = paint_data->paint_ctx->data;
  RutContext *ctx = data->ctx;
  CoglPrimitive *prim;
  GArray *points;
  GList *l;
  GList *next;
  float red, green, blue;

  if (path == NULL ||
      property->object != paint_data->entity ||
      property->spec->type != RUT_PROPERTY_TYPE_FLOAT)
    return;

  if (strcmp (property->spec->name, "x") == 0)
    red = 1.0, green = 0.0, blue = 0.0;
  else if (strcmp (property->spec->name, "y") == 0)
    red = 0.0, green = 1.0, blue = 0.0;
  else if (strcmp (property->spec->name, "z") == 0)
    red = 0.0, green = 0.0, blue = 1.0;
  else
    return;

  points = g_array_new (FALSE, FALSE, sizeof (CoglVertexP2));

  for (l = path->nodes.head; l; l = next)
    {
      RigNodeFloat *f_node = l->data;
      CoglVertexP2 p;

      next = l->next;

      /* FIXME: This clipping wasn't working... */
#if 0
      /* Only draw the nodes within the current viewport */
      if (next)
        {
          float max_t = (viewport_t_offset +
                         data->timeline_vp->width * viewport_t_scale);
          if (next->t < viewport_t_offset)
            continue;
          if (node->t > max_t && next->t > max_t)
            break;
        }
#endif

#define HANDLE_HALF_SIZE 4
      p.x = (paint_data->viewport_x +
             (f_node->t - paint_data->viewport_t_offset) *
             paint_data->viewport_t_scale);

      cogl_pipeline_set_color4f (paint_data->pipeline, red, green, blue, 1);

      p.y = (paint_data->viewport_y +
             (f_node->value - paint_data->viewport_y_offset) *
             paint_data->viewport_y_scale);
#if 1
#if 1
      cogl_framebuffer_push_matrix (fb);
      cogl_framebuffer_translate (fb, p.x, p.y, 0);
      cogl_framebuffer_scale (fb, HANDLE_HALF_SIZE, HANDLE_HALF_SIZE, 0);
      cogl_framebuffer_draw_attributes (fb,
                                        paint_data->pipeline,
                                        COGL_VERTICES_MODE_LINE_STRIP,
                                        1,
                                        data->circle_node_n_verts - 1,
                                        &data->circle_node_attribute,
                                        1);
      cogl_framebuffer_pop_matrix (fb);
#else
#if 0
      cogl_framebuffer_draw_rectangle (fb,
                                       pipeline,
                                       p.x - HANDLE_HALF_SIZE,
                                       p.y - HANDLE_HALF_SIZE,
                                       p.x + HANDLE_HALF_SIZE,
                                       p.y + HANDLE_HALF_SIZE);
#endif
#endif
#endif
      g_array_append_val (points, p);
    }

  prim = cogl_primitive_new_p2 (ctx->cogl_context,
                                COGL_VERTICES_MODE_LINE_STRIP,
                                points->len, (CoglVertexP2 *)points->data);
  draw_jittered_primitive4f (data, fb, prim, red, green, blue);
  cogl_object_unref (prim);

  g_array_free (points, TRUE);
}

static void
paint_timeline_camera (PaintContext *paint_ctx)
{
  RutPaintContext *rut_paint_ctx = &paint_ctx->_parent;
  RigData *data = paint_ctx->data;
  RutContext *ctx = data->ctx;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (rut_paint_ctx->camera);

  //cogl_framebuffer_push_matrix (fb);
  //cogl_framebuffer_transform (fb, rut_transformable_get_matrix (camera));

  if (data->selected_entity)
    {
      PaintTimelineData paint_data;

      paint_data.paint_ctx = paint_ctx;

      paint_data.entity = data->selected_entity;
      paint_data.viewport_x = 0;
      paint_data.viewport_y = 0;

      paint_data.viewport_t_scale =
        rut_ui_viewport_get_doc_scale_x (data->timeline_vp) *
        data->timeline_scale;

      paint_data.viewport_y_scale =
        rut_ui_viewport_get_doc_scale_y (data->timeline_vp) *
        data->timeline_scale;

      paint_data.viewport_t_offset =
        rut_ui_viewport_get_doc_x (data->timeline_vp);
      paint_data.viewport_y_offset =
        rut_ui_viewport_get_doc_y (data->timeline_vp);
      paint_data.pipeline = cogl_pipeline_new (data->ctx->cogl_context);

      rig_transition_foreach_property (data->selected_transition,
                                       paint_timeline_path_cb,
                                       &paint_data);

      cogl_object_unref (paint_data.pipeline);

      {
        CoglPrimitive *prim;
        double progress;
        float progress_x;
        float progress_line[4];

        progress = rut_timeline_get_progress (data->timeline);

        progress_x =
          -paint_data.viewport_t_offset *
          paint_data.viewport_t_scale +
          data->timeline_width *
          data->timeline_scale * progress;

        progress_line[0] = progress_x;
        progress_line[1] = 0;
        progress_line[2] = progress_x;
        progress_line[3] = data->timeline_height;

        prim = cogl_primitive_new_p2 (ctx->cogl_context,
                                      COGL_VERTICES_MODE_LINE_STRIP,
                                      2,
                                      (CoglVertexP2 *)progress_line);
        draw_jittered_primitive4f (data, fb, prim, 0, 1, 0);
        cogl_object_unref (prim);
      }
    }

  //cogl_framebuffer_pop_matrix (fb);
}

static RutTraverseVisitFlags
scenegraph_pre_paint_cb (RutObject *object,
                         int depth,
                         void *user_data)
{
  RutPaintContext *rut_paint_ctx = user_data;
  RutCamera *camera = rut_paint_ctx->camera;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (camera);
  RutPaintableVTable *vtable =
    rut_object_get_vtable (object, RUT_INTERFACE_ID_PAINTABLE);

#if 0
  if (rut_object_get_type (object) == &rut_camera_type)
    {
      g_print ("%*sCamera = %p\n", depth, "", object);
      rut_camera_flush (RUT_CAMERA (object));
      return RUT_TRAVERSE_VISIT_CONTINUE;
    }
  else
#endif

  if (rut_object_get_type (object) == &rut_ui_viewport_type)
    {
      RutUIViewport *ui_viewport = RUT_UI_VIEWPORT (object);
#if 0
      g_print ("%*sPushing clip = %f %f\n",
               depth, "",
               rut_ui_viewport_get_width (ui_viewport),
               rut_ui_viewport_get_height (ui_viewport));
#endif
      cogl_framebuffer_push_rectangle_clip (fb,
                                            0, 0,
                                            rut_ui_viewport_get_width (ui_viewport),
                                            rut_ui_viewport_get_height (ui_viewport));
    }

  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      //g_print ("%*sTransformable = %p\n", depth, "", object);
      const CoglMatrix *matrix = rut_transformable_get_matrix (object);
      //cogl_debug_matrix_print (matrix);
      cogl_framebuffer_push_matrix (fb);
      cogl_framebuffer_transform (fb, matrix);
    }

  if (rut_object_is (object, RUT_INTERFACE_ID_PAINTABLE))
    vtable->paint (object, rut_paint_ctx);

  /* XXX:
   * How can we maintain state between the pre and post stages?  Is it
   * ok to just "sub-class" the paint context and maintain a stack of
   * state that needs to be shared with the post paint code.
   */

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static RutTraverseVisitFlags
scenegraph_post_paint_cb (RutObject *object,
                          int depth,
                          void *user_data)
{
  RutPaintContext *rut_paint_ctx = user_data;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (rut_paint_ctx->camera);

#if 0
  if (rut_object_get_type (object) == &rut_camera_type)
    {
      rut_camera_end_frame (RUT_CAMERA (object));
      return RUT_TRAVERSE_VISIT_CONTINUE;
    }
  else
#endif

  if (rut_object_get_type (object) == &rut_ui_viewport_type)
    {
      cogl_framebuffer_pop_clip (fb);
    }

  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      cogl_framebuffer_pop_matrix (fb);
    }

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static CoglBool
paint (RutShell *shell, void *user_data)
{
  RigData *data = user_data;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (data->camera);
  PaintContext paint_ctx;
  RutPaintContext *rut_paint_ctx = &paint_ctx._parent;

  cogl_framebuffer_clear4f (fb,
                            COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                            0.22, 0.22, 0.22, 1);

  paint_ctx.data = data;
  paint_ctx.pass = PASS_COLOR;

  rut_paint_ctx->camera = data->camera;

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_camera_flush (data->camera);
      rut_graphable_traverse (data->root,
                              RUT_TRAVERSE_DEPTH_FIRST,
                              scenegraph_pre_paint_cb,
                              scenegraph_post_paint_cb,
                              &paint_ctx);
      /* FIXME: this should be moved to the end of this function but we
       * currently get warnings about unbalanced _flush()/_end_frame()
       * pairs. */
      rut_camera_end_frame (data->camera);
    }
#endif

  rut_paint_ctx->camera = data->camera;
  paint_camera_entity (data->light, &paint_ctx);

  rut_paint_ctx->camera = data->camera;
  paint_camera_entity (data->editor_camera, &paint_ctx);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_paint_ctx->camera = data->timeline_camera;
      rut_camera_flush (rut_paint_ctx->camera);
      paint_timeline_camera (&paint_ctx);
      rut_camera_end_frame (rut_paint_ctx->camera);
    }
#endif

#if 0
  rut_paint_ctx->camera = data->editor_camera;

  rut_graphable_traverse (data->editor_camera,
                          RUT_TRAVERSE_DEPTH_FIRST,
                          scenegraph_pre_paint_cb,
                          scenegraph_post_paint_cb,
                          &paint_ctx);
#endif

  cogl_onscreen_swap_buffers (COGL_ONSCREEN (fb));

  return FALSE;
}

static void
update_transition_progress_cb (RutProperty *target_property,
                               RutProperty *source_property,
                               void *user_data)
{
  RigData *data = user_data;
  double elapsed = rut_timeline_get_elapsed (data->timeline);
  RigTransition *transition = target_property->object;

  rig_transition_set_progress (transition, elapsed);
}

RigTransition *
rig_create_transition (RigData *data,
                       uint32_t id)
{
  RigTransition *transition = rig_transition_new (data->ctx, id);

  /* FIXME: this should probably only update the progress for the
   * current transition */
  rut_property_set_binding (&transition->props[RUT_TRANSITION_PROP_PROGRESS],
                            update_transition_progress_cb,
                            data,
                            data->timeline_elapsed,
                            NULL);

  return transition;
}

static void
unproject_window_coord (RutCamera *camera,
                        const CoglMatrix *modelview,
                        const CoglMatrix *inverse_modelview,
                        float object_coord_z,
                        float *x,
                        float *y)
{
  const CoglMatrix *projection = rut_camera_get_projection (camera);
  const CoglMatrix *inverse_projection =
    rut_camera_get_inverse_projection (camera);
  //float z;
  float ndc_x, ndc_y, ndc_z, ndc_w;
  float eye_x, eye_y, eye_z, eye_w;
  const float *viewport = rut_camera_get_viewport (camera);

  /* Convert object coord z into NDC z */
  {
    float tmp_x, tmp_y, tmp_z;
    const CoglMatrix *m = modelview;
    float z, w;

    tmp_x = m->xz * object_coord_z + m->xw;
    tmp_y = m->yz * object_coord_z + m->yw;
    tmp_z = m->zz * object_coord_z + m->zw;

    m = projection;
    z = m->zx * tmp_x + m->zy * tmp_y + m->zz * tmp_z + m->zw;
    w = m->wx * tmp_x + m->wy * tmp_y + m->wz * tmp_z + m->ww;

    ndc_z = z / w;
  }

  /* Undo the Viewport transform, putting us in Normalized Device Coords */
  ndc_x = (*x - viewport[0]) * 2.0f / viewport[2] - 1.0f;
  ndc_y = ((viewport[3] - 1 + viewport[1] - *y) * 2.0f / viewport[3] - 1.0f);

  /* Undo the Projection, putting us in Eye Coords. */
  ndc_w = 1;
  cogl_matrix_transform_point (inverse_projection,
                               &ndc_x, &ndc_y, &ndc_z, &ndc_w);
  eye_x = ndc_x / ndc_w;
  eye_y = ndc_y / ndc_w;
  eye_z = ndc_z / ndc_w;
  eye_w = 1;

  /* Undo the Modelview transform, putting us in Object Coords */
  cogl_matrix_transform_point (inverse_modelview,
                               &eye_x,
                               &eye_y,
                               &eye_z,
                               &eye_w);

  *x = eye_x;
  *y = eye_y;
  //*z = eye_z;
}

typedef void (*EntityTranslateCallback)(RutEntity *entity,
                                        float start[3],
                                        float rel[3],
                                        void *user_data);

typedef void (*EntityTranslateDoneCallback)(RutEntity *entity,
                                            float start[3],
                                            float rel[3],
                                            void *user_data);

typedef struct _EntityTranslateGrabClosure
{
  RigData *data;

  /* pointer position at start of grab */
  float grab_x;
  float grab_y;

  /* entity position at start of grab */
  float entity_grab_pos[3];
  RutEntity *entity;

  float x_vec[3];
  float y_vec[3];

  EntityTranslateCallback entity_translate_cb;
  EntityTranslateDoneCallback entity_translate_done_cb;
  void *user_data;
} EntityTranslateGrabClosure;

static RutInputEventStatus
entity_translate_grab_input_cb (RutInputEvent *event,
                                void *user_data)

{
  EntityTranslateGrabClosure *closure = user_data;
  RutEntity *entity = closure->entity;
  RigData *data = closure->data;

  g_print ("Entity grab event\n");

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      float x = rut_motion_event_get_x (event);
      float y = rut_motion_event_get_y (event);
      float move_x, move_y;
      float rel[3];
      float *x_vec = closure->x_vec;
      float *y_vec = closure->y_vec;

      move_x = x - closure->grab_x;
      move_y = y - closure->grab_y;

      rel[0] = x_vec[0] * move_x;
      rel[1] = x_vec[1] * move_x;
      rel[2] = x_vec[2] * move_x;

      rel[0] += y_vec[0] * move_y;
      rel[1] += y_vec[1] * move_y;
      rel[2] += y_vec[2] * move_y;

      if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_UP)
        {
          if (closure->entity_translate_done_cb)
            closure->entity_translate_done_cb (entity,
                                               closure->entity_grab_pos,
                                               rel,
                                               closure->user_data);

          rut_shell_ungrab_input (data->ctx->shell,
                                  entity_translate_grab_input_cb,
                                  user_data);

          g_slice_free (EntityTranslateGrabClosure, user_data);

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_MOVE)
        {
          closure->entity_translate_cb (entity,
                                        closure->entity_grab_pos,
                                        rel,
                                        closure->user_data);

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static void
inspector_property_changed_cb (RutProperty *target_property,
                               RutProperty *source_property,
                               void *user_data)
{
  RigData *data = user_data;
  RutBoxed new_value;

  rut_property_box (source_property, &new_value);

  rig_undo_journal_set_property_and_log (data->undo_journal,
                                         TRUE, /* mergable */
                                         data->selected_entity,
                                         &new_value,
                                         target_property);

  rut_boxed_destroy (&new_value);
}

typedef struct _AddComponentState
{
  RigData *data;
  int y_offset;
} AddComponentState;

static void
add_component_inspector_cb (RutComponent *component,
                            void *user_data)
{
  AddComponentState *state = user_data;
  RigData *data = state->data;
  RutInspector *inspector = rut_inspector_new (data->ctx,
                                               component,
                                               inspector_property_changed_cb,
                                               data);
  RutTransform *transform = rut_transform_new (data->ctx, inspector, NULL);
  float width, height;
  RutObject *doc_node;

  rut_refable_unref (inspector);

  rut_sizable_get_preferred_width (inspector,
                                   -1, /* for height */
                                   NULL, /* min_width */
                                   &width);
  rut_sizable_get_preferred_height (inspector,
                                    -1, /* for width */
                                    NULL, /* min_height */
                                    &height);
  rut_sizable_set_size (inspector, width, height);

  doc_node = rut_ui_viewport_get_doc_node (data->tool_vp);

  rut_transform_translate (transform, 0, state->y_offset, 0);
  state->y_offset += height;
  rut_graphable_add_child (doc_node, transform);
  rut_refable_unref (transform);

  data->component_inspectors =
    g_list_prepend (data->component_inspectors, inspector);
}

static void
update_inspector (RigData *data)
{
  RutObject *doc_node;

  if (data->inspector)
    {
      rut_graphable_remove_child (data->inspector);
      data->inspector = NULL;

      if (data->component_inspectors)
        {
          GList *l;

          for (l = data->component_inspectors; l; l = l->next)
            rut_graphable_remove_child (l->data);
          g_list_free (data->component_inspectors);
          data->component_inspectors = NULL;
        }
    }

  if (data->selected_entity)
    {
      float width, height;
      AddComponentState component_add_state;

      data->inspector = rut_inspector_new (data->ctx,
                                           data->selected_entity,
                                           inspector_property_changed_cb,
                                           data);

      rut_sizable_get_preferred_width (data->inspector,
                                       -1, /* for height */
                                       NULL, /* min_width */
                                       &width);
      rut_sizable_get_preferred_height (data->inspector,
                                        -1, /* for width */
                                        NULL, /* min_height */
                                        &height);
      rut_sizable_set_size (data->inspector, width, height);

      doc_node = rut_ui_viewport_get_doc_node (data->tool_vp);
      rut_graphable_add_child (doc_node, data->inspector);
      rut_refable_unref (data->inspector);

      component_add_state.data = data;
      component_add_state.y_offset = height + 10;
      rut_entity_foreach_component (data->selected_entity,
                                    add_component_inspector_cb,
                                    &component_add_state);
    }
}

static RutInputEventStatus
timeline_grab_input_cb (RutInputEvent *event, void *user_data)
{
  RigData *data = user_data;

  if (rut_input_event_get_type (event) != RUT_INPUT_EVENT_TYPE_MOTION)
    return RUT_INPUT_EVENT_STATUS_UNHANDLED;

  if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_MOVE)
    {
      RutButtonState state = rut_motion_event_get_button_state (event);
      float x = rut_motion_event_get_x (event);
      float y = rut_motion_event_get_y (event);

      if (state & RUT_BUTTON_STATE_1)
        {
          RutCamera *camera = data->timeline_camera;
          const CoglMatrix *view = rut_camera_get_view_transform (camera);
          CoglMatrix inverse_view;
          float progress;

          if (!cogl_matrix_get_inverse (view, &inverse_view))
            g_error ("Failed to get inverse transform");

          unproject_window_coord (camera,
                                  view,
                                  &inverse_view,
                                  0, /* z in entity coordinates */
                                  &x, &y);

          progress = x / data->timeline_width;
          //g_print ("x = %f, y = %f progress=%f\n", x, y, progress);

          rut_timeline_set_progress (data->timeline, progress);
          rut_shell_queue_redraw (data->ctx->shell);

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (state & RUT_BUTTON_STATE_2)
        {
          float dx = data->grab_x - x;
          float dy = data->grab_y - y;
          float t_scale =
            rut_ui_viewport_get_doc_scale_x (data->timeline_vp) *
            data->timeline_scale;

          float y_scale =
            rut_ui_viewport_get_doc_scale_y (data->timeline_vp) *
            data->timeline_scale;

          float inv_t_scale = 1.0 / t_scale;
          float inv_y_scale = 1.0 / y_scale;


          rut_ui_viewport_set_doc_x (data->timeline_vp,
                                     data->grab_timeline_vp_t + (dx * inv_t_scale));
          rut_ui_viewport_set_doc_y (data->timeline_vp,
                                     data->grab_timeline_vp_y + (dy * inv_y_scale));

          rut_shell_queue_redraw (data->ctx->shell);
        }
    }
  else if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_UP)
    {
      rut_shell_ungrab_input (data->ctx->shell,
                              timeline_grab_input_cb,
                              user_data);
      return RUT_INPUT_EVENT_STATUS_HANDLED;
    }
  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static RutInputEventStatus
timeline_input_cb (RutInputEvent *event,
                   void *user_data)
{
  RigData *data = user_data;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      data->key_focus_callback = timeline_input_cb;

      switch (rut_motion_event_get_action (event))
        {
        case RUT_MOTION_EVENT_ACTION_DOWN:
          {
            data->grab_x = rut_motion_event_get_x (event);
            data->grab_y = rut_motion_event_get_y (event);
            data->grab_timeline_vp_t = rut_ui_viewport_get_doc_x (data->timeline_vp);
            data->grab_timeline_vp_y = rut_ui_viewport_get_doc_y (data->timeline_vp);
            /* TODO: Add rut_shell_implicit_grab_input() that handles releasing
             * the grab for you */
            g_print ("timeline input grab\n");
            rut_shell_grab_input (data->ctx->shell,
                                  rut_input_event_get_camera (event),
                                  timeline_grab_input_cb, data);
            return RUT_INPUT_EVENT_STATUS_HANDLED;
          }
	default:
	  break;
        }
    }
  else if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_KEY &&
           rut_key_event_get_action (event) == RUT_KEY_EVENT_ACTION_UP)
    {
      switch (rut_key_event_get_keysym (event))
        {
        case RUT_KEY_equal:
          data->timeline_scale += 0.2;
          rut_shell_queue_redraw (data->ctx->shell);
          break;
        case RUT_KEY_minus:
          data->timeline_scale -= 0.2;
          rut_shell_queue_redraw (data->ctx->shell);
          break;
        case RUT_KEY_Home:
          data->timeline_scale = 1;
          rut_shell_queue_redraw (data->ctx->shell);
        }
      g_print ("Key press in timeline area\n");
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static RutInputEventStatus
timeline_region_input_cb (RutInputRegion *region,
                          RutInputEvent *event,
                          void *user_data)
{
  return timeline_input_cb (event, user_data);
}

static CoglPrimitive *
create_line_primitive (float a[3], float b[3])
{
  CoglVertexP3 data[2];
  CoglAttributeBuffer *attribute_buffer;
  CoglAttribute *attributes[1];
  CoglPrimitive *primitive;

  data[0].x = a[0];
  data[0].y = a[1];
  data[0].z = a[2];
  data[1].x = b[0];
  data[1].y = b[1];
  data[1].z = b[2];

  attribute_buffer = cogl_attribute_buffer_new (rut_cogl_context,
                                                2 * sizeof (CoglVertexP3),
                                                data);

  attributes[0] = cogl_attribute_new (attribute_buffer,
                                      "cogl_position_in",
                                      sizeof (CoglVertexP3),
                                      offsetof (CoglVertexP3, x),
                                      3,
                                      COGL_ATTRIBUTE_TYPE_FLOAT);

  primitive = cogl_primitive_new_with_attributes (COGL_VERTICES_MODE_LINES,
                                                  2, attributes, 1);

  cogl_object_unref (attribute_buffer);
  cogl_object_unref (attributes[0]);

  return primitive;
}

static void
transform_ray (CoglMatrix *transform,
               bool        inverse_transform,
               float       ray_origin[3],
               float       ray_direction[3])
{
  CoglMatrix inverse, normal_matrix, *m;

  m = transform;
  if (inverse_transform)
    {
      cogl_matrix_get_inverse (transform, &inverse);
      m = &inverse;
    }

  cogl_matrix_transform_points (m,
                                3, /* num components for input */
                                sizeof (float) * 3, /* input stride */
                                ray_origin,
                                sizeof (float) * 3, /* output stride */
                                ray_origin,
                                1 /* n_points */);

  cogl_matrix_get_inverse (m, &normal_matrix);
  cogl_matrix_transpose (&normal_matrix);

  rut_util_transform_normal (&normal_matrix,
                             &ray_direction[0],
                             &ray_direction[1],
                             &ray_direction[2]);
}

static CoglPrimitive *
create_picking_ray (RigData            *data,
                    CoglFramebuffer *fb,
                    float            ray_position[3],
                    float            ray_direction[3],
                    float            length)
{
  CoglPrimitive *line;
  float points[6];

  points[0] = ray_position[0];
  points[1] = ray_position[1];
  points[2] = ray_position[2];
  points[3] = ray_position[0] + length * ray_direction[0];
  points[4] = ray_position[1] + length * ray_direction[1];
  points[5] = ray_position[2] + length * ray_direction[2];

  line = create_line_primitive (points, points + 3);

  return line;
}

typedef struct _PickContext
{
  RutCamera *camera;
  CoglFramebuffer *fb;
  float *ray_origin;
  float *ray_direction;
  RutEntity *selected_entity;
  float selected_distance;
  int selected_index;
} PickContext;

static RutTraverseVisitFlags
entitygraph_pre_pick_cb (RutObject *object,
                         int depth,
                         void *user_data)
{
  PickContext *pick_ctx = user_data;
  CoglFramebuffer *fb = pick_ctx->fb;

  /* XXX: It could be nice if Cogl exposed matrix stacks directly, but for now
   * we just take advantage of an arbitrary framebuffer matrix stack so that
   * we can avoid repeated accumulating the transform of ancestors when
   * traversing between scenegraph nodes that have common ancestors.
   */
  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      const CoglMatrix *matrix = rut_transformable_get_matrix (object);
      cogl_framebuffer_push_matrix (fb);
      cogl_framebuffer_transform (fb, matrix);
    }

  if (rut_object_get_type (object) == &rut_entity_type)
    {
      RutEntity *entity = object;
      RutComponent *geometry;
      uint8_t *vertex_data;
      int n_vertices;
      size_t stride;
      int index;
      float distance;
      bool hit;
      float transformed_ray_origin[3];
      float transformed_ray_direction[3];
      CoglMatrix transform;

      if (!rut_entity_get_visible (entity))
        return RUT_TRAVERSE_VISIT_CONTINUE;

      geometry = rut_entity_get_component (entity, RUT_COMPONENT_TYPE_GEOMETRY);

      /* Get a mesh we can pick against */
      if (!(geometry &&
            rut_object_is (geometry, RUT_INTERFACE_ID_PICKABLE) &&
            (vertex_data = rut_pickable_get_vertex_data (geometry,
                                                         &stride,
                                                         &n_vertices))))
        return RUT_TRAVERSE_VISIT_CONTINUE;

      /* transform the ray into the model space */
      memcpy (transformed_ray_origin,
              pick_ctx->ray_origin, 3 * sizeof (float));
      memcpy (transformed_ray_direction,
              pick_ctx->ray_direction, 3 * sizeof (float));

      cogl_framebuffer_get_modelview_matrix (fb, &transform);

      transform_ray (&transform,
                     TRUE, /* inverse of the transform */
                     transformed_ray_origin,
                     transformed_ray_direction);

      /* intersect the transformed ray with the mesh data */
      hit = rut_util_intersect_mesh (vertex_data,
                                     n_vertices,
                                     stride,
                                     transformed_ray_origin,
                                     transformed_ray_direction,
                                     &index,
                                     &distance);

      if (hit)
        {
          const CoglMatrix *view = rut_camera_get_view_transform (pick_ctx->camera);
          float w = 1;

          /* to compare intersection distances we find the actual point of ray
           * intersection in model coordinates and transform that into eye
           * coordinates */

          transformed_ray_direction[0] *= distance;
          transformed_ray_direction[1] *= distance;
          transformed_ray_direction[2] *= distance;

          transformed_ray_direction[0] += transformed_ray_origin[0];
          transformed_ray_direction[1] += transformed_ray_origin[1];
          transformed_ray_direction[2] += transformed_ray_origin[2];

          cogl_matrix_transform_point (&transform,
                                       &transformed_ray_direction[0],
                                       &transformed_ray_direction[1],
                                       &transformed_ray_direction[2],
                                       &w);
          cogl_matrix_transform_point (view,
                                       &transformed_ray_direction[0],
                                       &transformed_ray_direction[1],
                                       &transformed_ray_direction[2],
                                       &w);
          distance = transformed_ray_direction[2];

          if (distance > pick_ctx->selected_distance)
            {
              pick_ctx->selected_entity = entity;
              pick_ctx->selected_distance = distance;
              pick_ctx->selected_index = index;
            }
        }
    }

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static RutTraverseVisitFlags
entitygraph_post_pick_cb (RutObject *object,
                          int depth,
                          void *user_data)
{
  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      PickContext *pick_ctx = user_data;
      cogl_framebuffer_pop_matrix (pick_ctx->fb);
    }

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static RutEntity *
pick (RigData *data,
      RutCamera *camera,
      CoglFramebuffer *fb,
      float ray_origin[3],
      float ray_direction[3])
{
  PickContext pick_ctx;

  pick_ctx.camera = camera;
  pick_ctx.fb = fb;
  pick_ctx.selected_distance = -G_MAXFLOAT;
  pick_ctx.selected_entity = NULL;
  pick_ctx.ray_origin = ray_origin;
  pick_ctx.ray_direction = ray_direction;

  rut_graphable_traverse (data->scene,
                          RUT_TRAVERSE_DEPTH_FIRST,
                          entitygraph_pre_pick_cb,
                          entitygraph_post_pick_cb,
                          &pick_ctx);

  if (pick_ctx.selected_entity)
    {
      g_message ("Hit entity, triangle #%d, distance %.2f",
                 pick_ctx.selected_index, pick_ctx.selected_distance);
    }

  return pick_ctx.selected_entity;
}

static void
update_camera_position (RigData *data)
{
  rut_entity_set_position (data->editor_camera_to_origin,
                           data->origin);

  rut_entity_set_translate (data->editor_camera_armature, 0, 0, data->editor_camera_z);

  rut_shell_queue_redraw (data->ctx->shell);
}

static void
print_quaternion (const CoglQuaternion *q,
                  const char *label)
{
  float angle = cogl_quaternion_get_rotation_angle (q);
  float axis[3];
  cogl_quaternion_get_rotation_axis (q, axis);
  g_print ("%s: [%f (%f, %f, %f)]\n", label, angle, axis[0], axis[1], axis[2]);
}

static CoglBool
translate_grab_entity (RigData *data,
                       RutCamera *camera,
                       RutEntity *entity,
                       float grab_x,
                       float grab_y,
                       EntityTranslateCallback translate_cb,
                       EntityTranslateDoneCallback done_cb,
                       void *user_data)
{
  EntityTranslateGrabClosure *closure =
    g_slice_new (EntityTranslateGrabClosure);
  RutEntity *parent = rut_graphable_get_parent (entity);
  CoglMatrix parent_transform;
  CoglMatrix inverse_transform;
  float origin[3] = {0, 0, 0};
  float unit_x[3] = {1, 0, 0};
  float unit_y[3] = {0, 1, 0};
  float x_vec[3];
  float y_vec[3];
  float entity_x, entity_y, entity_z;
  float w;

  if (!parent)
    return FALSE;

  rut_graphable_get_modelview (parent, camera, &parent_transform);

  if (!cogl_matrix_get_inverse (&parent_transform, &inverse_transform))
    {
      g_warning ("Failed to get inverse transform of entity");
      return FALSE;
    }

  /* Find the z of our selected entity in eye coordinates */
  entity_x = 0;
  entity_y = 0;
  entity_z = 0;
  w = 1;
  cogl_matrix_transform_point (&parent_transform,
                               &entity_x, &entity_y, &entity_z, &w);

  //g_print ("Entity origin in eye coords: %f %f %f\n", entity_x, entity_y, entity_z);

  /* Convert unit x and y vectors in screen coordinate
   * into points in eye coordinates with the same z depth
   * as our selected entity */

  unproject_window_coord (camera,
                          &data->identity, &data->identity,
                          entity_z, &origin[0], &origin[1]);
  origin[2] = entity_z;
  //g_print ("eye origin: %f %f %f\n", origin[0], origin[1], origin[2]);

  unproject_window_coord (camera,
                          &data->identity, &data->identity,
                          entity_z, &unit_x[0], &unit_x[1]);
  unit_x[2] = entity_z;
  //g_print ("eye unit_x: %f %f %f\n", unit_x[0], unit_x[1], unit_x[2]);

  unproject_window_coord (camera,
                          &data->identity, &data->identity,
                          entity_z, &unit_y[0], &unit_y[1]);
  unit_y[2] = entity_z;
  //g_print ("eye unit_y: %f %f %f\n", unit_y[0], unit_y[1], unit_y[2]);


  /* Transform our points from eye coordinates into entity
   * coordinates and convert into input mapping vectors */

  w = 1;
  cogl_matrix_transform_point (&inverse_transform,
                               &origin[0], &origin[1], &origin[2], &w);
  w = 1;
  cogl_matrix_transform_point (&inverse_transform,
                               &unit_x[0], &unit_x[1], &unit_x[2], &w);
  w = 1;
  cogl_matrix_transform_point (&inverse_transform,
                               &unit_y[0], &unit_y[1], &unit_y[2], &w);


  x_vec[0] = unit_x[0] - origin[0];
  x_vec[1] = unit_x[1] - origin[1];
  x_vec[2] = unit_x[2] - origin[2];

  //g_print (" =========================== Entity coords: x_vec = %f, %f, %f\n",
  //         x_vec[0], x_vec[1], x_vec[2]);

  y_vec[0] = unit_y[0] - origin[0];
  y_vec[1] = unit_y[1] - origin[1];
  y_vec[2] = unit_y[2] - origin[2];

  //g_print (" =========================== Entity coords: y_vec = %f, %f, %f\n",
  //         y_vec[0], y_vec[1], y_vec[2]);

  closure->data = data;
  closure->grab_x = grab_x;
  closure->grab_y = grab_y;

  memcpy (closure->entity_grab_pos,
          rut_entity_get_position (entity),
          sizeof (float) * 3);

  closure->entity = entity;
  closure->entity_translate_cb = translate_cb;
  closure->entity_translate_done_cb = done_cb;
  closure->user_data = user_data;

  memcpy (closure->x_vec, x_vec, sizeof (float) * 3);
  memcpy (closure->y_vec, y_vec, sizeof (float) * 3);

  rut_shell_grab_input (data->ctx->shell,
                        camera,
                        entity_translate_grab_input_cb,
                        closure);

  return TRUE;
}

static void
reload_position_inspector (RigData *data,
                           RutEntity *entity)
{
  if (data->inspector)
    {
      RutProperty *property =
        rut_introspectable_lookup_property (entity, "position");

      rut_inspector_reload_property (data->inspector, property);
    }
}

static void
entity_translate_done_cb (RutEntity *entity,
                          float start[3],
                          float rel[3],
                          void *user_data)
{
  RigData *data = user_data;

  rig_undo_journal_move_and_log (data->undo_journal,
                                 FALSE, /* mergable */
                                 entity,
                                 start[0] + rel[0],
                                 start[1] + rel[1],
                                 start[2] + rel[2]);

  reload_position_inspector (data, entity);

  rut_shell_queue_redraw (data->ctx->shell);
}

static void
entity_translate_cb (RutEntity *entity,
                     float start[3],
                     float rel[3],
                     void *user_data)
{
  RigData *data = user_data;

  rut_entity_set_translate (entity,
                            start[0] + rel[0],
                            start[1] + rel[1],
                            start[2] + rel[2]);

  reload_position_inspector (data, entity);

  rut_shell_queue_redraw (data->ctx->shell);
}

static void
scene_translate_cb (RutEntity *entity,
                    float start[3],
                    float rel[3],
                    void *user_data)
{
  RigData *data = user_data;

  data->origin[0] = start[0] - rel[0];
  data->origin[1] = start[1] - rel[1];
  data->origin[2] = start[2] - rel[2];

  update_camera_position (data);
}

static void
set_play_mode_enabled (RigData *data, CoglBool enabled)
{
  data->play_mode = enabled;

  if (data->play_mode)
    {
      data->enable_dof = TRUE;
      data->debug_pick_ray = 0;
    }
  else
    {
      data->enable_dof = FALSE;
      data->debug_pick_ray = 1;
    }

  rut_shell_queue_redraw (data->ctx->shell);
}

static RutInputEventStatus
main_input_cb (RutInputEvent *event,
               void *user_data)
{
  RigData *data = user_data;

  g_print ("Main Input Callback\n");

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      RutMotionEventAction action = rut_motion_event_get_action (event);
      RutModifierState modifiers = rut_motion_event_get_modifier_state (event);
      float x = rut_motion_event_get_x (event);
      float y = rut_motion_event_get_y (event);
      RutButtonState state;

      if (rut_camera_transform_window_coordinate (data->editor_camera_component, &x, &y))
        data->key_focus_callback = main_input_cb;

      state = rut_motion_event_get_button_state (event);

      if (action == RUT_MOTION_EVENT_ACTION_DOWN &&
          state == RUT_BUTTON_STATE_1)
        {
          /* pick */
          RutCamera *camera;
          float ray_position[3], ray_direction[3], screen_pos[2],
                z_far, z_near;
          const float *viewport;
          const CoglMatrix *inverse_projection;
          //CoglMatrix *camera_transform;
          const CoglMatrix *camera_view;
          CoglMatrix camera_transform;

          camera = rut_entity_get_component (data->editor_camera,
                                             RUT_COMPONENT_TYPE_CAMERA);
          viewport = rut_camera_get_viewport (RUT_CAMERA (camera));
          z_near = rut_camera_get_near_plane (RUT_CAMERA (camera));
          z_far = rut_camera_get_far_plane (RUT_CAMERA (camera));
          inverse_projection =
            rut_camera_get_inverse_projection (RUT_CAMERA (camera));

#if 0
          camera_transform = rut_entity_get_transform (data->editor_camera);
#else
          camera_view = rut_camera_get_view_transform (camera);
          cogl_matrix_get_inverse (camera_view, &camera_transform);
#endif

          screen_pos[0] = x;
          screen_pos[1] = y;

          rut_util_create_pick_ray (viewport,
                                    inverse_projection,
                                    &camera_transform,
                                    screen_pos,
                                    ray_position,
                                    ray_direction);

          if (data->debug_pick_ray)
            {
              float x1 = 0, y1 = 0, z1 = z_near, w1 = 1;
              float x2 = 0, y2 = 0, z2 = z_far, w2 = 1;
              float len;

              if (data->picking_ray)
                cogl_object_unref (data->picking_ray);

              /* FIXME: This is a hack, we should intersect the ray with
               * the far plane to decide how long the debug primitive
               * should be */
              cogl_matrix_transform_point (&camera_transform,
                                           &x1, &y1, &z1, &w1);
              cogl_matrix_transform_point (&camera_transform,
                                           &x2, &y2, &z2, &w2);
              len = z2 - z1;

              data->picking_ray = create_picking_ray (data,
                                                      rut_camera_get_framebuffer (camera),
                                                      ray_position,
                                                      ray_direction,
                                                      len);
            }

          data->selected_entity = pick (data,
                                        camera,
                                        rut_camera_get_framebuffer (camera),
                                        ray_position,
                                        ray_direction);

          rut_shell_queue_redraw (data->ctx->shell);
          if (data->selected_entity == NULL)
            rut_tool_update (data->tool, NULL);
          else if (data->selected_entity == data->light_handle)
            data->selected_entity = data->light;

          update_inspector (data);

          /* If we have selected an entity then initiate a grab so the
           * entity can be moved with the mouse...
           */
          if (data->selected_entity)
            {
              if (!translate_grab_entity (data,
                                          rut_input_event_get_camera (event),
                                          data->selected_entity,
                                          rut_motion_event_get_x (event),
                                          rut_motion_event_get_y (event),
                                          entity_translate_cb,
                                          entity_translate_done_cb,
                                          data))
                return RUT_INPUT_EVENT_STATUS_UNHANDLED;
            }

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (action == RUT_MOTION_EVENT_ACTION_DOWN &&
               state == RUT_BUTTON_STATE_2 &&
               ((modifiers & RUT_MODIFIER_SHIFT_ON) == 0))
        {
          //data->saved_rotation = *rut_entity_get_rotation (data->editor_camera);
          data->saved_rotation = *rut_entity_get_rotation (data->editor_camera_rotate);

          cogl_quaternion_init_identity (&data->arcball.q_drag);

          //rut_arcball_mouse_down (&data->arcball, data->width - x, y);
          rut_arcball_mouse_down (&data->arcball, data->main_width - x, data->main_height - y);
          g_print ("Arcball init, mouse = (%d, %d)\n", (int)(data->width - x), (int)(data->height - y));

          print_quaternion (&data->saved_rotation, "Saved Quaternion");
          print_quaternion (&data->arcball.q_drag, "Arcball Initial Quaternion");
          //data->button_down = TRUE;

          data->grab_x = x;
          data->grab_y = y;
          memcpy (data->saved_origin, data->origin, sizeof (data->origin));

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (action == RUT_MOTION_EVENT_ACTION_MOVE &&
               state == RUT_BUTTON_STATE_2 &&
               modifiers & RUT_MODIFIER_SHIFT_ON)
        {
          if (!translate_grab_entity (data,
                                      rut_input_event_get_camera (event),
                                      data->editor_camera_to_origin,
                                      rut_motion_event_get_x (event),
                                      rut_motion_event_get_y (event),
                                      scene_translate_cb,
                                      NULL,
                                      data))
            return RUT_INPUT_EVENT_STATUS_UNHANDLED;
#if 0
          float origin[3] = {0, 0, 0};
          float unit_x[3] = {1, 0, 0};
          float unit_y[3] = {0, 1, 0};
          float x_vec[3];
          float y_vec[3];
          float dx;
          float dy;
          float translation[3];

          rut_entity_get_transformed_position (data->editor_camera,
                                               origin);
          rut_entity_get_transformed_position (data->editor_camera,
                                               unit_x);
          rut_entity_get_transformed_position (data->editor_camera,
                                               unit_y);

          x_vec[0] = origin[0] - unit_x[0];
          x_vec[1] = origin[1] - unit_x[1];
          x_vec[2] = origin[2] - unit_x[2];

            {
              CoglMatrix transform;
              rut_graphable_get_transform (data->editor_camera, &transform);
              cogl_debug_matrix_print (&transform);
            }
          g_print (" =========================== x_vec = %f, %f, %f\n",
                   x_vec[0], x_vec[1], x_vec[2]);

          y_vec[0] = origin[0] - unit_y[0];
          y_vec[1] = origin[1] - unit_y[1];
          y_vec[2] = origin[2] - unit_y[2];

          //dx = (x - data->grab_x) * (data->editor_camera_z / 100.0f);
          //dy = -(y - data->grab_y) * (data->editor_camera_z / 100.0f);
          dx = (x - data->grab_x);
          dy = -(y - data->grab_y);

          translation[0] = dx * x_vec[0];
          translation[1] = dx * x_vec[1];
          translation[2] = dx * x_vec[2];

          translation[0] += dy * y_vec[0];
          translation[1] += dy * y_vec[1];
          translation[2] += dy * y_vec[2];

          data->origin[0] = data->saved_origin[0] + translation[0];
          data->origin[1] = data->saved_origin[1] + translation[1];
          data->origin[2] = data->saved_origin[2] + translation[2];

          update_camera_position (data);

          g_print ("Translate %f %f dx=%f, dy=%f\n",
                   x - data->grab_x,
                   y - data->grab_y,
                   dx, dy);
#endif
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (action == RUT_MOTION_EVENT_ACTION_MOVE &&
               state == RUT_BUTTON_STATE_2 &&
               ((modifiers & RUT_MODIFIER_SHIFT_ON) == 0))
        {
          CoglQuaternion new_rotation;

          //if (!data->button_down)
          //  break;

          //rut_arcball_mouse_motion (&data->arcball, data->width - x, y);
          rut_arcball_mouse_motion (&data->arcball, data->main_width - x, data->main_height - y);
          g_print ("Arcball motion, center=%f,%f mouse = (%f, %f)\n",
                   data->arcball.center[0],
                   data->arcball.center[1],
                   x, y);

          cogl_quaternion_multiply (&new_rotation,
                                    &data->saved_rotation,
                                    &data->arcball.q_drag);

          //rut_entity_set_rotation (data->editor_camera, &new_rotation);
          rut_entity_set_rotation (data->editor_camera_rotate, &new_rotation);

          print_quaternion (&new_rotation, "New Rotation");

          print_quaternion (&data->arcball.q_drag, "Arcball Quaternion");

          g_print ("rig entity set rotation\n");

          rut_shell_queue_redraw (data->ctx->shell);

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }

#if 0
      switch (rut_motion_event_get_action (event))
        {
        case RUT_MOTION_EVENT_ACTION_DOWN:
          /* TODO: Add rut_shell_implicit_grab_input() that handles releasing
           * the grab for you */
          rut_shell_grab_input (data->ctx->shell, timeline_grab_input_cb, data);
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
#endif
    }
#ifdef RIG_EDITOR_ENABLED
  else if (!_rig_in_device_mode &&
           rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_KEY &&
           rut_key_event_get_action (event) == RUT_KEY_EVENT_ACTION_UP)
    {
      switch (rut_key_event_get_keysym (event))
        {
        case RUT_KEY_s:
          rig_save (data, _rig_handset_remaining_args[0]);
          break;
        case RUT_KEY_z:
          if (rut_key_event_get_modifier_state (event) & RUT_MODIFIER_CTRL_ON)
            rig_undo_journal_undo (data->undo_journal);
          break;
        case RUT_KEY_y:
          if (rut_key_event_get_modifier_state (event) & RUT_MODIFIER_CTRL_ON)
            rig_undo_journal_redo (data->undo_journal);
          break;
        case RUT_KEY_minus:
          if (data->editor_camera_z)
            data->editor_camera_z *= 1.2f;
          else
            data->editor_camera_z = 0.1;

          update_camera_position (data);
          break;
        case RUT_KEY_equal:
          data->editor_camera_z *= 0.8;
          update_camera_position (data);
          break;
        case RUT_KEY_p:
          set_play_mode_enabled (data, !data->play_mode);
          break;
        }
    }
#endif /* RIG_EDITOR_ENABLED */

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static RutInputEventStatus
device_mode_grab_input_cb (RutInputEvent *event, void *user_data)
{
  RigData *data = user_data;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      RutMotionEventAction action = rut_motion_event_get_action (event);

      switch (action)
        {
        case RUT_MOTION_EVENT_ACTION_UP:
          rut_shell_ungrab_input (data->ctx->shell,
                                  device_mode_grab_input_cb,
                                  user_data);
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        case RUT_MOTION_EVENT_ACTION_MOVE:
          {
            float x = rut_motion_event_get_x (event);
            float dx = x - data->grab_x;
            CoglFramebuffer *fb = COGL_FRAMEBUFFER (data->onscreen);
            float progression = dx / cogl_framebuffer_get_width (fb);

            rut_timeline_set_progress (data->timeline,
                                       data->grab_progress + progression);

            rut_shell_queue_redraw (data->ctx->shell);
            return RUT_INPUT_EVENT_STATUS_HANDLED;
          }
        default:
          return RUT_INPUT_EVENT_STATUS_UNHANDLED;
        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static RutInputEventStatus
device_mode_input_cb (RutInputEvent *event,
                      void *user_data)
{
  RigData *data = user_data;

  g_print ("Device Input Callback\n");

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      RutMotionEventAction action = rut_motion_event_get_action (event);
      RutButtonState state = rut_motion_event_get_button_state (event);

      if (action == RUT_MOTION_EVENT_ACTION_DOWN &&
          state == RUT_BUTTON_STATE_1)
        {
            data->grab_x = rut_motion_event_get_x (event);
            data->grab_y = rut_motion_event_get_y (event);
            data->grab_progress = rut_timeline_get_progress (data->timeline);

            /* TODO: Add rut_shell_implicit_grab_input() that handles releasing
             * the grab for you */
            rut_shell_grab_input (data->ctx->shell,
                                  rut_input_event_get_camera (event),
                                  device_mode_grab_input_cb, data);
            return RUT_INPUT_EVENT_STATUS_HANDLED;

        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static RutInputEventStatus
editor_input_region_cb (RutInputRegion *region,
                      RutInputEvent *event,
                      void *user_data)
{
#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    return main_input_cb (event, user_data);
  else
#endif
    return device_mode_input_cb (event, user_data);
}

void
matrix_view_2d_in_frustum (CoglMatrix *matrix,
                           float left,
                           float right,
                           float bottom,
                           float top,
                           float z_near,
                           float z_2d,
                           float width_2d,
                           float height_2d)
{
  float left_2d_plane = left / z_near * z_2d;
  float right_2d_plane = right / z_near * z_2d;
  float bottom_2d_plane = bottom / z_near * z_2d;
  float top_2d_plane = top / z_near * z_2d;

  float width_2d_start = right_2d_plane - left_2d_plane;
  float height_2d_start = top_2d_plane - bottom_2d_plane;

  /* Factors to scale from framebuffer geometry to frustum
   * cross-section geometry. */
  float width_scale = width_2d_start / width_2d;
  float height_scale = height_2d_start / height_2d;

  //cogl_matrix_translate (matrix,
  //                       left_2d_plane, top_2d_plane, -z_2d);
  cogl_matrix_translate (matrix,
                         left_2d_plane, top_2d_plane, 0);

  cogl_matrix_scale (matrix, width_scale, -height_scale, width_scale);
}

/* Assuming a symmetric perspective matrix is being used for your
 * projective transform then for a given z_2d distance within the
 * projective frustrum this convenience function determines how
 * we can use an entity transform to move from a normalized coordinate
 * space with (0,0) in the center of the screen to a non-normalized
 * 2D coordinate space with (0,0) at the top-left of the screen.
 *
 * Note: It assumes the viewport aspect ratio matches the desired
 * aspect ratio of the 2d coordinate space which is why we only
 * need to know the width of the 2d coordinate space.
 *
 */
void
get_entity_transform_for_2d_view (float fov_y,
                                  float aspect,
                                  float z_near,
                                  float z_2d,
                                  float width_2d,
                                  float *dx,
                                  float *dy,
                                  float *dz,
                                  CoglQuaternion *rotation,
                                  float *scale)
{
  float top = z_near * tan (fov_y * G_PI / 360.0);
  float left = -top * aspect;
  float right = top * aspect;

  float left_2d_plane = left / z_near * z_2d;
  float right_2d_plane = right / z_near * z_2d;
  float top_2d_plane = top / z_near * z_2d;

  float width_2d_start = right_2d_plane - left_2d_plane;

  *dx = left_2d_plane;
  *dy = top_2d_plane;
  *dz = 0;
  //*dz = -z_2d;

  /* Factors to scale from framebuffer geometry to frustum
   * cross-section geometry. */
  *scale = width_2d_start / width_2d;

  cogl_quaternion_init_from_z_rotation (rotation, 180);
}

static void
matrix_view_2d_in_perspective (CoglMatrix *matrix,
                               float fov_y,
                               float aspect,
                               float z_near,
                               float z_2d,
                               float width_2d,
                               float height_2d)
{
  float top = z_near * tan (fov_y * G_PI / 360.0);

  matrix_view_2d_in_frustum (matrix,
                             -top * aspect,
                             top * aspect,
                             -top,
                             top,
                             z_near,
                             z_2d,
                             width_2d,
                             height_2d);
}

static void
allocate_main_area (RigData *data)
{
  float screen_aspect;
  float main_aspect;
  float device_scale;

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_bevel_get_size (data->main_area_bevel, &data->main_width, &data->main_height);
      if (data->main_width <= 0)
        data->main_width = 10;
      if (data->main_height <= 0)
        data->main_height = 10;
    }
  else
#endif /* RIG_EDITOR_ENABLED */
    {
      CoglFramebuffer *fb = COGL_FRAMEBUFFER (data->onscreen);
      data->main_width = cogl_framebuffer_get_width (fb);
      data->main_height = cogl_framebuffer_get_height (fb);
    }

  /* Update the window camera */
  rut_camera_set_projection_mode (data->camera, RUT_PROJECTION_ORTHOGRAPHIC);
  rut_camera_set_orthographic_coordinates (data->camera,
                                           0, 0, data->width, data->height);
  rut_camera_set_near_plane (data->camera, -1);
  rut_camera_set_far_plane (data->camera, 100);

  rut_camera_set_viewport (data->camera, 0, 0, data->width, data->height);

  screen_aspect = DEVICE_WIDTH / DEVICE_HEIGHT;
  main_aspect = data->main_width / data->main_height;

  if (screen_aspect < main_aspect) /* screen is slimmer and taller than the main area */
    {
      data->screen_area_height = data->main_height;
      data->screen_area_width = data->screen_area_height * screen_aspect;

      rut_entity_set_translate (data->editor_camera_screen_pos,
                                -(data->main_width / 2.0) + (data->screen_area_width / 2.0),
                                0, 0);
    }
  else
    {
      data->screen_area_width = data->main_width;
      data->screen_area_height = data->screen_area_width / screen_aspect;

      rut_entity_set_translate (data->editor_camera_screen_pos,
                                0,
                                -(data->main_height / 2.0) + (data->screen_area_height / 2.0),
                                0);
    }

  /* NB: We know the screen area matches the device aspect ratio so we can use
   * a uniform scale here... */
  device_scale = data->screen_area_width / DEVICE_WIDTH;

  rut_entity_set_scale (data->editor_camera_dev_scale, 1.0 / device_scale);

  /* Setup projection for main content view */
  {
    float fovy = 10; /* y-axis field of view */
    float aspect = (float)data->main_width/(float)data->main_height;
    float z_near = 10; /* distance to near clipping plane */
    float z_far = 100; /* distance to far clipping plane */
    float x = 0, y = 0, z_2d = 30, w = 1;
    CoglMatrix inverse;

    data->z_2d = z_2d; /* position to 2d plane */

    cogl_matrix_init_identity (&data->main_view);
    matrix_view_2d_in_perspective (&data->main_view,
                                   fovy, aspect, z_near, data->z_2d,
                                   data->main_width,
                                   data->main_height);

    rut_camera_set_projection_mode (data->editor_camera_component,
                                    RUT_PROJECTION_PERSPECTIVE);
    rut_camera_set_field_of_view (data->editor_camera_component, fovy);
    rut_camera_set_near_plane (data->editor_camera_component, z_near);
    rut_camera_set_far_plane (data->editor_camera_component, z_far);

    /* Handle the z_2d translation by changing the length of the
     * camera's armature.
     */
    cogl_matrix_get_inverse (&data->main_view,
                             &inverse);
    cogl_matrix_transform_point (&inverse,
                                 &x, &y, &z_2d, &w);

    data->editor_camera_z = z_2d / device_scale;
    rut_entity_set_translate (data->editor_camera_armature, 0, 0, data->editor_camera_z);
    //rut_entity_set_translate (data->editor_camera_armature, 0, 0, 0);

    {
      float dx, dy, dz, scale;
      CoglQuaternion rotation;

      get_entity_transform_for_2d_view (fovy,
                                        aspect,
                                        z_near,
                                        data->z_2d,
                                        data->main_width,
                                        &dx,
                                        &dy,
                                        &dz,
                                        &rotation,
                                        &scale);

      rut_entity_set_translate (data->editor_camera_2d_view, -dx, -dy, -dz);
      rut_entity_set_rotation (data->editor_camera_2d_view, &rotation);
      rut_entity_set_scale (data->editor_camera_2d_view, 1.0 / scale);
    }
  }

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_arcball_init (&data->arcball,
                        data->main_width / 2,
                        data->main_height / 2,
                        sqrtf (data->main_width *
                               data->main_width +
                               data->main_height *
                               data->main_height) / 2);
    }
#endif /* RIG_EDITOR_ENABLED */
}

static void
allocate (RigData *data)
{
  float vp_width;
  float vp_height;

  data->top_bar_height = 30;
  //data->top_bar_height = 0;
  data->left_bar_width = data->width * 0.2;
  //data->left_bar_width = 200;
  //data->left_bar_width = 0;
  data->right_bar_width = data->width * 0.2;
  //data->right_bar_width = 200;
  data->bottom_bar_height = data->height * 0.2;
  data->grab_margin = 5;
  //data->main_width = data->width - data->left_bar_width - data->right_bar_width;
  //data->main_height = data->height - data->top_bar_height - data->bottom_bar_height;

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    rut_split_view_set_size (data->splits[0], data->width, data->height);
#endif

  allocate_main_area (data);

#ifdef RIG_EDITOR_ENABLED
  /* Setup projection for the timeline view */
  if (!_rig_in_device_mode)
    {
      data->timeline_width = data->width - data->right_bar_width;
      data->timeline_height = data->bottom_bar_height;

      rut_camera_set_projection_mode (data->timeline_camera, RUT_PROJECTION_ORTHOGRAPHIC);
      rut_camera_set_orthographic_coordinates (data->timeline_camera,
                                               0, 0,
                                               data->timeline_width,
                                               data->timeline_height);
      rut_camera_set_near_plane (data->timeline_camera, -1);
      rut_camera_set_far_plane (data->timeline_camera, 100);
      rut_camera_set_background_color4f (data->timeline_camera, 1, 0, 0, 1);

      rut_camera_set_viewport (data->timeline_camera,
                               0,
                               data->height - data->bottom_bar_height,
                               data->timeline_width,
                               data->timeline_height);

      rut_input_region_set_rectangle (data->timeline_input_region,
                                      0, 0, data->timeline_width, data->timeline_height);

      vp_width = data->width - data->bottom_bar_height;
      rut_ui_viewport_set_width (data->timeline_vp,
                                 vp_width);
      vp_height = data->bottom_bar_height;
      rut_ui_viewport_set_height (data->timeline_vp,
                                  vp_height);
      rut_ui_viewport_set_doc_scale_x (data->timeline_vp,
                                       (vp_width / data->timeline_len));
      rut_ui_viewport_set_doc_scale_y (data->timeline_vp,
                                       (vp_height / DEVICE_HEIGHT));
    }
#endif /* RIG_EDITOR_ENABLED */
}

static void
data_onscreen_resize (CoglOnscreen *onscreen,
                      int width,
                      int height,
                      void *user_data)
{
  RigData *data = user_data;

  data->width = width;
  data->height = height;

  rut_property_dirty (&data->ctx->property_ctx, &data->properties[RUT_DATA_PROP_WIDTH]);
  rut_property_dirty (&data->ctx->property_ctx, &data->properties[RUT_DATA_PROP_HEIGHT]);

  allocate (data);
}

static void
camera_viewport_binding_cb (RutProperty *target_property,
                            RutProperty *source_property,
                            void *user_data)
{
  RigData *data = user_data;
  float x, y, z, width, height;

  x = y = z = 0;
  rut_graphable_fully_transform_point (data->main_area_bevel,
                                       data->camera,
                                       &x, &y, &z);

  data->main_x = x;
  data->main_y = y;

  x = RUT_UTIL_NEARBYINT (x);
  y = RUT_UTIL_NEARBYINT (y);

  rut_bevel_get_size (data->main_area_bevel, &width, &height);

  /* XXX: We round down here since that's currently what
   * rig-bevel.c:_rut_bevel_paint() does too. */
  width = (int)width;
  height = (int)height;

  rut_camera_set_viewport (data->editor_camera_component,
                           x, y, width, height);

  rut_input_region_set_rectangle (data->editor_input_region,
                                  x, y,
                                  x + width,
                                  y + height);

  allocate_main_area (data);
}

static void
init (RutShell *shell, void *user_data)
{
  RigData *data = user_data;
  CoglFramebuffer *fb;
  float vector3[3];
  int i;
  char *full_path;
  CoglError *error = NULL;
  CoglTexture2D *color_buffer;
  RutColor color;
  RutMesh *mesh;
  RutMaterial *material;
  RutLight *light;
  RutCamera *camera;
  RutColor top_bar_ref_color, main_area_ref_color, right_bar_ref_color;

  /* A unit test for the list_splice/list_unsplice functions */
#if 0
  _rut_test_list_splice ();
#endif

  cogl_matrix_init_identity (&data->identity);

  for (i = 0; i < RUT_DATA_N_PROPS; i++)
    rut_property_init (&data->properties[i],
                       &rut_data_property_specs[i],
                       data);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    data->onscreen = cogl_onscreen_new (data->ctx->cogl_context, 1000, 700);
  else
#endif
    data->onscreen = cogl_onscreen_new (data->ctx->cogl_context,
                                        DEVICE_WIDTH / 2, DEVICE_HEIGHT / 2);
  cogl_onscreen_show (data->onscreen);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      /* FIXME: On SDL this isn't taking affect if set before allocating
       * the framebuffer. */
      cogl_onscreen_set_resizable (data->onscreen, TRUE);
      cogl_onscreen_add_resize_handler (data->onscreen, data_onscreen_resize, data);
    }
#endif

  fb = COGL_FRAMEBUFFER (data->onscreen);
  data->width = cogl_framebuffer_get_width (fb);
  data->height  = cogl_framebuffer_get_height (fb);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    data->undo_journal = rig_undo_journal_new (data);

  /* Create a color gradient texture that can be used for debugging
   * shadow mapping.
   *
   * XXX: This should probably simply be #ifdef DEBUG code.
   */
  if (!_rig_in_device_mode)
    {
      CoglVertexP2C4 quad[] = {
        { 0, 0, 0xff, 0x00, 0x00, 0xff },
        { 0, 200, 0x00, 0xff, 0x00, 0xff },
        { 200, 200, 0x00, 0x00, 0xff, 0xff },
        { 200, 0, 0xff, 0xff, 0xff, 0xff }
      };
      CoglOffscreen *offscreen;
      CoglPrimitive *prim =
        cogl_primitive_new_p2c4 (data->ctx->cogl_context, COGL_VERTICES_MODE_TRIANGLE_FAN, 4, quad);
      CoglPipeline *pipeline = cogl_pipeline_new (data->ctx->cogl_context);

      data->gradient = COGL_TEXTURE (
        cogl_texture_2d_new_with_size (rut_cogl_context,
                                       200, 200,
                                       COGL_PIXEL_FORMAT_ANY,
                                       NULL));

      offscreen = cogl_offscreen_new_to_texture (data->gradient);

      cogl_framebuffer_orthographic (COGL_FRAMEBUFFER (offscreen),
                                     0, 0,
                                     200,
                                     200,
                                     -1,
                                     100);
      cogl_framebuffer_clear4f (COGL_FRAMEBUFFER (offscreen),
                                COGL_BUFFER_BIT_COLOR | COGL_BUFFER_BIT_DEPTH,
                                0, 0, 0, 1);
      cogl_framebuffer_draw_primitive (COGL_FRAMEBUFFER (offscreen),
                                       pipeline,
                                       prim);
      cogl_object_unref (prim);
      cogl_object_unref (offscreen);
    }
#endif /* RIG_EDITOR_ENABLED */

  /*
   * Shadow mapping
   */

  /* Setup the shadow map */
  /* TODO: reallocate if the onscreen framebuffer is resized */
  color_buffer = cogl_texture_2d_new_with_size (rut_cogl_context,
                                                data->width * 2, data->height * 2,
                                                COGL_PIXEL_FORMAT_ANY,
                                                &error);
  if (error)
    g_critical ("could not create texture: %s", error->message);

  data->shadow_color = color_buffer;

  /* XXX: Rutht now there's no way to disable rendering to the color
   * buffer. */
  data->shadow_fb =
    cogl_offscreen_new_to_texture (COGL_TEXTURE (color_buffer));

  /* retrieve the depth texture */
  cogl_framebuffer_set_depth_texture_enabled (COGL_FRAMEBUFFER (data->shadow_fb),
                                              TRUE);
  /* FIXME: It doesn't seem right that we can query back the texture before
   * the framebuffer has been allocated. */
  data->shadow_map =
    cogl_framebuffer_get_depth_texture (COGL_FRAMEBUFFER (data->shadow_fb));

  if (data->shadow_fb == NULL)
    g_critical ("could not create offscreen buffer");

  data->default_pipeline = cogl_pipeline_new (data->ctx->cogl_context);

  /*
   * Depth of Field
   */

  data->dof = rut_dof_effect_new (data->ctx);
  data->enable_dof = FALSE;

  data->circle_texture = rut_create_circle_texture (data->ctx,
                                                    CIRCLE_TEX_RADIUS /* radius */,
                                                    CIRCLE_TEX_PADDING /* padding */);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      data->grid_prim = rut_create_create_grid (data->ctx,
                                                DEVICE_WIDTH,
                                                DEVICE_HEIGHT,
                                                100,
                                                100);
    }
#endif

  data->circle_node_attribute =
    rut_create_circle_fan_p2 (data->ctx, 20, &data->circle_node_n_verts);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      full_path = g_build_filename (RIG_SHARE_DIR, "light-bulb.png", NULL);
      //full_path = g_build_filename (RUT_HANDSET_SHARE_DIR, "nine-slice-test.png", NULL);
      data->light_icon = rut_load_texture (data->ctx, full_path, &error);
      g_free (full_path);
      if (!data->light_icon)
        {
          g_warning ("Failed to load light-bulb texture: %s", error->message);
          cogl_error_free (error);
        }

      data->timeline_vp = rut_ui_viewport_new (data->ctx, 0, 0, NULL);
    }
#endif

  data->device_transform = rut_transform_new (data->ctx, NULL);

  data->camera = rut_camera_new (data->ctx, COGL_FRAMEBUFFER (data->onscreen));
  rut_camera_set_clear (data->camera, FALSE);

  /* XXX: Basically just a hack for now. We should have a
   * RutShellWindow type that internally creates a RutCamera that can
   * be used when handling input events in device coordinates.
   */
  rut_shell_set_window_camera (shell, data->camera);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      data->timeline_camera = rut_camera_new (data->ctx, fb);
      rut_camera_set_clear (data->timeline_camera, FALSE);
      rut_shell_add_input_camera (shell, data->timeline_camera, NULL);
      data->timeline_scale = 1;
      data->timeline_len = 20;
    }
#endif

  data->scene = rut_graph_new (data->ctx, NULL);

  /* Conceptually we rig the camera to an armature with a pivot fixed
   * at the current origin. This setup makes it straight forward to
   * model user navigation by letting us change the length of the
   * armature to handle zoom, rotating the armature to handle
   * middle-click rotating the scene with the mouse and moving the
   * position of the armature for shift-middle-click translations with
   * the mouse.
   *
   * It also simplifies things if all the viewport setup for the
   * camera is handled using entity transformations as opposed to
   * mixing entity transforms with manual camera view transforms.
   */

  data->editor_camera_to_origin = rut_entity_new (data->ctx, data->entity_next_id++);
  rut_graphable_add_child (data->scene, data->editor_camera_to_origin);
  rut_entity_set_label (data->editor_camera_to_origin, "rig:camera_to_origin");

  data->editor_camera_rotate = rut_entity_new (data->ctx, data->entity_next_id++);
  rut_graphable_add_child (data->editor_camera_to_origin, data->editor_camera_rotate);
  rut_entity_set_label (data->editor_camera_rotate, "rig:camera_rotate");

  data->editor_camera_armature = rut_entity_new (data->ctx, data->entity_next_id++);
  rut_graphable_add_child (data->editor_camera_rotate, data->editor_camera_armature);
  rut_entity_set_label (data->editor_camera_armature, "rig:camera_armature");

  data->editor_camera_origin_offset = rut_entity_new (data->ctx, data->entity_next_id++);
  rut_graphable_add_child (data->editor_camera_armature, data->editor_camera_origin_offset);
  rut_entity_set_label (data->editor_camera_origin_offset, "rig:camera_origin_offset");

  data->editor_camera_dev_scale = rut_entity_new (data->ctx, data->entity_next_id++);
  rut_graphable_add_child (data->editor_camera_origin_offset, data->editor_camera_dev_scale);
  rut_entity_set_label (data->editor_camera_dev_scale, "rig:camera_dev_scale");

  data->editor_camera_screen_pos = rut_entity_new (data->ctx, data->entity_next_id++);
  rut_graphable_add_child (data->editor_camera_dev_scale, data->editor_camera_screen_pos);
  rut_entity_set_label (data->editor_camera_screen_pos, "rig:camera_screen_pos");

  data->editor_camera_2d_view = rut_entity_new (data->ctx, data->entity_next_id++);
  //rut_graphable_add_child (data->editor_camera_screen_pos, data->editor_camera_2d_view); FIXME
  rut_entity_set_label (data->editor_camera_2d_view, "rig:camera_2d_view");

  data->editor_camera = rut_entity_new (data->ctx, data->entity_next_id++);
  //rut_graphable_add_child (data->editor_camera_2d_view, data->editor_camera); FIXME
  rut_graphable_add_child (data->editor_camera_screen_pos, data->editor_camera);
  rut_entity_set_label (data->editor_camera, "rig:camera");

  data->origin[0] = DEVICE_WIDTH / 2;
  data->origin[1] = DEVICE_HEIGHT / 2;
  data->origin[2] = 0;

  rut_entity_translate (data->editor_camera_to_origin,
                        data->origin[0],
                        data->origin[1],
                        data->origin[2]);
                        //DEVICE_WIDTH / 2, (DEVICE_HEIGHT / 2), 0);

  //rut_entity_rotate_z_axis (data->editor_camera_to_origin, 45);

  rut_entity_translate (data->editor_camera_origin_offset,
                        -DEVICE_WIDTH / 2, -(DEVICE_HEIGHT / 2), 0);

  /* FIXME: currently we also do a z translation due to using
   * cogl_matrix_view_2d_in_perspective, we should stop using that api so we can
   * do our z_2d translation here...
   *
   * XXX: should the camera_z transform be done for the negative translate?
   */
  //device scale = 0.389062494
  data->editor_camera_z = 0.f;
  rut_entity_translate (data->editor_camera_armature, 0, 0, data->editor_camera_z);

#if 0
  {
    float pos[3] = {0, 0, 0};
    rut_entity_set_position (data->editor_camera_rig, pos);
    rut_entity_translate (data->editor_camera_rig, 100, 100, 0);
  }
#endif

  //rut_entity_translate (data->editor_camera, 100, 100, 0);

#if 0
  data->editor_camera_z = 20.f;
  vector3[0] = 0.f;
  vector3[1] = 0.f;
  vector3[2] = data->editor_camera_z;
  rut_entity_set_position (data->editor_camera, vector3);
#else
  data->editor_camera_z = 10.f;
#endif

  data->editor_camera_component = rut_camera_new (data->ctx, fb);
  rut_camera_set_clear (data->editor_camera_component, FALSE);
  rut_entity_add_component (data->editor_camera, data->editor_camera_component);
  rut_shell_add_input_camera (shell,
                              data->editor_camera_component,
                              data->scene);

  data->editor_input_region =
    rut_input_region_new_rectangle (0, 0, 0, 0, editor_input_region_cb, data);
  rut_input_region_set_hud_mode (data->editor_input_region, TRUE);
  //rut_camera_add_input_region (data->camera,
  rut_camera_add_input_region (data->editor_camera_component,
                               data->editor_input_region);


  update_camera_position (data);

  data->current_camera = data->editor_camera;

  data->light = rut_entity_new (data->ctx, data->entity_next_id++);
  data->entities = g_list_prepend (data->entities, data->light);
#if 1
  vector3[0] = 0;
  vector3[1] = 0;
  vector3[2] = 500;
  rut_entity_set_position (data->light, vector3);

  rut_entity_rotate_x_axis (data->light, 20);
  rut_entity_rotate_y_axis (data->light, -20);
#endif

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      RutMesh *mesh = rut_mesh_new_from_template (data->ctx, "cube");

      data->light_handle = rut_entity_new (data->ctx, data->entity_next_id++);
      rut_entity_add_component (data->light_handle, mesh);
      rut_graphable_add_child (data->light, data->light_handle);
      rut_entity_set_scale (data->light_handle, 100);
      rut_entity_set_cast_shadow (data->light_handle, FALSE);
    }
#endif

  light = rut_light_new ();
  rut_color_init_from_4f (&color, .2f, .2f, .2f, 1.f);
  rut_light_set_ambient (light, &color);
  rut_color_init_from_4f (&color, .6f, .6f, .6f, 1.f);
  rut_light_set_diffuse (light, &color);
  rut_color_init_from_4f (&color, .4f, .4f, .4f, 1.f);
  rut_light_set_specular (light, &color);

  rut_entity_add_component (data->light, light);

  camera = rut_camera_new (data->ctx, COGL_FRAMEBUFFER (data->shadow_fb));
  data->shadow_map_camera = camera;

  rut_camera_set_background_color4f (camera, 0.f, .3f, 0.f, 1.f);
  rut_camera_set_projection_mode (camera,
                                  RUT_PROJECTION_ORTHOGRAPHIC);
  rut_camera_set_orthographic_coordinates (camera,
                                           -1000, -1000, 1000, 1000);
  rut_camera_set_near_plane (camera, 1.1f);
  rut_camera_set_far_plane (camera, 1500.f);

  rut_entity_add_component (data->light, camera);

  rut_graphable_add_child (data->scene, data->light);

  data->root =
    rut_graph_new (data->ctx,
                   //(data->main_transform = rut_transform_new (data->ctx, NULL)),
                   NULL);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      RutGraph *graph = rut_graph_new (data->ctx, NULL);
      RutTransform *transform;
      RutText *text;
      float x = 10;
      float width, height;

      rut_color_init_from_4f (&top_bar_ref_color, 0.41, 0.41, 0.41, 1.0);
      rut_color_init_from_4f (&main_area_ref_color, 0.22, 0.22, 0.22, 1.0);
      rut_color_init_from_4f (&right_bar_ref_color, 0.45, 0.45, 0.45, 1.0);

      data->splits[0] =
        rut_split_view_new (data->ctx,
                            RUT_SPLIT_VIEW_SPLIT_HORIZONTAL,
                            100,
                            100,
                            NULL);

      transform = rut_transform_new (data->ctx,
                                     (text = rut_text_new (data->ctx)), NULL);
      rut_transform_translate (transform, x, 5, 0);
      rut_text_set_text (text, "File");
      rut_graphable_add_child (graph, transform);
      rut_refable_unref (transform);
      rut_sizable_get_size (text, &width, &height);
      x += width + 30;

      transform = rut_transform_new (data->ctx,
                                     (text = rut_text_new (data->ctx)), NULL);
      rut_transform_translate (transform, x, 5, 0);
      rut_text_set_text (text, "Edit");
      rut_graphable_add_child (graph, transform);
      rut_refable_unref (transform);
      rut_sizable_get_size (text, &width, &height);
      x += width + 30;

      transform = rut_transform_new (data->ctx,
                                     (text = rut_text_new (data->ctx)), NULL);
      rut_transform_translate (transform, x, 5, 0);
      rut_text_set_text (text, "Help");
      rut_graphable_add_child (graph, transform);
      rut_refable_unref (transform);
      rut_sizable_get_size (text, &width, &height);
      x += width + 30;

      data->top_bar_stack =
        rut_stack_new (data->ctx, 0, 0,
                       (data->top_bar_rect =
                        rut_rectangle_new4f (data->ctx, 0, 0,
                                             0.41, 0.41, 0.41, 1)),
                       graph,
                       rut_bevel_new (data->ctx, 0, 0, &top_bar_ref_color),
                       NULL);

      rut_graphable_add_child (data->root, data->splits[0]);

      data->splits[1] = rut_split_view_new (data->ctx,
                                            RUT_SPLIT_VIEW_SPLIT_VERTICAL,
                                            100,
                                            100,
                                            NULL);

      rut_split_view_set_child0 (data->splits[0], data->top_bar_stack);
      rut_split_view_set_child1 (data->splits[0], data->splits[1]);

      data->splits[2] = rut_split_view_new (data->ctx,
                                            RUT_SPLIT_VIEW_SPLIT_HORIZONTAL,
                                            100,
                                            100,
                                            NULL);

      data->splits[3] = rut_split_view_new (data->ctx,
                                            RUT_SPLIT_VIEW_SPLIT_HORIZONTAL,
                                            100,
                                            100,
                                            NULL);

      data->splits[4] = rut_split_view_new (data->ctx,
                                            RUT_SPLIT_VIEW_SPLIT_VERTICAL,
                                            100,
                                            100,
                                            NULL);

      data->icon_bar_stack = rut_stack_new (data->ctx, 0, 0,
                                            (data->icon_bar_rect =
                                             rut_rectangle_new4f (data->ctx, 0, 0,
                                                                  0.41, 0.41, 0.41, 1)),
                                            rut_bevel_new (data->ctx, 0, 0, &top_bar_ref_color),
                                            NULL);
      rut_split_view_set_child0 (data->splits[3], data->splits[4]);
      rut_split_view_set_child1 (data->splits[3], data->icon_bar_stack);

      data->left_bar_stack = rut_stack_new (data->ctx, 0, 0,
                                            (data->left_bar_rect =
                                             rut_rectangle_new4f (data->ctx, 0, 0,
                                                                  0.57, 0.57, 0.57, 1)),
                                            (data->assets_vp =
                                             rut_ui_viewport_new (data->ctx,
                                                                  0, 0,
                                                                  NULL)),
                                            rut_bevel_new (data->ctx, 0, 0, &top_bar_ref_color),
                                            NULL);

      rut_ui_viewport_set_x_pannable (data->assets_vp, FALSE);

        {
          RutEntry *entry;
          RutText *text;
          RutTransform *transform;
          transform = rut_transform_new (data->ctx,
                                         (entry = rut_entry_new (data->ctx)), NULL);
          rut_transform_translate (transform, 20, 10, 0);
          rut_graphable_add_child (data->assets_vp, transform);

          text = rut_entry_get_text (entry);
          rut_text_set_editable (text, TRUE);
          rut_text_set_text (text, "Search...");
        }


      data->main_area_bevel = rut_bevel_new (data->ctx, 0, 0, &main_area_ref_color),

      rut_split_view_set_child0 (data->splits[4], data->left_bar_stack);
      rut_split_view_set_child1 (data->splits[4], data->main_area_bevel);

      data->bottom_bar_stack = rut_stack_new (data->ctx, 0, 0,
                                              (data->bottom_bar_rect =
                                               rut_rectangle_new4f (data->ctx, 0, 0,
                                                                    0.57, 0.57, 0.57, 1)),
                                              NULL);

      rut_split_view_set_child0 (data->splits[2], data->splits[3]);
      rut_split_view_set_child1 (data->splits[2], data->bottom_bar_stack);

      data->right_bar_stack =
        rut_stack_new (data->ctx, 100, 100,
                       (data->right_bar_rect =
                        rut_rectangle_new4f (data->ctx, 0, 0,
                                             0.57, 0.57, 0.57, 1)),
                       (data->tool_vp =
                        rut_ui_viewport_new (data->ctx, 0, 0, NULL)),
                       rut_bevel_new (data->ctx, 0, 0, &right_bar_ref_color),
                       NULL);

      rut_ui_viewport_set_x_pannable (data->tool_vp, FALSE);

      rut_split_view_set_child0 (data->splits[1], data->splits[2]);
      rut_split_view_set_child1 (data->splits[1], data->right_bar_stack);

      rut_split_view_set_split_offset (data->splits[0], 30);
      rut_split_view_set_split_offset (data->splits[1], 850);
      rut_split_view_set_split_offset (data->splits[2], 500);
      rut_split_view_set_split_offset (data->splits[3], 470);
      rut_split_view_set_split_offset (data->splits[4], 150);
    }
#endif

  rut_shell_add_input_camera (shell, data->camera, data->root);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      RutProperty *main_area_width =
        rut_introspectable_lookup_property (data->main_area_bevel, "width");
      RutProperty *main_area_height =
        rut_introspectable_lookup_property (data->main_area_bevel, "height");

      rut_property_set_binding_by_name (data->editor_camera_component,
                                        "viewport_x",
                                        camera_viewport_binding_cb,
                                        data,
                                        /* XXX: Hack: we are currently relying on
                                         * the bevel width being redundantly re-set
                                         * at times when bevel's position may have
                                         * also changed.
                                         *
                                         * FIXME: We need a proper allocation cycle
                                         * in Rut!
                                         */
                                        main_area_width,
                                        NULL);

      rut_property_set_binding_by_name (data->editor_camera_component,
                                        "viewport_y",
                                        camera_viewport_binding_cb,
                                        data,
                                        /* XXX: Hack: we are currently relying on
                                         * the bevel width being redundantly re-set
                                         * at times when bevel's position may have
                                         * also changed.
                                         *
                                         * FIXME: We need a proper allocation cycle
                                         * in Rut!
                                         */
                                        main_area_width,
                                        NULL);

      rut_property_set_binding_by_name (data->editor_camera_component,
                                        "viewport_width",
                                        camera_viewport_binding_cb,
                                        data,
                                        main_area_width,
                                        NULL);

      rut_property_set_binding_by_name (data->editor_camera_component,
                                        "viewport_height",
                                        camera_viewport_binding_cb,
                                        data,
                                        main_area_height,
                                        NULL);
    }
  else
#endif /* RIG_EDITOR_ENABLED */
    {
      int width = cogl_framebuffer_get_width (fb);
      int height = cogl_framebuffer_get_height (fb);

      rut_camera_set_viewport (data->editor_camera_component,
                               0, 0, width, height);

      rut_input_region_set_rectangle (data->editor_input_region,
                                      0, 0, width, height);

    }

#if 0
  data->slider_transform =
    rut_transform_new (data->ctx,
                       data->slider = rut_slider_new (data->ctx,
                                                      RUT_AXIS_X,
                                                      0, 1,
                                                      data->main_width));
  rut_graphable_add_child (data->bottom_bar_transform, data->slider_transform);

  data->slider_progress =
    rut_introspectable_lookup_property (data->slider, "progress");
#endif

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      data->timeline_input_region =
        rut_input_region_new_rectangle (0, 0, 0, 0,
                                        timeline_region_input_cb,
                                        data);
      rut_camera_add_input_region (data->timeline_camera,
                                   data->timeline_input_region);
    }
#endif /* RIG_EDITOR_ENABLED */

  data->timeline = rut_timeline_new (data->ctx, 20.0);
  rut_timeline_stop (data->timeline);

  data->timeline_elapsed =
    rut_introspectable_lookup_property (data->timeline, "elapsed");
  data->timeline_progress =
    rut_introspectable_lookup_property (data->timeline, "progress");

  /* tool */
  data->tool = rut_tool_new (data->shell);
  rut_tool_set_camera (data->tool, data->editor_camera);

  /* picking ray */
  data->picking_ray_color = cogl_pipeline_new (data->ctx->cogl_context);
  cogl_pipeline_set_color4f (data->picking_ray_color, 1.0, 0.0, 0.0, 1.0);

  allocate (data);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    set_play_mode_enabled (data, FALSE);
  else
#endif
    set_play_mode_enabled (data, TRUE);

#ifndef __ANDROID__
  if (_rig_handset_remaining_args &&
      _rig_handset_remaining_args[0])
    {
      if (_rig_handset_remaining_args[0])
        {
          struct stat st;

          _rut_project_dir = g_path_get_dirname (_rig_handset_remaining_args[0]);
          rut_set_assets_location (data->ctx, _rut_project_dir);

          stat (_rig_handset_remaining_args[0], &st);
          if (S_ISREG (st.st_mode))
            rig_load (data, _rig_handset_remaining_args[0]);
        }
    }
#endif
}

static void
fini (RutShell *shell, void *user_data)
{
  RigData *data = user_data;
  int i;

  rut_refable_unref (data->camera);
  rut_refable_unref (data->root);

  for (i = 0; i < RUT_DATA_N_PROPS; i++)
    rut_property_destroy (&data->properties[i]);

  cogl_object_unref (data->circle_texture);

  cogl_object_unref (data->circle_node_attribute);

  rut_dof_effect_free (data->dof);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_refable_unref (data->timeline_vp);
      cogl_object_unref (data->grid_prim);
      cogl_object_unref (data->light_icon);
    }
#endif
}

static RutInputEventStatus
shell_input_handler (RutInputEvent *event, void *user_data)
{
  RigData *data = user_data;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      /* Anything that can claim the keyboard focus will do so during
       * motion events so we clear it before running other input
       * callbacks */
      data->key_focus_callback = NULL;
    }

  switch (rut_input_event_get_type (event))
    {
    case RUT_INPUT_EVENT_TYPE_MOTION:
#if 0
      switch (rut_motion_event_get_action (event))
        {
        case RUT_MOTION_EVENT_ACTION_DOWN:
          //g_print ("Press Down\n");
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        case RUT_MOTION_EVENT_ACTION_UP:
          //g_print ("Release\n");
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        case RUT_MOTION_EVENT_ACTION_MOVE:
          //g_print ("Move\n");
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
#endif
      break;

    case RUT_INPUT_EVENT_TYPE_KEY:
      {
        if (data->key_focus_callback)
          data->key_focus_callback (event, data);
      }
      break;
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

typedef struct _AssetInputClosure
{
  RutAsset *asset;
  RigData *data;
} AssetInputClosure;

static RutInputEventStatus
asset_input_cb (RutInputRegion *region,
                RutInputEvent *event,
                void *user_data)
{
  AssetInputClosure *closure = user_data;
  RutAsset *asset = closure->asset;
  RigData *data = closure->data;

  g_print ("Asset input\n");

  if (rut_asset_get_type (asset) != RUT_ASSET_TYPE_TEXTURE)
    return RUT_INPUT_EVENT_STATUS_UNHANDLED;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_DOWN)
        {
          RutEntity *entity = rut_entity_new (data->ctx,
                                              data->entity_next_id++);
          CoglTexture *texture = rut_asset_get_texture (asset);
          RutMaterial *material = rut_material_new (data->ctx, asset, NULL);
          RutDiamond *diamond =
            rut_diamond_new (data->ctx,
                             400,
                             cogl_texture_get_width (texture),
                             cogl_texture_get_height (texture));
          rut_entity_add_component (entity, material);
          rut_entity_add_component (entity, diamond);

          data->selected_entity = entity;
          rut_graphable_add_child (data->scene, entity);

          update_inspector (data);

          rut_shell_queue_redraw (data->ctx->shell);
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

#if 0
static RutInputEventStatus
add_light_cb (RutInputRegion *region,
              RutInputEvent *event,
              void *user_data)
{
  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_DOWN)
        {
          g_print ("Add light!\n");
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}
#endif

static void
add_asset_icon (RigData *data,
                RutAsset *asset,
                float y_pos)
{
  AssetInputClosure *closure;
  CoglTexture *texture;
  RutNineSlice *nine_slice;
  RutInputRegion *region;
  RutTransform *transform;

  if (rut_asset_get_type (asset) != RUT_ASSET_TYPE_TEXTURE)
    return;

  closure = g_slice_new (AssetInputClosure);
  closure->asset = asset;
  closure->data = data;

  texture = rut_asset_get_texture (asset);

  transform =
    rut_transform_new (data->ctx,
                       (nine_slice = rut_nine_slice_new (data->ctx,
                                                         texture,
                                                         0, 0, 0, 0,
                                                         100, 100)),
                       (region =
                        rut_input_region_new_rectangle (0, 0, 100, 100,
                                                        asset_input_cb,
                                                        closure)),
                       NULL);
  rut_graphable_add_child (data->assets_list, transform);

  /* XXX: It could be nicer to have some form of weak pointer
   * mechanism to manage the lifetime of these closures... */
  data->asset_input_closures = g_list_prepend (data->asset_input_closures,
                                               closure);

  rut_transform_translate (transform, 10, y_pos, 0);

  //rut_input_region_set_graphable (region, nine_slice);

  rut_refable_unref (transform);
  rut_refable_unref (nine_slice);
  rut_refable_unref (region);
}

static void
free_asset_input_closures (RigData *data)
{
  GList *l;

  for (l = data->asset_input_closures; l; l = l->next)
    g_slice_free (AssetInputClosure, l->data);
  g_list_free (data->asset_input_closures);
  data->asset_input_closures = NULL;
}

void
rig_update_asset_list (RigData *data)
{
  GList *l;
  int i = 0;
  RutObject *doc_node;

  if (data->assets_list)
    {
      rut_graphable_remove_child (data->assets_list);
      free_asset_input_closures (data);
    }

  data->assets_list = rut_graph_new (data->ctx, NULL);

  doc_node = rut_ui_viewport_get_doc_node (data->assets_vp);
  rut_graphable_add_child (doc_node, data->assets_list);
  rut_refable_unref (data->assets_list);

  for (l = data->assets, i= 0; l; l = l->next, i++)
    add_asset_icon (data, l->data, 70 + 110 * i);

  //add_asset_icon (data, data->light_icon, 10 + 110 * i++, add_light_cb, NULL);
}

void
rig_free_ux (RigData *data)
{
  GList *l;

  for (l = data->transitions; l; l = l->next)
    rig_transition_free (l->data);
  g_list_free (data->transitions);
  data->transitions = NULL;

  for (l = data->assets; l; l = l->next)
    rut_refable_unref (l->data);
  g_list_free (data->assets);
  data->assets = NULL;

  free_asset_input_closures (data);
}

static void
init_types (void)
{
}

#ifdef __ANDROID__

void
android_main (struct android_app *application)
{
  RigData data;

  /* Make sure glue isn't stripped */
  app_dummy ();

  g_android_init ();

  memset (&data, 0, sizeof (RigData));
  data.app = application;

  init_types ();

  data.shell = rut_android_shell_new (application,
                                      init,
                                      fini,
                                      paint,
                                      &data);

  data.ctx = rut_context_new (data.shell);

  rut_context_init (data.ctx);

  rut_shell_set_input_callback (data.shell, shell_input_handler, &data);

  rut_shell_main (data.shell);
}

#else

int
main (int argc, char **argv)
{
  RigData data;
  GOptionContext *context = g_option_context_new (NULL);
  GError *error = NULL;

  g_option_context_add_main_entries (context, rut_handset_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_error ("option parsing failed: %s\n", error->message);
      exit(EXIT_FAILURE);
    }

  memset (&data, 0, sizeof (RigData));

  init_types ();

  data.shell = rut_shell_new (init, fini, paint, &data);

  data.ctx = rut_context_new (data.shell);

  rut_context_init (data.ctx);

  rut_shell_add_input_callback (data.shell, shell_input_handler, &data, NULL);

  rut_shell_main (data.shell);

  return 0;
}

#endif