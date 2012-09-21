#ifndef _RUT_INTERFACES_H_
#define _RUT_INTERFACES_H_

#include "rut-types.h"
#include "rut-property.h"

/* A Collection of really simple, common interfaces that don't seem to
 * warrent being split out into separate files.
 */


/*
 *
 * Refcountable Interface
 *
 */

typedef struct _RutRefCountableVTable
{
  void *(*ref)(void *object);
  void (*unref)(void *object);
  void (*free)(void *object);
} RutRefCountableVTable;

void *
rut_refable_simple_ref (void *object);

void
rut_refable_simple_unref (void *object);

void *
rut_refable_ref (void *object);

void
rut_refable_unref (void *object);

/*
 *
 * Graphable Interface
 *
 */

typedef struct _RutGraphableVTable
{
  void (*child_removed) (RutObject *parent, RutObject *child);
  void (*child_added) (RutObject *parent, RutObject *child);

  void (*parent_changed) (RutObject *child,
                          RutObject *old_parent,
                          RutObject *new_parent);
} RutGraphableVTable;

typedef struct _RutGraphableProps
{
  RutObject *parent;
  GQueue children;
} RutGraphableProps;

#if 0
RutCamera *
rut_graphable_find_camera (RutObject *object);
#endif

/* RutTraverseFlags:
 * RUT_TRAVERSE_DEPTH_FIRST: Traverse the graph in
 *   a depth first order.
 * RUT_TRAVERSE_BREADTH_FIRST: Traverse the graph in a
 *   breadth first order.
 *
 * Controls some options for how rut_graphable_traverse() iterates
 * through a graph.
 */
typedef enum {
  RUT_TRAVERSE_DEPTH_FIRST   = 1L<<0,
  RUT_TRAVERSE_BREADTH_FIRST = 1L<<1
} RutTraverseFlags;

/* RutTraverseVisitFlags:
 * RUT_TRAVERSE_VISIT_CONTINUE: Continue traversing as
 *   normal
 * RUT_TRAVERSE_VISIT_SKIP_CHILDREN: Don't traverse the
 *   children of the last visited object. (Not applicable when using
 *   %RUT_TRAVERSE_DEPTH_FIRST_POST_ORDER since the children
 *   are visited before having an opportunity to bail out)
 * RUT_TRAVERSE_VISIT_BREAK: Immediately bail out without
 *   visiting any more objects.
 *
 * Each time an object is visited during a graph traversal the
 * RutTraverseCallback can return a set of flags that may affect the
 * continuing traversal. It may stop traversal completely, just skip
 * over children for the current object or continue as normal.
 */
typedef enum {
  RUT_TRAVERSE_VISIT_CONTINUE       = 1L<<0,
  RUT_TRAVERSE_VISIT_SKIP_CHILDREN  = 1L<<1,
  RUT_TRAVERSE_VISIT_BREAK          = 1L<<2
} RutTraverseVisitFlags;

/* The callback prototype used with rut_graphable_traverse. The
 * returned flags can be used to affect the continuing traversal
 * either by continuing as normal, skipping over children of an
 * object or bailing out completely.
 */
typedef RutTraverseVisitFlags (*RutTraverseCallback) (RutObject *object,
                                                      int depth,
                                                      void *user_data);

RutTraverseVisitFlags
rut_graphable_traverse (RutObject *root,
                        RutTraverseFlags flags,
                        RutTraverseCallback before_children_callback,
                        RutTraverseCallback after_children_callback,
                        void *user_data);

void
rut_graphable_init (RutObject *object);

void
rut_graphable_add_child (RutObject *parent, RutObject *child);

void
rut_graphable_remove_child (RutObject *child);

void
rut_graphable_remove_all_children (RutObject *parent);

RutObject *
rut_graphable_get_parent (RutObject *child);

void
rut_graphable_apply_transform (RutObject *graphable,
                               CoglMatrix *transform);

void
rut_graphable_get_transform (RutObject *graphable,
                             CoglMatrix *transform);

void
rut_graphable_get_modelview (RutObject *graphable,
                             RutCamera *camera,
                             CoglMatrix *transform);

void
rut_graphable_fully_transform_point (RutObject *graphable,
                                     RutCamera *camera,
                                     float *x,
                                     float *y,
                                     float *z);

/*
 *
 * Introspectable Interface
 *
 */

typedef void (*RutIntrospectablePropertyCallback) (RutProperty *property,
                                                   void *user_data);

typedef struct _RutIntrospectableVTable
{
  RutProperty *(*lookup_property) (RutObject *object, const char *name);
  void (*foreach_property) (RutObject *object,
                            RutIntrospectablePropertyCallback callback,
                            void *user_data);
} RutIntrospectableVTable;

RutProperty *
rut_introspectable_lookup_property (RutObject *object,
                                    const char *name);

void
rut_introspectable_foreach_property (RutObject *object,
                                     RutIntrospectablePropertyCallback callback,
                                     void *user_data);

typedef struct _RutSimpleIntrospectableProps
{
  RutProperty *first_property;
  int n_properties;
} RutSimpleIntrospectableProps;

void
rut_simple_introspectable_init (RutObject *object,
                                RutPropertySpec *specs,
                                RutProperty *properties);

void
rut_simple_introspectable_destroy (RutObject *object);

RutProperty *
rut_simple_introspectable_lookup_property (RutObject *object,
                                           const char *name);

void
rut_simple_introspectable_foreach_property (RutObject *object,
                                            RutIntrospectablePropertyCallback callback,
                                            void *user_data);

typedef struct RutTransformableVTable
{
  const CoglMatrix *(*get_matrix) (RutObject *object);
} RutTransformableVTable;

const CoglMatrix *
rut_transformable_get_matrix (RutObject *object);

typedef struct _RutSizableVTable
{
  void (* set_size) (void *object,
                     float width,
                     float height);
  void (* get_size) (void *object,
                     float *width,
                     float *height);
  void (* get_preferred_width) (void *object,
                                float for_height,
                                float *min_width_p,
                                float *natural_width_p);
  void (* get_preferred_height) (void *object,
                                 float for_width,
                                 float *min_height_p,
                                 float *natural_height_p);
} RutSizableVTable;

void
rut_sizable_set_size (RutObject *object,
                      float width,
                      float height);

void
rut_sizable_get_size (void *object,
                      float *width,
                      float *height);

void
rut_sizable_get_preferred_width (void *object,
                                 float for_height,
                                 float *min_width_p,
                                 float *natural_width_p);
void
rut_sizable_get_preferred_height (void *object,
                                  float for_width,
                                  float *min_height_p,
                                  float *natural_height_p);

/*
 *
 * Primable Interface
 * (E.g. implemented by all geometry components)
 *
 */
typedef struct _RutPrimableVTable
{
  CoglPrimitive *(*get_primitive)(void *object);
} RutPrimableVTable;

CoglPrimitive *
rut_primable_get_primitive (RutObject *object);


/*
 *
 * Pickable Interface
 * (E.g. implemented by all geometry components)
 *
 */
typedef struct _RutPickableVTable
{
  void *(*get_vertex_data)(void *object,
                           size_t *stride,
                           int *n_vertices);
} RutPickableVTable;

void *
rut_pickable_get_vertex_data (RutObject *object,
                              size_t *stride,
                              int *n_vertices);

#endif /* _RUT_INTERFACES_H_ */
