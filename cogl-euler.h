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
 * Authors:
 *   Robert Bragg <robert@linux.intel.com>
 */

#if !defined(__CG_H_INSIDE__) && !defined(CG_COMPILATION)
#error "Only <cogl/cogl.h> can be included directly."
#endif

#ifndef __CG_EULER_H
#define __CG_EULER_H

#include <cogl/cogl-types.h>

CG_BEGIN_DECLS

/**
 * SECTION:cogl-euler
 * @short_description: Functions for initializing and manipulating
 * euler angles.
 *
 * Euler angles are a simple representation of a 3 dimensional
 * rotation; comprised of 3 ordered heading, pitch and roll rotations.
 * An important thing to understand is that the axis of rotation
 * belong to the object being rotated and so they also rotate as each
 * of the heading, pitch and roll rotations are applied.
 *
 * One way to consider euler angles is to imagine controlling an
 * aeroplane, where you first choose a heading (Such as flying south
 * east), then you set the pitch (such as 30 degrees to take off) and
 * then you might set a roll, by dipping the left, wing as you prepare
 * to turn.
 *
 * They have some advantages and limitations that it helps to be
 * aware of:
 *
 * Advantages:
 * <itemizedlist>
 * <listitem>
 * Easy to understand and use, compared to quaternions and matrices,
 * so may be a good choice for a user interface.
 * </listitem>
 * <listitem>
 * Efficient storage, needing only 3 components any rotation can be
 * represented.
 * <note>Actually the #cg_euler_t type isn't optimized for size because
 * we may cache the equivalent #cg_quaternion_t along with a euler
 * rotation, but it would be trivial for an application to track the
 * components of euler rotations in a packed float array if optimizing
 * for size was important. The values could be passed to Cogl only when
 * manipulation is necessary.</note>
 * </listitem>
 * </itemizedlist>
 *
 * Disadvantages:
 * <itemizedlist>
 * <listitem>
 * Aliasing: it's possible to represent some rotations with multiple
 * different heading, pitch and roll rotations.
 * </listitem>
 * <listitem>
 * They can suffer from a problem called Gimbal Lock. A good
 * explanation of this can be seen on wikipedia here:
 * http://en.wikipedia.org/wiki/Gimbal_lock but basically two
 * of the axis of rotation may become aligned and so you loose a
 * degree of freedom. For example a pitch of +-90° would mean that
 * heading and bank rotate around the same axis.
 * </listitem>
 * <listitem>
 * If you use euler angles to orient something in 3D space and try to
 * transition between orientations by interpolating the component
 * angles you probably wont get the transitions you expect as they may
 * not follow the shortest path between the two orientations.
 * </listitem>
 * <listitem>
 * There's no standard to what order the component axis rotations are
 * applied. The most common convention seems to be what we do in Cogl
 * with heading (y-axis), pitch (x-axis) and then roll (z-axis), but
 * other software might apply x-axis, y-axis then z-axis or any other
 * order so you need to consider this if you are accepting euler
 * rotations from some other software. Other software may also use
 * slightly different aeronautical terms, such as "yaw" instead of
 * "heading" or "bank" instead of "roll".
 * </listitem>
 * </itemizedlist>
 *
 * To minimize the aliasing issue we may refer to "Canonical Euler"
 * angles where heading and roll are restricted to +- 180° and pitch is
 * restricted to +- 90°. If pitch is +- 90° bank is set to 0°.
 *
 * Quaternions don't suffer from Gimbal Lock and they can be nicely
 * interpolated between, their disadvantage is that they don't have an
 * intuitive representation.
 *
 * A common practice is to accept angles in the intuitive Euler form
 * and convert them to quaternions internally to avoid Gimbal Lock and
 * handle interpolations. See cg_quaternion_init_from_euler().
 */

/**
 * cg_euler_t:
 * @heading: Angle to rotate around an object's y axis
 * @pitch: Angle to rotate around an object's x axis
 * @roll: Angle to rotate around an object's z axis
 *
 * Represents an ordered rotation first of @heading degrees around an
 * object's y axis, then @pitch degrees around an object's x axis and
 * finally @roll degrees around an object's z axis.
 *
 * <note>It's important to understand the that axis are associated
 * with the object being rotated, so the axis also rotate in sequence
 * with the rotations being applied.</note>
 *
 * The members of a #cg_euler_t can be initialized, for example, with
 * cg_euler_init() and cg_euler_init_from_quaternion ().
 *
 * You may also want to look at cg_quaternion_init_from_euler() if
 * you want to do interpolation between 3d rotations.
 *
 * Since: 2.0
 */
struct _cg_euler_t {
    /*< public > */
    float heading;
    float pitch;
    float roll;
};
CG_STRUCT_SIZE_ASSERT(cg_euler_t, 12);

/**
 * cg_euler_init:
 * @euler: The #cg_euler_t angle to initialize
 * @heading: Angle to rotate around an object's y axis
 * @pitch: Angle to rotate around an object's x axis
 * @roll: Angle to rotate around an object's z axis
 *
 * Initializes @euler to represent a rotation of @x_angle degrees
 * around the x axis, then @y_angle degrees around the y_axis and
 * @z_angle degrees around the z axis.
 *
 * Since: 2.0
 */
void cg_euler_init(cg_euler_t *euler, float heading, float pitch, float roll);

/**
 * cg_euler_init_from_matrix:
 * @euler: The #cg_euler_t angle to initialize
 * @matrix: A #cg_matrix_t containing a rotation, but no scaling,
 *          mirroring or skewing.
 *
 * Extracts a euler rotation from the given @matrix and
 * initializses @euler with the component x, y and z rotation angles.
 */
void cg_euler_init_from_matrix(cg_euler_t *euler, const cg_matrix_t *matrix);

/**
 * cg_euler_init_from_quaternion:
 * @euler: The #cg_euler_t angle to initialize
 * @quaternion: A #cg_euler_t with the rotation to initialize with
 *
 * Initializes a @euler rotation with the equivalent rotation
 * represented by the given @quaternion.
 */
void cg_euler_init_from_quaternion(cg_euler_t *euler,
                                   const cg_quaternion_t *quaternion);

/**
 * cg_euler_equal:
 * @v1: The first euler angle to compare
 * @v2: The second euler angle to compare
 *
 * Compares the two given euler angles @v1 and @v1 and it they are
 * equal returns %true else %false.
 *
 * <note>This function only checks that all three components rotations
 * are numerically equal, it does not consider that some rotations
 * can be represented with different component rotations</note>
 *
 * Returns: %true if @v1 and @v2 are equal else %false.
 * Since: 2.0
 */
bool cg_euler_equal(const void *v1, const void *v2);

/**
 * cg_euler_copy:
 * @src: A #cg_euler_t to copy
 *
 * Allocates a new #cg_euler_t and initilizes it with the component
 * angles of @src. The newly allocated euler should be freed using
 * cg_euler_free().
 *
 * Returns: A newly allocated #cg_euler_t
 * Since: 2.0
 */
cg_euler_t *cg_euler_copy(const cg_euler_t *src);

/**
 * cg_euler_free:
 * @euler: A #cg_euler_t allocated via cg_euler_copy()
 *
 * Frees a #cg_euler_t that was previously allocated using
 * cg_euler_copy().
 *
 * Since: 2.0
 */
void cg_euler_free(cg_euler_t *euler);

CG_END_DECLS

#endif /* __CG_EULER_H */