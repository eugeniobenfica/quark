/* A library's header, as libraries write them: an include guard, macros that decorate each declaration, a second
   header beside it, typedefs, an enum, structs C lays out and handles, callbacks, and what Quark cannot say. */
#ifndef SHAPES_H
#define SHAPES_H

#include <stddef.h>
#include <stdint.h>
#include "shapes_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHAPES_VERSION "2.1"
#define SHAPES_MAX_POINTS (1 << SHAPES_BITS)
#define SHAPES_SCALE 2.5f
#define SHAPES_MASK 0xFFu

typedef struct shapes_point { double x, y; } shapes_point;

typedef struct shapes_canvas shapes_canvas; /* only named: a handle */

enum shapes_kind { SHAPES_CIRCLE, SHAPES_SQUARE = 4, SHAPES_BOTH = SHAPES_CIRCLE | SHAPES_SQUARE };

typedef struct {
    uint8_t rgba[4];
    const char *name;
    shapes_point at;
    enum shapes_kind kind;
    long weight;
} shapes_style;

typedef int (SHAPES_CALL *shapes_compare)(const void *a, const void *b);

typedef struct shapes_ops {
    shapes_compare compare;
    void (*draw)(shapes_canvas *canvas, int layer);
} shapes_ops;

union shapes_value { int i; float f; };

struct shapes_flags { unsigned visible : 1; unsigned locked : 1; };

SHAPES_API(shapes_canvas *) shapes_open(const char *title, size_t width, size_t height);
SHAPES_API(void) shapes_close(shapes_canvas *canvas);
SHAPES_API(int) shapes_draw(shapes_canvas *canvas, const shapes_style *style, const shapes_point *points,
                            size_t count);
SHAPES_API(shapes_point) shapes_middle(shapes_point a, shapes_point b);
SHAPES_API(void) shapes_sort(shapes_point *points, size_t count, shapes_compare compare);
SHAPES_API(unsigned long) shapes_read(shapes_canvas *canvas, unsigned char *out, unsigned long room);
SHAPES_API(int) shapes_log(const char *format, ...);
SHAPES_API(void) shapes_set(shapes_canvas *canvas, union shapes_value value);
SHAPES_API(int) shapes_print(const char *format, va_list args);
SHAPES_API(const char *) shapes_name(enum shapes_kind kind);
SHAPES_API(shapes_compare) shapes_default_compare(void);
SHAPES_API(const shapes_ops *) shapes_ops_of(shapes_canvas *canvas);

extern int shapes_count;

static inline double shapes_twice(double x) { return 2 * x; }

#ifdef __cplusplus
}
#endif

#endif
