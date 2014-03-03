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

#include <rut.h>

#include "rig-material.h"
#include "rig-pointalism-grid.h"
#include "rig-asset.h"

static RutPropertySpec _rig_material_prop_specs[] = {
  {
    .name = "visible",
    .type = RUT_PROPERTY_TYPE_BOOLEAN,
    .getter.boolean_type = rig_material_get_visible,
    .setter.boolean_type = rig_material_set_visible,
    .nick = "Visible",
    .blurb = "Whether the material is visible or not",
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = TRUE
  },
  {
    .name = "cast_shadow",
    .type = RUT_PROPERTY_TYPE_BOOLEAN,
    .getter.boolean_type = rig_material_get_cast_shadow,
    .setter.boolean_type = rig_material_set_cast_shadow,
    .nick = "Cast Shadow",
    .blurb = "Whether the material casts shadows or not",
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = TRUE
  },
  {
    .name = "receive_shadow",
    .type = RUT_PROPERTY_TYPE_BOOLEAN,
    .getter.boolean_type = rig_material_get_receive_shadow,
    .setter.boolean_type = rig_material_set_receive_shadow,
    .nick = "Receive Shadow",
    .blurb = "Whether the material receives shadows or not",
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = TRUE
  },
  {
    .name = "color_source",
    .nick = "Color Source",
    .type = RUT_PROPERTY_TYPE_ASSET,
    .validation = { .asset.type = RIG_ASSET_TYPE_TEXTURE },
    .getter.asset_type = rig_material_get_color_source_asset,
    .setter.asset_type = rig_material_set_color_source_asset,
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = FALSE
  },
  {
    .name = "normal_map",
    .nick = "Normal Map",
    .type = RUT_PROPERTY_TYPE_ASSET,
    .validation = { .asset.type = RIG_ASSET_TYPE_NORMAL_MAP },
    .getter.asset_type = rig_material_get_normal_map_asset,
    .setter.asset_type = rig_material_set_normal_map_asset,
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = FALSE
  },
  {
    .name = "alpha_mask",
    .nick = "Alpha Mask",
    .type = RUT_PROPERTY_TYPE_ASSET,
    .validation = { .asset.type = RIG_ASSET_TYPE_ALPHA_MASK },
    .getter.asset_type = rig_material_get_alpha_mask_asset,
    .setter.asset_type = rig_material_set_alpha_mask_asset,
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = FALSE
  },
  {
    .name = "ambient",
    .nick = "Ambient",
    .type = RUT_PROPERTY_TYPE_COLOR,
    .getter.color_type = rig_material_get_ambient,
    .setter.color_type = rig_material_set_ambient,
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = TRUE
  },
  {
    .name = "diffuse",
    .nick = "Diffuse",
    .type = RUT_PROPERTY_TYPE_COLOR,
    .getter.color_type = rig_material_get_diffuse,
    .setter.color_type = rig_material_set_diffuse,
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = TRUE
  },
  {
    .name = "specular",
    .nick = "Specular",
    .type = RUT_PROPERTY_TYPE_COLOR,
    .getter.color_type = rig_material_get_specular,
    .setter.color_type = rig_material_set_specular,
    .flags = RUT_PROPERTY_FLAG_READWRITE,
    .animatable = TRUE
  },
  {
    .name = "shininess",
    .nick = "Shininess",
    .type = RUT_PROPERTY_TYPE_FLOAT,
    .getter.float_type = rig_material_get_shininess,
    .setter.float_type = rig_material_set_shininess,
    .flags = RUT_PROPERTY_FLAG_READWRITE |
      RUT_PROPERTY_FLAG_VALIDATE,
    .validation = { .float_range = { 0, 1000 }},
    .animatable = TRUE
  },
  {
    .name = "alpha-mask-threshold",
    .nick = "Alpha Threshold",
    .type = RUT_PROPERTY_TYPE_FLOAT,
    .getter.float_type = rig_material_get_alpha_mask_threshold,
    .setter.float_type = rig_material_set_alpha_mask_threshold,
    .flags = RUT_PROPERTY_FLAG_READWRITE |
      RUT_PROPERTY_FLAG_VALIDATE,
    .validation = { .float_range = { 0, 1 }},
    .animatable = TRUE
  },
  { 0 }
};

static void
_rig_material_free (void *object)
{
  RigMaterial *material = object;

#ifdef RIG_ENABLE_DEBUG
  {
    RutComponentableProps *component =
      rut_object_get_properties (object, RUT_TRAIT_ID_COMPONENTABLE);
    g_return_if_fail (component->entity == NULL);
  }
#endif

  if (material->color_source_asset)
    rut_object_unref (material->color_source_asset);

  if (material->normal_map_asset)
    rut_object_unref (material->normal_map_asset);

  if (material->alpha_mask_asset)
    rut_object_unref (material->alpha_mask_asset);

  rut_introspectable_destroy (material);

  rut_object_free (RigMaterial, material);
}

static RutObject *
_rig_material_copy (RutObject *object)
{
  RigMaterial *material = object;
  RigEntity *entity = material->component.entity;
  RutContext *ctx = rig_entity_get_context (entity);
  RigMaterial *copy = rig_material_new (ctx, NULL);

  copy->visible = material->cast_shadow;
  copy->cast_shadow = material->cast_shadow;
  copy->receive_shadow = material->receive_shadow;

  if (material->color_source_asset)
    copy->color_source_asset = rut_object_ref (material->color_source_asset);
  if (material->normal_map_asset)
    copy->normal_map_asset = rut_object_ref (material->normal_map_asset);
  if (material->alpha_mask_asset)
    copy->alpha_mask_asset = rut_object_ref (material->alpha_mask_asset);

  copy->ambient = material->ambient;
  copy->diffuse = material->diffuse;
  copy->specular = material->specular;
  copy->shininess = material->shininess;
  copy->alpha_mask_threshold = material->alpha_mask_threshold;

  return copy;
}

RutType rig_material_type;

void
_rig_material_init_type (void)
{

  static RutComponentableVTable componentable_vtable = {
    .copy = _rig_material_copy
  };


  RutType *type = &rig_material_type;
#define TYPE RigMaterial

  rut_type_init (type, G_STRINGIFY (TYPE), _rig_material_free);
  rut_type_add_trait (type,
                      RUT_TRAIT_ID_COMPONENTABLE,
                      offsetof (TYPE, component),
                      &componentable_vtable);
  rut_type_add_trait (type,
                      RUT_TRAIT_ID_INTROSPECTABLE,
                      offsetof (TYPE, introspectable),
                      NULL); /* no implied vtable */

#undef TYPE
}

RigMaterial *
rig_material_new (RutContext *ctx,
                  RigAsset *asset)
{
  RigMaterial *material =
    rut_object_alloc0 (RigMaterial, &rig_material_type, _rig_material_init_type);



  material->component.type = RUT_COMPONENT_TYPE_MATERIAL;

  material->visible = TRUE;
  material->receive_shadow = TRUE;

  cogl_color_init_from_4f (&material->ambient, 0.23, 0.23, 0.23, 1);
  cogl_color_init_from_4f (&material->diffuse, 0.75, 0.75, 0.75, 1);
  cogl_color_init_from_4f (&material->specular, 0.64, 0.64, 0.64, 1);

  material->shininess = 100;

  rut_introspectable_init (material,
                           _rig_material_prop_specs,
                           material->properties);

  material->uniforms_flush_age = -1;

  material->color_source_asset = NULL;
  material->normal_map_asset = NULL;
  material->alpha_mask_asset = NULL;

  if (asset)
    {
      switch (rig_asset_get_type (asset))
        {
        case RIG_ASSET_TYPE_TEXTURE:
          material->color_source_asset = rut_object_ref (asset);
          break;
        case RIG_ASSET_TYPE_NORMAL_MAP:
          material->normal_map_asset = rut_object_ref (asset);
          break;
        case RIG_ASSET_TYPE_ALPHA_MASK:
          material->alpha_mask_asset = rut_object_ref (asset);
          break;
        default:
          g_warn_if_reached ();
        }
    }

  return material;
}

void
rig_material_set_color_source_asset (RutObject *object,
                                     RigAsset *color_source_asset)
{
  RigMaterial *material = object;

  if (material->color_source_asset == color_source_asset)
    return;

  if (material->color_source_asset)
    {
      rut_object_unref (material->color_source_asset);
      material->color_source_asset = NULL;
    }

  material->color_source_asset = color_source_asset;
  if (color_source_asset)
    rut_object_ref (color_source_asset);

  if (material->component.entity)
    rig_entity_notify_changed (material->component.entity);
}

RigAsset *
rig_material_get_color_source_asset (RutObject *object)
{
  RigMaterial *material = object;
  return material->color_source_asset;
}

void
rig_material_set_normal_map_asset (RutObject *object,
                                   RigAsset *normal_map_asset)
{
  RigMaterial *material = object;

  if (material->normal_map_asset == normal_map_asset)
    return;

  if (material->normal_map_asset)
    {
      rut_object_unref (material->normal_map_asset);
      material->normal_map_asset = NULL;
    }

  material->normal_map_asset = normal_map_asset;

  if (normal_map_asset)
    rut_object_ref (normal_map_asset);

  if (material->component.entity)
    rig_entity_notify_changed (material->component.entity);
}

RigAsset *
rig_material_get_normal_map_asset (RutObject *object)
{
  RigMaterial *material = object;

  return material->normal_map_asset;
}

void
rig_material_set_alpha_mask_asset (RutObject *object,
                                   RigAsset *alpha_mask_asset)
{
  RigMaterial *material = object;

  if (material->alpha_mask_asset == alpha_mask_asset)
    return;

  if (material->alpha_mask_asset)
    {
      rut_object_unref (material->alpha_mask_asset);
      material->alpha_mask_asset = NULL;
    }

  material->alpha_mask_asset = alpha_mask_asset;

  if (alpha_mask_asset)
    rut_object_ref (alpha_mask_asset);

  if (material->component.entity)
    rig_entity_notify_changed (material->component.entity);
}

RigAsset *
rig_material_get_alpha_mask_asset (RutObject *object)
{
  RigMaterial *material = object;

  return material->alpha_mask_asset;
}

void
rig_material_set_ambient (RutObject *obj,
                          const CoglColor *color)
{
  RigMaterial *material = obj;
  RigEntity *entity;
  RutContext *ctx;

  material->ambient = *color;
  material->uniforms_age++;

  entity = material->component.entity;
  ctx = rig_entity_get_context (entity);
  rut_property_dirty (&ctx->property_ctx,
                      &material->properties[RIG_MATERIAL_PROP_AMBIENT]);
}

const CoglColor *
rig_material_get_ambient (RutObject *obj)
{
  RigMaterial *material = obj;

  return &material->ambient;
}

void
rig_material_set_diffuse (RutObject *obj,
                          const CoglColor *color)
{
  RigMaterial *material = obj;
  RigEntity *entity;
  RutContext *ctx;

  material->diffuse = *color;
  material->uniforms_age++;

  entity = material->component.entity;
  ctx = rig_entity_get_context (entity);
  rut_property_dirty (&ctx->property_ctx,
                      &material->properties[RIG_MATERIAL_PROP_DIFFUSE]);
}

const CoglColor *
rig_material_get_diffuse (RutObject *obj)
{
  RigMaterial *material = obj;

  return &material->diffuse;
}

void
rig_material_set_specular (RutObject *obj,
                           const CoglColor *color)
{
  RigMaterial *material = obj;
  RigEntity *entity;
  RutContext *ctx;

  material->specular = *color;
  material->uniforms_age++;

  entity = material->component.entity;
  ctx = rig_entity_get_context (entity);
  rut_property_dirty (&ctx->property_ctx,
                      &material->properties[RIG_MATERIAL_PROP_SPECULAR]);
}

const CoglColor *
rig_material_get_specular (RutObject *obj)
{
  RigMaterial *material = obj;

  return &material->specular;
}

void
rig_material_set_shininess (RutObject *obj,
                            float shininess)
{
  RigMaterial *material = obj;
  RigEntity *entity;
  RutContext *ctx;

  material->shininess = shininess;
  material->uniforms_age++;

  entity = material->component.entity;
  ctx = rig_entity_get_context (entity);
  rut_property_dirty (&ctx->property_ctx,
                      &material->properties[RIG_MATERIAL_PROP_SPECULAR]);
}

float
rig_material_get_shininess (RutObject *obj)
{
  RigMaterial *material = obj;

  return material->shininess;
}

float
rig_material_get_alpha_mask_threshold (RutObject *obj)
{
  RigMaterial *material = obj;

  return material->alpha_mask_threshold;
}

void
rig_material_set_alpha_mask_threshold (RutObject *obj,
                                       float threshold)
{
  RigMaterial *material = obj;
  RigEntity *entity;
  RutContext *ctx;

  if (material->alpha_mask_threshold == threshold)
    return;

  material->alpha_mask_threshold = threshold;

  entity = material->component.entity;
  ctx = rig_entity_get_context (entity);
  rut_property_dirty (&ctx->property_ctx,
                      &material->properties[RIG_MATERIAL_PROP_ALPHA_MASK_THRESHOLD]);
}

void
rig_material_flush_uniforms (RigMaterial *material,
                             CoglPipeline *pipeline)
{
  int location;
  RutObject *geo;
  RigEntity *entity = material->component.entity;

  //if (material->uniforms_age == material->uniforms_flush_age)
  //  return;

  location = cogl_pipeline_get_uniform_location (pipeline, "material_ambient");
  cogl_pipeline_set_uniform_float (pipeline,
                                   location,
                                   4, 1,
                                   (float *)&material->ambient);

  location = cogl_pipeline_get_uniform_location (pipeline, "material_diffuse");
  cogl_pipeline_set_uniform_float (pipeline,
                                   location,
                                   4, 1,
                                   (float *)&material->diffuse);

  location = cogl_pipeline_get_uniform_location (pipeline, "material_specular");
  cogl_pipeline_set_uniform_float (pipeline,
                                   location,
                                   4, 1,
                                   (float *)&material->specular);

  location = cogl_pipeline_get_uniform_location (pipeline,
                                                 "material_shininess");
  cogl_pipeline_set_uniform_1f (pipeline, location, material->shininess);

  location = cogl_pipeline_get_uniform_location (pipeline,
                                                 "material_alpha_threshold");
  cogl_pipeline_set_uniform_1f (pipeline, location, material->alpha_mask_threshold);

  geo = rig_entity_get_component (entity, RUT_COMPONENT_TYPE_GEOMETRY);

  if (rut_object_get_type (geo) == &rig_pointalism_grid_type &&
      material->color_source_asset)
    {
      int scale, z;
      scale = rig_pointalism_grid_get_scale (geo);
      z = rig_pointalism_grid_get_z (geo);

      location = cogl_pipeline_get_uniform_location (pipeline,
                                                     "scale_factor");
      cogl_pipeline_set_uniform_1f (pipeline, location, scale);

      location = cogl_pipeline_get_uniform_location (pipeline, "z_trans");
      cogl_pipeline_set_uniform_1f (pipeline, location, z);

      location = cogl_pipeline_get_uniform_location (pipeline, "anti_scale");
      if (rig_pointalism_grid_get_lighter (geo))
        cogl_pipeline_set_uniform_1i (pipeline, location, 1);
      else
        cogl_pipeline_set_uniform_1i (pipeline, location, 0);
    }

  material->uniforms_flush_age = material->uniforms_age;
}

void
rig_material_dirty_uniforms (RigMaterial *material)
{
  material->uniforms_flush_age = material->uniforms_age -1;
}

bool
rig_material_get_cast_shadow (RutObject *obj)
{
  RigMaterial *material = obj;

  return material->cast_shadow;
}

void
rig_material_set_cast_shadow (RutObject *obj, bool cast_shadow)
{
  RigMaterial *material = obj;
  RigEntity *entity;
  RutContext *ctx;

  if (material->cast_shadow == cast_shadow)
    return;

  material->cast_shadow = cast_shadow;

  entity = material->component.entity;
  ctx = rig_entity_get_context (entity);
  rut_property_dirty (&ctx->property_ctx,
                      &material->properties[RIG_MATERIAL_PROP_CAST_SHADOW]);
}

bool
rig_material_get_receive_shadow (RutObject *obj)
{
  RigMaterial *material = obj;

  return material->receive_shadow;
}

void
rig_material_set_receive_shadow (RutObject *obj,
                                 bool receive_shadow)
{
  RigMaterial *material = obj;
  RigEntity *entity;
  RutContext *ctx;

  if (material->receive_shadow == receive_shadow)
    return;

  material->receive_shadow = receive_shadow;

  entity = material->component.entity;
  ctx = rig_entity_get_context (entity);
  rut_property_dirty (&ctx->property_ctx,
                      &material->properties[RIG_MATERIAL_PROP_RECEIVE_SHADOW]);

  rig_entity_notify_changed (entity);
}

bool
rig_material_get_visible (RutObject *obj)
{
  RigMaterial *material = obj;

  return material->visible;
}

void
rig_material_set_visible (RutObject *obj, bool visible)
{
  RigMaterial *material = obj;
  RigEntity *entity;
  RutContext *ctx;

  if (material->visible == visible)
    return;

  material->visible = visible;

  entity = material->component.entity;
  ctx = rig_entity_get_context (entity);
  rut_property_dirty (&ctx->property_ctx,
                      &material->properties[RIG_MATERIAL_PROP_VISIBLE]);
}