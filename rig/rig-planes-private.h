/*
 * Rig
 *
 * A rig of UI prototyping utilities
 *
 * Copyright (C) 2011  Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __RIG_PLANES_PRIVATE_H__
#define __RIG_PLANES_PRIVATE_H__

#include <cogl/cogl2-experimental.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct _RigPlane
{
  CoglVector3 v0;
  CoglVector3 n;
} RigPlane;

G_END_DECLS

#endif /* __RIG_PLANES_PRIVATE_H__ */
