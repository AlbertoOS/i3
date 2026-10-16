/*
 * i3 - sparse surface regression tests
 *
 * Sparse edges are local pixmaps, while decoration geometry is frame-relative.
 * Text must be dispatched in the local coordinate system of each edge.
 */

#include <stdio.h>
#include <stdlib.h>

#include "sparse_surface.h"

typedef struct {
    surface_t *surface;
    int x;
    int y;
    int max_width;
} text_call_t;

static text_call_t calls[SPARSE_SURFACE_SIDE_COUNT];
static int call_count;

void draw_util_text(i3String *text, surface_t *surface, color_t fg_color,
                    color_t bg_color, int x, int y, int max_width) {
    (void)text;
    (void)fg_color;
    (void)bg_color;

    calls[call_count++] = (text_call_t){
        .surface = surface,
        .x = x,
        .y = y,
        .max_width = max_width,
    };
}

static void expect_call(int index, surface_t *surface, int x, int y,
                        int max_width) {
    const text_call_t actual = calls[index];
    if (actual.surface != surface || actual.x != x || actual.y != y ||
        actual.max_width != max_width) {
        fprintf(stderr,
                "call %d: expected surface=%p x=%d y=%d width=%d, got "
                "surface=%p x=%d y=%d width=%d\n",
                index, (void *)surface, x, y, max_width,
                (void *)actual.surface, actual.x, actual.y, actual.max_width);
        exit(EXIT_FAILURE);
    }
}

int main(void) {
    sparse_surface_t surface = {0};
    surface.side_surface[SPARSE_SURFACE_TOP].id = 1;
    surface.x_offset[SPARSE_SURFACE_TOP] = 5;
    surface.y_offset[SPARSE_SURFACE_TOP] = 7;
    surface.side_surface[SPARSE_SURFACE_RIGHT].id = 2;
    surface.x_offset[SPARSE_SURFACE_RIGHT] = 13;
    surface.y_offset[SPARSE_SURFACE_RIGHT] = 7;

    sparse_surface_text(NULL, &surface, COLOR_TRANSPARENT, COLOR_TRANSPARENT,
                        11, 10, 80);

    if (call_count != 2) {
        fprintf(stderr, "expected 2 text calls, got %d\n", call_count);
        return EXIT_FAILURE;
    }

    expect_call(0, &(surface.side_surface[SPARSE_SURFACE_TOP]), 6, 3, 80);
    expect_call(1, &(surface.side_surface[SPARSE_SURFACE_RIGHT]), -2, 3, 80);
    return EXIT_SUCCESS;
}
