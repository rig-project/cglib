/*
 * Cogl
 *
 * A Low-Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2011 Intel Corporation.
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
 * Authors:
 *  Robert Bragg <robert@linux.intel.com>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl-renderer-private.h"
#include "cogl-display-private.h"
#include "cogl-context-private.h"
#include "cogl-framebuffer-private.h"
#include "cogl-private.h"
#include "cogl-winsys-stub-private.h"

#include <string.h>

static int _cg_winsys_stub_dummy_ptr;

/* This provides a NOP winsys. This can be useful for debugging or for
 * integrating with toolkits that already have window system
 * integration code.
 */

static cg_func_ptr_t
_cg_winsys_renderer_get_proc_address(
    cg_renderer_t *renderer, const char *name, bool in_core)
{
    static UModule *module = NULL;

    /* this should find the right function if the program is linked against a
     * library providing it */
    if (C_UNLIKELY(module == NULL))
        module = c_module_open(NULL, 0);

    if (module) {
        void *symbol;

        if (c_module_symbol(module, name, &symbol))
            return symbol;
    }

    return NULL;
}

static void
_cg_winsys_renderer_disconnect(cg_renderer_t *renderer)
{
    renderer->winsys = NULL;
}

static bool
_cg_winsys_renderer_connect(cg_renderer_t *renderer,
                            cg_error_t **error)
{
    renderer->winsys = &_cg_winsys_stub_dummy_ptr;
    return true;
}

static void
_cg_winsys_display_destroy(cg_display_t *display)
{
    display->winsys = NULL;
}

static bool
_cg_winsys_display_setup(cg_display_t *display, cg_error_t **error)
{
    display->winsys = &_cg_winsys_stub_dummy_ptr;
    return true;
}

static bool
_cg_winsys_context_init(cg_context_t *context, cg_error_t **error)
{
    context->winsys = &_cg_winsys_stub_dummy_ptr;

    if (!_cg_context_update_features(context, error))
        return false;

    memset(context->winsys_features, 0, sizeof(context->winsys_features));

    return true;
}

static void
_cg_winsys_context_deinit(cg_context_t *context)
{
    context->winsys = NULL;
}

static bool
_cg_winsys_onscreen_init(cg_onscreen_t *onscreen,
                         cg_error_t **error)
{
    return true;
}

static void
_cg_winsys_onscreen_deinit(cg_onscreen_t *onscreen)
{
}

static void
_cg_winsys_onscreen_bind(cg_onscreen_t *onscreen)
{
}

static void
_cg_winsys_onscreen_swap_buffers_with_damage(
    cg_onscreen_t *onscreen, const int *rectangles, int n_rectangles)
{
}

static void
_cg_winsys_onscreen_update_swap_throttled(cg_onscreen_t *onscreen)
{
}

static void
_cg_winsys_onscreen_set_visibility(cg_onscreen_t *onscreen,
                                   bool visibility)
{
}

const cg_winsys_vtable_t *
_cg_winsys_stub_get_vtable(void)
{
    static bool vtable_inited = false;
    static cg_winsys_vtable_t vtable;

    /* It would be nice if we could use C99 struct initializers here
       like the GLX backend does. However this code is more likely to be
       compiled using Visual Studio which (still!) doesn't support them
       so we initialize it in code instead */

    if (!vtable_inited) {
        memset(&vtable, 0, sizeof(vtable));

        vtable.id = CG_WINSYS_ID_STUB;
        vtable.name = "STUB";
        vtable.renderer_get_proc_address = _cg_winsys_renderer_get_proc_address;
        vtable.renderer_connect = _cg_winsys_renderer_connect;
        vtable.renderer_disconnect = _cg_winsys_renderer_disconnect;
        vtable.display_setup = _cg_winsys_display_setup;
        vtable.display_destroy = _cg_winsys_display_destroy;
        vtable.context_init = _cg_winsys_context_init;
        vtable.context_deinit = _cg_winsys_context_deinit;

        vtable.onscreen_init = _cg_winsys_onscreen_init;
        vtable.onscreen_deinit = _cg_winsys_onscreen_deinit;
        vtable.onscreen_bind = _cg_winsys_onscreen_bind;
        vtable.onscreen_swap_buffers_with_damage =
            _cg_winsys_onscreen_swap_buffers_with_damage;
        vtable.onscreen_update_swap_throttled =
            _cg_winsys_onscreen_update_swap_throttled;
        vtable.onscreen_set_visibility = _cg_winsys_onscreen_set_visibility;

        vtable_inited = true;
    }

    return &vtable;
}