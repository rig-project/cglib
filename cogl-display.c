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
 * Authors:
 *   Robert Bragg <robert@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "cogl-private.h"
#include "cogl-object.h"

#include "cogl-display-private.h"
#include "cogl-renderer-private.h"
#include "cogl-winsys-private.h"
#ifdef CG_HAS_WAYLAND_EGL_SERVER_SUPPORT
#include "cogl-wayland-server.h"
#endif

static void _cg_display_free(cg_display_t *display);

CG_OBJECT_DEFINE(Display, display);

static const cg_winsys_vtable_t *
_cg_display_get_winsys(cg_display_t *display)
{
    return display->renderer->winsys_vtable;
}

static void
_cg_display_free(cg_display_t *display)
{
    const cg_winsys_vtable_t *winsys;

    if (display->setup) {
        winsys = _cg_display_get_winsys(display);
        winsys->display_destroy(display);
        display->setup = false;
    }

    if (display->renderer) {
        cg_object_unref(display->renderer);
        display->renderer = NULL;
    }

    if (display->onscreen_template) {
        cg_object_unref(display->onscreen_template);
        display->onscreen_template = NULL;
    }

    c_slice_free(cg_display_t, display);
}

cg_display_t *
cg_display_new(cg_renderer_t *renderer,
               cg_onscreen_template_t *onscreen_template)
{
    cg_display_t *display = c_slice_new0(cg_display_t);
    cg_error_t *error = NULL;

    _cg_init();

    display->renderer = renderer;
    if (renderer)
        cg_object_ref(renderer);
    else
        display->renderer = cg_renderer_new();

    if (!cg_renderer_connect(display->renderer, &error))
        c_error("Failed to connect to renderer: %s\n", error->message);

    display->setup = false;

#ifdef CG_HAS_EGL_PLATFORM_GDL_SUPPORT
    display->gdl_plane = GDL_PLANE_ID_UPP_C;
#endif

    display = _cg_display_object_new(display);

    cg_display_set_onscreen_template(display, onscreen_template);

    return display;
}

cg_renderer_t *
cg_display_get_renderer(cg_display_t *display)
{
    return display->renderer;
}

void
cg_display_set_onscreen_template(cg_display_t *display,
                                 cg_onscreen_template_t *onscreen_template)
{
    _CG_RETURN_IF_FAIL(display->setup == false);

    if (onscreen_template)
        cg_object_ref(onscreen_template);

    if (display->onscreen_template)
        cg_object_unref(display->onscreen_template);

    display->onscreen_template = onscreen_template;

    /* NB: we want to maintain the invariable that there is always an
     * onscreen template associated with a cg_display_t... */
    if (!onscreen_template)
        display->onscreen_template = cg_onscreen_template_new();
}

bool
cg_display_setup(cg_display_t *display, cg_error_t **error)
{
    const cg_winsys_vtable_t *winsys;

    if (display->setup)
        return true;

    winsys = _cg_display_get_winsys(display);
    if (!winsys->display_setup(display, error))
        return false;

    display->setup = true;

    return true;
}

#ifdef CG_HAS_EGL_PLATFORM_GDL_SUPPORT
void
cg_gdl_display_set_plane(cg_display_t *display, gdl_plane_id_t plane)
{
    _CG_RETURN_IF_FAIL(display->setup == false);

    display->gdl_plane = plane;
}
#endif

#ifdef CG_HAS_WAYLAND_EGL_SERVER_SUPPORT
void
cg_wayland_display_set_compositor_display(cg_display_t *display,
                                          struct wl_display *wayland_display)
{
    _CG_RETURN_IF_FAIL(display->setup == false);

    display->wayland_compositor_display = wayland_display;
}
#endif