/*
 * i3 - sparse surface regression tests
 *
 * Transient layout geometry must never produce invalid X pixmap dimensions.
 */

#include <stdio.h>
#include <stdlib.h>

#include "sparse_surface.h"

/* This internal helper will normalize aggregate and edge geometry. */
void sparse_surface_set_geometry(sparse_surface_t *surface, int width, int height,
                                 int top_height, int bottom_height,
                                 int left_width, int right_width);

static void expect_equal(const char *field, int actual, int expected) {
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", field, expected, actual);
        exit(EXIT_FAILURE);
    }
}

int main(void) {
    sparse_surface_t surface = {0};

    sparse_surface_set_geometry(&surface, 0, 0, 3, -4, 7, 8);

    expect_equal("frame width", surface.width, 1);
    expect_equal("frame height", surface.height, 1);
    expect_equal("top x", surface.x_offset[SPARSE_SURFACE_TOP], 1);
    expect_equal("top y", surface.y_offset[SPARSE_SURFACE_TOP], 0);
    expect_equal("top width", surface.side_surface[SPARSE_SURFACE_TOP].width, 0);
    expect_equal("top height", surface.side_surface[SPARSE_SURFACE_TOP].height, 1);
    expect_equal("bottom y", surface.y_offset[SPARSE_SURFACE_BOTTOM], 1);
    expect_equal("bottom height", surface.side_surface[SPARSE_SURFACE_BOTTOM].height, 0);
    expect_equal("left width", surface.side_surface[SPARSE_SURFACE_LEFT].width, 1);
    expect_equal("left height", surface.side_surface[SPARSE_SURFACE_LEFT].height, 1);
    expect_equal("right x", surface.x_offset[SPARSE_SURFACE_RIGHT], 1);
    expect_equal("right width", surface.side_surface[SPARSE_SURFACE_RIGHT].width, 0);

    sparse_surface_set_geometry(&surface, 100, 80, 20, 2, 1, 3);
    expect_equal("normal top x", surface.x_offset[SPARSE_SURFACE_TOP], 1);
    expect_equal("normal top width", surface.side_surface[SPARSE_SURFACE_TOP].width, 96);
    expect_equal("normal top height", surface.side_surface[SPARSE_SURFACE_TOP].height, 20);
    expect_equal("normal bottom y", surface.y_offset[SPARSE_SURFACE_BOTTOM], 78);
    expect_equal("normal bottom height", surface.side_surface[SPARSE_SURFACE_BOTTOM].height, 2);
    expect_equal("normal left width", surface.side_surface[SPARSE_SURFACE_LEFT].width, 1);
    expect_equal("normal right x", surface.x_offset[SPARSE_SURFACE_RIGHT], 97);
    expect_equal("normal right width", surface.side_surface[SPARSE_SURFACE_RIGHT].width, 3);

    sparse_surface_set_geometry(&surface, 100, 80, 0, 0, 0, 0);
    expect_equal("stacked top x", surface.x_offset[SPARSE_SURFACE_TOP], 0);
    expect_equal("stacked top y", surface.y_offset[SPARSE_SURFACE_TOP], 0);
    expect_equal("stacked top width", surface.side_surface[SPARSE_SURFACE_TOP].width, 100);
    expect_equal("stacked top height", surface.side_surface[SPARSE_SURFACE_TOP].height, 80);
    expect_equal("stacked bottom width", surface.side_surface[SPARSE_SURFACE_BOTTOM].width, 0);
    expect_equal("stacked left width", surface.side_surface[SPARSE_SURFACE_LEFT].width, 0);
    expect_equal("stacked right width", surface.side_surface[SPARSE_SURFACE_RIGHT].width, 0);

    return EXIT_SUCCESS;
}
