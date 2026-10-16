/*
 * i3 - sparse surface drawing helpers
 */

#include "sparse_surface.h"

void sparse_surface_copy(sparse_surface_t *source, surface_t *destination,
                         double source_x, double source_y,
                         double destination_x, double destination_y,
                         double width, double height) {
    if (source == NULL || destination == NULL || destination->id == XCB_NONE ||
        destination->cr == NULL || width <= 0 || height <= 0) {
        return;
    }

    for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
        surface_t *surface = &(source->side_surface[side]);
        if (surface->id == XCB_NONE || surface->surface == NULL) {
            continue;
        }

        const double adjusted_source_x = source_x - source->x_offset[side];
        const double adjusted_source_y = source_y - source->y_offset[side];
        const double side_destination_x = destination_x - adjusted_source_x;
        const double side_destination_y = destination_y - adjusted_source_y;

        cairo_save(destination->cr);
        cairo_set_operator(destination->cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(destination->cr, surface->surface,
                                 side_destination_x, side_destination_y);

        /* A sparse side can be larger than the requested copy region. */
        cairo_rectangle(destination->cr, destination_x, destination_y, width, height);
        cairo_clip(destination->cr);
        cairo_rectangle(destination->cr, side_destination_x, side_destination_y,
                        surface->width, surface->height);
        cairo_fill(destination->cr);
        CAIRO_SURFACE_FLUSH(surface->surface);
        CAIRO_SURFACE_FLUSH(destination->surface);
        cairo_restore(destination->cr);
    }
}
