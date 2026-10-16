/*
 * i3 - sparse surface regression tests
 *
 * Verify that sparse-surface copying honors the requested copy rectangle and
 * composes four offset edge surfaces without painting the frame interior.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cairo/cairo.h>

#include "sparse_surface.h"

#define RED 0xffff0000
#define GREEN 0xff00ff00
#define BLUE 0xff0000ff
#define YELLOW 0xffffff00
#define MAGENTA 0xffff00ff
#define TRANSPARENT 0x00000000

static void expect_pixel(cairo_surface_t *surface, int x, int y,
                         uint32_t expected, const char *description) {
    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface) /
                       (int)sizeof(uint32_t);
    const uint32_t actual =
        ((const uint32_t *)cairo_image_surface_get_data(surface))[y * stride + x];
    if (actual != expected) {
        fprintf(stderr, "%s: expected %#010x at (%d, %d), got %#010x\n",
                description, expected, x, y, actual);
        exit(EXIT_FAILURE);
    }
}

static void init_side(sparse_surface_t *source, sparse_surface_side side,
                      int x_offset, int y_offset, int width, int height,
                      uint32_t color) {
    surface_t *surface = &(source->side_surface[side]);
    surface->id = side + 1;
    surface->width = width;
    surface->height = height;
    source->x_offset[side] = x_offset;
    source->y_offset[side] = y_offset;
    surface->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    surface->cr = cairo_create(surface->surface);
    cairo_set_source_rgba(surface->cr, ((color >> 16) & 0xff) / 255.0,
                          ((color >> 8) & 0xff) / 255.0,
                          (color & 0xff) / 255.0,
                          ((color >> 24) & 0xff) / 255.0);
    cairo_paint(surface->cr);
}

static void clear_surface(surface_t *surface) {
    cairo_set_operator(surface->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(surface->cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(surface->cr);
}

static void free_surface(surface_t *surface) {
    cairo_destroy(surface->cr);
    cairo_surface_destroy(surface->surface);
}

/* Compare every destination pixel against the logical source frame rather
 * than sampling only edge centers. Integer coordinates keep this independent
 * of Cairo's antialiasing rules. Missing edges must preserve existing pixels. */
static void expect_copy(sparse_surface_t *source, surface_t *destination,
                        int source_x, int source_y, int destination_x, int destination_y,
                        int width, int height, bool clipped, const char *description) {
    cairo_set_operator(destination->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(destination->cr, 1.0, 0.0, 1.0);
    cairo_paint(destination->cr);
    cairo_save(destination->cr);
    if (clipped) {
        cairo_rectangle(destination->cr, 1, 2, 6, 5);
        cairo_clip(destination->cr);
    }
    /* The copy must restore the caller's operator, source, and clip. */
    cairo_set_operator(destination->cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgb(destination->cr, 0.25, 0.5, 0.75);
    sparse_surface_copy(source, destination, source_x, source_y,
                        destination_x, destination_y, width, height);
    if (cairo_get_operator(destination->cr) != CAIRO_OPERATOR_OVER) {
        fprintf(stderr, "%s: copy changed the caller's operator\n", description);
        exit(EXIT_FAILURE);
    }
    double red, green, blue, alpha;
    if (cairo_pattern_get_rgba(cairo_get_source(destination->cr),
                               &red, &green, &blue, &alpha) != CAIRO_STATUS_SUCCESS ||
        red != 0.25 || green != 0.5 || blue != 0.75 || alpha != 1.0) {
        fprintf(stderr, "%s: copy changed the caller's source\n", description);
        exit(EXIT_FAILURE);
    }
    double x1, y1, x2, y2;
    cairo_clip_extents(destination->cr, &x1, &y1, &x2, &y2);
    if (x1 != (clipped ? 1 : 0) || y1 != (clipped ? 2 : 0) ||
        x2 != (clipped ? 7 : destination->width) ||
        y2 != (clipped ? 7 : destination->height)) {
        fprintf(stderr, "%s: copy changed the caller's clip\n", description);
        exit(EXIT_FAILURE);
    }
    cairo_restore(destination->cr);

    const uint32_t colors[] = {RED, GREEN, BLUE, YELLOW};
    for (int y = 0; y < destination->height; y++) {
        for (int x = 0; x < destination->width; x++) {
            uint32_t expected = MAGENTA;
            if (x >= destination_x && x < destination_x + width &&
                y >= destination_y && y < destination_y + height &&
                (!clipped || (x >= 1 && x < 7 && y >= 2 && y < 7))) {
                const int frame_x = source_x + x - destination_x;
                const int frame_y = source_y + y - destination_y;
                for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
                    const surface_t *edge = &source->side_surface[side];
                    if (edge->id != XCB_NONE && edge->surface != NULL &&
                        frame_x >= source->x_offset[side] &&
                        frame_x < source->x_offset[side] + edge->width &&
                        frame_y >= source->y_offset[side] &&
                        frame_y < source->y_offset[side] + edge->height) {
                        expected = colors[side];
                    }
                }
            }
            expect_pixel(destination->surface, x, y, expected, description);
        }
    }
    if (cairo_status(destination->cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "%s: destination Cairo context failed\n", description);
        exit(EXIT_FAILURE);
    }
}

int main(void) {
    sparse_surface_t source = {0};
    source.width = 10;
    source.height = 10;
    init_side(&source, SPARSE_SURFACE_TOP, 2, 0, 6, 2, RED);
    init_side(&source, SPARSE_SURFACE_BOTTOM, 2, 8, 6, 2, GREEN);
    init_side(&source, SPARSE_SURFACE_LEFT, 0, 0, 2, 10, BLUE);
    init_side(&source, SPARSE_SURFACE_RIGHT, 8, 0, 2, 10, YELLOW);

    surface_t destination = {0};
    destination.id = 1;
    destination.width = 10;
    destination.height = 10;
    destination.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 10, 10);
    destination.cr = cairo_create(destination.surface);
    clear_surface(&destination);

    sparse_surface_copy(&source, &destination, 0, 0, 0, 0, 10, 10);
    expect_pixel(destination.surface, 0, 0, BLUE, "left edge owns top-left corner");
    expect_pixel(destination.surface, 3, 1, RED, "top edge is positioned by its offset");
    expect_pixel(destination.surface, 3, 8, GREEN, "bottom edge is positioned by its offset");
    expect_pixel(destination.surface, 9, 1, YELLOW, "right edge owns top-right corner");
    expect_pixel(destination.surface, 5, 5, TRANSPARENT, "frame interior stays untouched");

    clear_surface(&destination);
    sparse_surface_copy(&source, &destination, 2, 0, 2, 3, 3, 4);
    expect_pixel(destination.surface, 2, 3, RED, "partial copy origin is painted");
    expect_pixel(destination.surface, 4, 4, RED, "partial copy extent is painted");
    expect_pixel(destination.surface, 2, 5, TRANSPARENT, "partial copy preserves transparent interior");
    expect_pixel(destination.surface, 5, 3, TRANSPARENT, "partial copy does not overpaint to the right");

    expect_copy(&source, &destination, 1, 2, 4, 1, 7, 8, false,
                "unequal origins with right destination clipping");
    expect_copy(&source, &destination, 5, 7, 1, 4, 5, 5, false,
                "unequal origins with source outside bottom edge");
    expect_copy(&source, &destination, 0, 0, -3, -2, 10, 10, false,
                "negative destination clips left and top");
    expect_copy(&source, &destination, 0, 0, 7, 8, 10, 10, false,
                "destination clips right and bottom");
    expect_copy(&source, &destination, -2, -1, 1, 2, 12, 11, false,
                "source rectangle starts outside the frame");
    expect_copy(&source, &destination, 0, 0, 0, 0, 10, 10, true,
                "existing destination clip is respected");
    expect_copy(&source, &destination, 2, 1, -1, 3, 10, 10, true,
                "existing clip with unequal and negative origins");
    expect_copy(&source, &destination, 0, 0, 0, 0, 0, 10, false,
                "zero width does not paint");
    expect_copy(&source, &destination, 0, 0, 0, 0, 10, -1, false,
                "negative height does not paint");

    for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
        surface_t *edge = &source.side_surface[side];
        const xcb_drawable_t id = edge->id;
        edge->id = XCB_NONE;
        expect_copy(&source, &destination, 0, 0, 0, 0, 10, 10, false,
                    "missing edge preserves destination pixels");
        expect_copy(&source, &destination, 1, 2, 3, 1, 9, 8, true,
                    "missing edge with unequal origins and clipping");
        edge->id = id;

        cairo_surface_t *image = edge->surface;
        edge->surface = NULL;
        expect_copy(&source, &destination, 0, 0, 0, 0, 10, 10, false,
                    "missing Cairo surface preserves destination pixels");
        edge->surface = image;
    }

    free_surface(&destination);
    for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
        free_surface(&(source.side_surface[side]));
    }
    puts("sparse surface copy: origins, missing edges, clipping, and Cairo state passed");
    return EXIT_SUCCESS;
}
