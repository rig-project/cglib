/*
 * Cogl
 *
 * A Low-Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2010 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 *
 * Authors:
 *   Robert Bragg <robert@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl-util.h"
#include "cogl-context-private.h"
#include "cogl-object-private.h"
#include "cogl-journal-private.h"
#include "cogl-attribute.h"
#include "cogl-attribute-private.h"
#include "cogl-pipeline.h"
#include "cogl-pipeline-private.h"
#include "cogl-pipeline-opengl-private.h"
#include "cogl-texture-private.h"
#include "cogl-framebuffer-private.h"
#include "cogl-indices-private.h"
#ifdef CG_PIPELINE_PROGEND_GLSL
#include "cogl-pipeline-progend-glsl-private.h"
#endif
#include "cogl-private.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* This isn't defined in the GLES headers */
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif

static void _cg_attribute_free(cg_attribute_t *attribute);

CG_OBJECT_DEFINE(Attribute, attribute);

static bool
validate_cg_attribute_name(const char *name,
                           char **real_attribute_name,
                           cg_attribute_name_id_t *name_id,
                           bool *normalized,
                           int *layer_number)
{
    name = name + 3; /* skip "cg_" */

    *normalized = false;
    *layer_number = 0;

    if (strcmp(name, "position_in") == 0)
        *name_id = CG_ATTRIBUTE_NAME_ID_POSITION_ARRAY;
    else if (strcmp(name, "color_in") == 0) {
        *name_id = CG_ATTRIBUTE_NAME_ID_COLOR_ARRAY;
        *normalized = true;
    } else if (strcmp(name, "tex_coord_in") == 0) {
        *real_attribute_name = "cg_tex_coord0_in";
        *name_id = CG_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY;
    } else if (strncmp(name, "tex_coord", strlen("tex_coord")) == 0) {
        char *endptr;
        *layer_number = strtoul(name + 9, &endptr, 10);
        if (strcmp(endptr, "_in") != 0) {
            c_warning("Texture coordinate attributes should either be named "
                      "\"cg_tex_coord_in\" or named with a texture unit index "
                      "like \"cg_tex_coord2_in\"\n");
            return false;
        }
        *name_id = CG_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY;
    } else if (strcmp(name, "normal_in") == 0) {
        *name_id = CG_ATTRIBUTE_NAME_ID_NORMAL_ARRAY;
        *normalized = true;
    } else if (strcmp(name, "point_size_in") == 0)
        *name_id = CG_ATTRIBUTE_NAME_ID_POINT_SIZE_ARRAY;
    else {
        c_warning("Unknown cg_* attribute name cg_%s\n", name);
        return false;
    }

    return true;
}

cg_attribute_name_state_t *
_cg_attribute_register_attribute_name(cg_context_t *context, const char *name)
{
    cg_attribute_name_state_t *name_state = c_new(cg_attribute_name_state_t, 1);
    int name_index = context->n_attribute_names++;
    char *name_copy = c_strdup(name);

    name_state->name = NULL;
    name_state->name_index = name_index;
    if (strncmp(name, "cg_", 3) == 0) {
        if (!validate_cg_attribute_name(name,
                                        &name_state->name,
                                        &name_state->name_id,
                                        &name_state->normalized_default,
                                        &name_state->layer_number))
            goto error;
    } else {
        name_state->name_id = CG_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY;
        name_state->normalized_default = false;
        name_state->layer_number = 0;
    }

    if (name_state->name == NULL)
        name_state->name = name_copy;

    c_hash_table_insert(
        context->attribute_name_states_hash, name_copy, name_state);

    if (C_UNLIKELY(context->attribute_name_index_map == NULL))
        context->attribute_name_index_map =
            c_array_new(false, false, sizeof(void *));

    c_array_set_size(context->attribute_name_index_map, name_index + 1);

    c_array_index(context->attribute_name_index_map,
                  cg_attribute_name_state_t *,
                  name_index) = name_state;

    return name_state;

error:
    c_free(name_state);
    return NULL;
}

static bool
validate_n_components(const cg_attribute_name_state_t *name_state,
                      int n_components)
{
    switch (name_state->name_id) {
    case CG_ATTRIBUTE_NAME_ID_POSITION_ARRAY:
        if (C_UNLIKELY(n_components == 1)) {
            c_critical("glVertexPointer doesn't allow 1 component vertex "
                       "positions so we currently only support \"cg_vertex\" "
                       "attributes where n_components == 2, 3 or 4");
            return false;
        }
        break;
    case CG_ATTRIBUTE_NAME_ID_COLOR_ARRAY:
        if (C_UNLIKELY(n_components != 3 && n_components != 4)) {
            c_critical("glColorPointer expects 3 or 4 component colors so we "
                       "currently only support \"cg_color\" attributes where "
                       "n_components == 3 or 4");
            return false;
        }
        break;
    case CG_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY:
        break;
    case CG_ATTRIBUTE_NAME_ID_NORMAL_ARRAY:
        if (C_UNLIKELY(n_components != 3)) {
            c_critical("glNormalPointer expects 3 component normals so we "
                       "currently only support \"cg_normal\" attributes "
                       "where n_components == 3");
            return false;
        }
        break;
    case CG_ATTRIBUTE_NAME_ID_POINT_SIZE_ARRAY:
        if (C_UNLIKELY(n_components != 1)) {
            c_critical("The point size attribute can only have one "
                       "component");
            return false;
        }
        break;
    case CG_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY:
        return true;
    }

    return true;
}

cg_attribute_t *
cg_attribute_new(cg_attribute_buffer_t *attribute_buffer,
                 const char *name,
                 size_t stride,
                 size_t offset,
                 int n_components,
                 cg_attribute_type_t type)
{
    cg_attribute_t *attribute = c_slice_new(cg_attribute_t);
    cg_buffer_t *buffer = CG_BUFFER(attribute_buffer);
    cg_context_t *ctx = buffer->context;

    attribute->is_buffered = true;

    attribute->name_state =
        c_hash_table_lookup(ctx->attribute_name_states_hash, name);
    if (!attribute->name_state) {
        cg_attribute_name_state_t *name_state =
            _cg_attribute_register_attribute_name(ctx, name);
        if (!name_state)
            goto error;
        attribute->name_state = name_state;
    }

    attribute->d.buffered.attribute_buffer = cg_object_ref(attribute_buffer);
    attribute->d.buffered.stride = stride;
    attribute->d.buffered.offset = offset;
    attribute->d.buffered.n_components = n_components;
    attribute->d.buffered.type = type;

    attribute->immutable_ref = 0;

    if (attribute->name_state->name_id != CG_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY) {
        if (!validate_n_components(attribute->name_state, n_components))
            return NULL;
        attribute->normalized = attribute->name_state->normalized_default;
    } else
        attribute->normalized = false;

    return _cg_attribute_object_new(attribute);

error:
    _cg_attribute_free(attribute);
    return NULL;
}

static cg_attribute_t *
_cg_attribute_new_const(cg_context_t *context,
                        const char *name,
                        int n_components,
                        int n_columns,
                        bool transpose,
                        const float *value)
{
    cg_attribute_t *attribute = c_slice_new(cg_attribute_t);

    attribute->name_state =
        c_hash_table_lookup(context->attribute_name_states_hash, name);
    if (!attribute->name_state) {
        cg_attribute_name_state_t *name_state =
            _cg_attribute_register_attribute_name(context, name);
        if (!name_state)
            goto error;
        attribute->name_state = name_state;
    }

    if (!validate_n_components(attribute->name_state, n_components))
        goto error;

    attribute->is_buffered = false;
    attribute->normalized = false;

    attribute->d.constant.context = cg_object_ref(context);

    attribute->d.constant.boxed.v.array = NULL;

    if (n_columns == 1) {
        _cg_boxed_value_set_float(
            &attribute->d.constant.boxed, n_components, 1, value);
    } else {
        /* FIXME: Up until GL[ES] 3 only square matrices were supported
         * and we don't currently expose non-square matrices in Cogl.
         */
        _CG_RETURN_VAL_IF_FAIL(n_columns == n_components, NULL);
        _cg_boxed_value_set_matrix(
            &attribute->d.constant.boxed, n_columns, 1, transpose, value);
    }

    return _cg_attribute_object_new(attribute);

error:
    _cg_attribute_free(attribute);
    return NULL;
}

cg_attribute_t *
cg_attribute_new_const_1f(cg_context_t *context, const char *name, float value)
{
    return _cg_attribute_new_const(context,
                                   name,
                                   1, /* n_components */
                                   1, /* 1 column vector */
                                   false, /* no transpose */
                                   &value);
}

cg_attribute_t *
cg_attribute_new_const_2fv(cg_context_t *context,
                           const char *name,
                           const float *value)
{
    return _cg_attribute_new_const(context,
                                   name,
                                   2, /* n_components */
                                   1, /* 1 column vector */
                                   false, /* no transpose */
                                   value);
}

cg_attribute_t *
cg_attribute_new_const_3fv(cg_context_t *context,
                           const char *name,
                           const float *value)
{
    return _cg_attribute_new_const(context,
                                   name,
                                   3, /* n_components */
                                   1, /* 1 column vector */
                                   false, /* no transpose */
                                   value);
}

cg_attribute_t *
cg_attribute_new_const_4fv(cg_context_t *context,
                           const char *name,
                           const float *value)
{
    return _cg_attribute_new_const(context,
                                   name,
                                   4, /* n_components */
                                   1, /* 1 column vector */
                                   false, /* no transpose */
                                   value);
}

cg_attribute_t *
cg_attribute_new_const_2f(cg_context_t *context,
                          const char *name,
                          float component0,
                          float component1)
{
    float vec2[2] = { component0, component1 };
    return _cg_attribute_new_const(context,
                                   name,
                                   2, /* n_components */
                                   1, /* 1 column vector */
                                   false, /* no transpose */
                                   vec2);
}

cg_attribute_t *
cg_attribute_new_const_3f(cg_context_t *context,
                          const char *name,
                          float component0,
                          float component1,
                          float component2)
{
    float vec3[3] = { component0, component1, component2 };
    return _cg_attribute_new_const(context,
                                   name,
                                   3, /* n_components */
                                   1, /* 1 column vector */
                                   false, /* no transpose */
                                   vec3);
}

cg_attribute_t *
cg_attribute_new_const_4f(cg_context_t *context,
                          const char *name,
                          float component0,
                          float component1,
                          float component2,
                          float component3)
{
    float vec4[4] = { component0, component1, component2, component3 };
    return _cg_attribute_new_const(context,
                                   name,
                                   4, /* n_components */
                                   1, /* 1 column vector */
                                   false, /* no transpose */
                                   vec4);
}

cg_attribute_t *
cg_attribute_new_const_2x2fv(cg_context_t *context,
                             const char *name,
                             const float *matrix2x2,
                             bool transpose)
{
    return _cg_attribute_new_const(context,
                                   name,
                                   2, /* n_components */
                                   2, /* 2 column vector */
                                   false, /* no transpose */
                                   matrix2x2);
}

cg_attribute_t *
cg_attribute_new_const_3x3fv(cg_context_t *context,
                             const char *name,
                             const float *matrix3x3,
                             bool transpose)
{
    return _cg_attribute_new_const(context,
                                   name,
                                   3, /* n_components */
                                   3, /* 3 column vector */
                                   false, /* no transpose */
                                   matrix3x3);
}

cg_attribute_t *
cg_attribute_new_const_4x4fv(cg_context_t *context,
                             const char *name,
                             const float *matrix4x4,
                             bool transpose)
{
    return _cg_attribute_new_const(context,
                                   name,
                                   4, /* n_components */
                                   4, /* 4 column vector */
                                   false, /* no transpose */
                                   matrix4x4);
}

bool
cg_attribute_get_normalized(cg_attribute_t *attribute)
{
    _CG_RETURN_VAL_IF_FAIL(cg_is_attribute(attribute), false);

    return attribute->normalized;
}

static void
warn_about_midscene_changes(void)
{
    static bool seen = false;
    if (!seen) {
        c_warning("Mid-scene modification of attributes has "
                  "undefined results\n");
        seen = true;
    }
}

void
cg_attribute_set_normalized(cg_attribute_t *attribute, bool normalized)
{
    _CG_RETURN_IF_FAIL(cg_is_attribute(attribute));

    if (C_UNLIKELY(attribute->immutable_ref))
        warn_about_midscene_changes();

    attribute->normalized = normalized;
}

cg_attribute_buffer_t *
cg_attribute_get_buffer(cg_attribute_t *attribute)
{
    _CG_RETURN_VAL_IF_FAIL(cg_is_attribute(attribute), NULL);
    _CG_RETURN_VAL_IF_FAIL(attribute->is_buffered, NULL);

    return attribute->d.buffered.attribute_buffer;
}

void
cg_attribute_set_buffer(cg_attribute_t *attribute,
                        cg_attribute_buffer_t *attribute_buffer)
{
    _CG_RETURN_IF_FAIL(cg_is_attribute(attribute));
    _CG_RETURN_IF_FAIL(attribute->is_buffered);

    if (C_UNLIKELY(attribute->immutable_ref))
        warn_about_midscene_changes();

    cg_object_ref(attribute_buffer);

    cg_object_unref(attribute->d.buffered.attribute_buffer);
    attribute->d.buffered.attribute_buffer = attribute_buffer;
}

cg_attribute_t *
_cg_attribute_immutable_ref(cg_attribute_t *attribute)
{
    cg_buffer_t *buffer = CG_BUFFER(attribute->d.buffered.attribute_buffer);

    _CG_RETURN_VAL_IF_FAIL(cg_is_attribute(attribute), NULL);

    attribute->immutable_ref++;
    _cg_buffer_immutable_ref(buffer);
    return attribute;
}

void
_cg_attribute_immutable_unref(cg_attribute_t *attribute)
{
    cg_buffer_t *buffer = CG_BUFFER(attribute->d.buffered.attribute_buffer);

    _CG_RETURN_IF_FAIL(cg_is_attribute(attribute));
    _CG_RETURN_IF_FAIL(attribute->immutable_ref > 0);

    attribute->immutable_ref--;
    _cg_buffer_immutable_unref(buffer);
}

static void
_cg_attribute_free(cg_attribute_t *attribute)
{
    if (attribute->is_buffered)
        cg_object_unref(attribute->d.buffered.attribute_buffer);
    else
        _cg_boxed_value_destroy(&attribute->d.constant.boxed);

    c_slice_free(cg_attribute_t, attribute);
}

static bool
validate_layer_cb(cg_pipeline_t *pipeline, int layer_index, void *user_data)
{
    cg_texture_t *texture =
        cg_pipeline_get_layer_texture(pipeline, layer_index);
    cg_flush_layer_state_t *state = user_data;
    bool status = true;

    /* invalid textures will be handled correctly in
     * _cg_pipeline_flush_layers_gl_state */
    if (texture == NULL)
        goto validated;

    _cg_texture_flush_journal_rendering(texture);

    /* Give the texture a chance to know that we're rendering
       non-quad shaped primitives. If the texture is in an atlas it
       will be migrated */
    _cg_texture_ensure_non_quad_rendering(texture);

    /* We need to ensure the mipmaps are ready before deciding
     * anything else about the texture because the texture storate
     * could completely change if it needs to be migrated out of the
     * atlas and will affect how we validate the layer.
     */
    _cg_pipeline_pre_paint_for_layer(pipeline, layer_index);

    if (!_cg_texture_can_hardware_repeat(texture)) {
        c_warning("Disabling layer %d of the current source material, "
                  "because texturing with the vertex buffer API is not "
                  "currently supported using sliced textures, or textures "
                  "with waste\n",
                  layer_index);

        /* XXX: maybe we can add a mechanism for users to forcibly use
         * textures with waste where it would be their responsability to use
         * texture coords in the range [0,1] such that sampling outside isn't
         * required. We can then use a texture matrix (or a modification of
         * the users own matrix) to map 1 to the edge of the texture data.
         *
         * Potentially, given the same guarantee as above we could also
         * support a single sliced layer too. We would have to redraw the
         * vertices once for each layer, each time with a fiddled texture
         * matrix.
         */
        state->fallback_layers |= (1 << state->unit);
        state->options.flags |= CG_PIPELINE_FLUSH_FALLBACK_MASK;
    }

validated:
    state->unit++;
    return status;
}

void
_cg_flush_attributes_state(cg_framebuffer_t *framebuffer,
                           cg_pipeline_t *pipeline,
                           cg_draw_flags_t flags,
                           cg_attribute_t **attributes,
                           int n_attributes)
{
    cg_context_t *ctx = framebuffer->context;
    cg_flush_layer_state_t layers_state;

    if (!(flags & CG_DRAW_SKIP_JOURNAL_FLUSH))
        _cg_journal_flush(framebuffer->journal);

    layers_state.unit = 0;
    layers_state.options.flags = 0;
    layers_state.fallback_layers = 0;

    if (!(flags & CG_DRAW_SKIP_PIPELINE_VALIDATION))
        cg_pipeline_foreach_layer(pipeline, validate_layer_cb, &layers_state);

    /* NB: _cg_framebuffer_flush_state may disrupt various state (such
     * as the pipeline state) when flushing the clip stack, so should
     * always be done first when preparing to draw. We need to do this
     * before setting up the array pointers because setting up the clip
     * stack can cause some drawing which would change the array
     * pointers. */
    if (!(flags & CG_DRAW_SKIP_FRAMEBUFFER_FLUSH))
        _cg_framebuffer_flush_state(
            framebuffer, framebuffer, CG_FRAMEBUFFER_STATE_ALL);

    /* In cg_read_pixels we have a fast-path when reading a single
     * pixel and the scene is just comprised of simple rectangles still
     * in the journal. For this optimization to work we need to track
     * when the framebuffer really does get drawn to. */
    _cg_framebuffer_mark_mid_scene(framebuffer);
    _cg_framebuffer_mark_clear_clip_dirty(framebuffer);

    ctx->driver_vtable->flush_attributes_state(
        framebuffer, pipeline, &layers_state, flags, attributes, n_attributes);
}

int
_cg_attribute_get_n_components(cg_attribute_t *attribute)
{
    if (attribute->is_buffered)
        return attribute->d.buffered.n_components;
    else
        return attribute->d.constant.boxed.size;
}
