/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * sparse_surface.c: A 'sparse surface' represents the pixmap borders around
 * a window. We allocate four separate pixmaps, one for each side of the
 * window, which means we don't allocate any memory for the contents of
 * the window, which we don't draw to.
 */

#include "sparse_surface.h"
#include "all.h"

/* The default visual_type to use if none is specified when creating the surface. Must be defined globally. */
extern xcb_visualtype_t *visual_type;

static void sparse_surface_free_one_side(xcb_connection_t *conn, sparse_surface_t *sparse_surface, sparse_surface_side side);

bool sparse_surface_initialized(sparse_surface_t *sparse_surface) {
    return sparse_surface->side_surface[SPARSE_SURFACE_TOP].id != XCB_NONE ||
           sparse_surface->side_surface[SPARSE_SURFACE_BOTTOM].id != XCB_NONE ||
           sparse_surface->side_surface[SPARSE_SURFACE_LEFT].id != XCB_NONE ||
           sparse_surface->side_surface[SPARSE_SURFACE_RIGHT].id != XCB_NONE;
}

/*
 * Destroys the surface.
 *
 */
static void surface_free(xcb_connection_t *conn,
                         cairo_t **cr,
                         xcb_gcontext_t *gc,
                         cairo_surface_t **surface) {
    cairo_status_t status = CAIRO_STATUS_SUCCESS;
    if (*cr) {
        status = cairo_status(*cr);
    }
    if (status != CAIRO_STATUS_SUCCESS) {
        LOG("Found cairo context in an error status while freeing, error %d is %s",
            status, cairo_status_to_string(status));
    }

    if (*gc != XCB_NONE) {
        xcb_free_gc(conn, *gc);
        *gc = XCB_NONE;
    }
    if (*surface != NULL) {
        cairo_surface_destroy(*surface);
    }
    if (*cr != NULL) {
        cairo_destroy(*cr);
    }

    /* We need to explicitly set these to NULL to avoid assertion errors in
     * cairo when calling this multiple times. This can happen, for example,
     * when setting the border of a window to none and then closing it. */
    *surface = NULL;
    *cr = NULL;
}

void sparse_surface_set_geometry(sparse_surface_t *surface, int width, int height,
                                 int top_height, int bottom_height,
                                 int left_width, int right_width) {
    width = MAX(width, 1);
    height = MAX(height, 1);

    surface->width = width;
    surface->height = height;

    if (top_height == 0 && bottom_height == 0 && left_width == 0 && right_width == 0) {
        /* A stacked/tabbed parent uses one full-size top surface. */
        for (int i = 0; i < SPARSE_SURFACE_SIDE_COUNT; i++) {
            surface->x_offset[i] = 0;
            surface->y_offset[i] = 0;
            surface->side_surface[i].width = 0;
            surface->side_surface[i].height = 0;
        }

        surface->side_surface[SPARSE_SURFACE_TOP].width = width;
        surface->side_surface[SPARSE_SURFACE_TOP].height = height;
        return;
    }

    top_height = MIN(MAX(top_height, 0), height);
    bottom_height = MIN(MAX(bottom_height, 0), height - top_height);
    left_width = MIN(MAX(left_width, 0), width);
    right_width = MIN(MAX(right_width, 0), width - left_width);

    surface->side_surface[SPARSE_SURFACE_TOP].width = width - left_width - right_width;
    surface->side_surface[SPARSE_SURFACE_TOP].height = top_height;
    surface->x_offset[SPARSE_SURFACE_TOP] = left_width;
    surface->y_offset[SPARSE_SURFACE_TOP] = 0;

    surface->side_surface[SPARSE_SURFACE_BOTTOM].width = width - left_width - right_width;
    surface->side_surface[SPARSE_SURFACE_BOTTOM].height = bottom_height;
    surface->x_offset[SPARSE_SURFACE_BOTTOM] = left_width;
    surface->y_offset[SPARSE_SURFACE_BOTTOM] = height - bottom_height;

    surface->side_surface[SPARSE_SURFACE_LEFT].width = left_width;
    surface->side_surface[SPARSE_SURFACE_LEFT].height = height;
    surface->x_offset[SPARSE_SURFACE_LEFT] = 0;
    surface->y_offset[SPARSE_SURFACE_LEFT] = 0;

    surface->side_surface[SPARSE_SURFACE_RIGHT].width = right_width;
    surface->side_surface[SPARSE_SURFACE_RIGHT].height = height;
    surface->x_offset[SPARSE_SURFACE_RIGHT] = width - right_width;
    surface->y_offset[SPARSE_SURFACE_RIGHT] = 0;
}

static bool sparse_surface_create_one_side(xcb_connection_t *conn,
                                           sparse_surface_t *sparse_surface,
                                           sparse_surface_side side,
                                           xcb_drawable_t frame_id,
                                           uint16_t win_depth,
                                           xcb_visualtype_t *visual) {
    surface_t *surface = &(sparse_surface->side_surface[side]);
    if (surface->width == 0 || surface->height == 0) {
        sparse_surface_free_one_side(conn, sparse_surface, side);
        return true;
    }

    sparse_surface_free_one_side(conn, sparse_surface, side);

    const xcb_pixmap_t pixmap = xcb_generate_id(conn);
    xcb_void_cookie_t pixmap_cookie = xcb_create_pixmap_checked(
        conn, win_depth, pixmap, frame_id, surface->width, surface->height);
    xcb_generic_error_t *error = xcb_request_check(conn, pixmap_cookie);
    if (error != NULL) {
        ELOG("Could not create sparse surface pixmap. X error code: %d.\n",
             error->error_code);
        free(error);
        return false;
    }

    const xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_void_cookie_t gc_cookie = xcb_create_gc_checked(conn, gc, pixmap, 0, NULL);
    error = xcb_request_check(conn, gc_cookie);
    if (error != NULL) {
        ELOG("Could not create sparse surface graphics context. X error code: %d.\n",
             error->error_code);
        free(error);
        xcb_free_pixmap(conn, pixmap);
        return false;
    }

    cairo_surface_t *cairo_surface = cairo_xcb_surface_create(
        conn, pixmap, visual, surface->width, surface->height);
    if (cairo_surface_status(cairo_surface) != CAIRO_STATUS_SUCCESS) {
        ELOG("Could not create sparse surface Cairo surface: %s.\n",
             cairo_status_to_string(cairo_surface_status(cairo_surface)));
        cairo_surface_destroy(cairo_surface);
        xcb_free_gc(conn, gc);
        xcb_free_pixmap(conn, pixmap);
        return false;
    }

    cairo_t *cr = cairo_create(cairo_surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        ELOG("Could not create sparse surface Cairo context: %s.\n",
             cairo_status_to_string(cairo_status(cr)));
        cairo_destroy(cr);
        cairo_surface_destroy(cairo_surface);
        xcb_free_gc(conn, gc);
        xcb_free_pixmap(conn, pixmap);
        return false;
    }

    surface->id = pixmap;
    surface->gc = gc;
    surface->surface = cairo_surface;
    surface->cr = cr;
    cairo_translate(cr, -(sparse_surface->x_offset[side]),
                    -(sparse_surface->y_offset[side]));
    return true;
}

/*
 * Initialize the sparse surface to represent the given drawables.
 *
 */
void sparse_surface_create_and_init(xcb_connection_t *conn, sparse_surface_t *sparse_surface,
                                    xcb_drawable_t frame_id,
                                    uint16_t win_depth,
                                    xcb_visualtype_t *visual, int width, int height,
                                    int top_height, int bottom_height, int left_width, int right_width) {
    sparse_surface_set_geometry(sparse_surface, width, height,
                                top_height, bottom_height,
                                left_width, right_width);

    if (visual == NULL) {
        visual = visual_type;
    }

    for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
        if (!sparse_surface_create_one_side(conn, sparse_surface, side, frame_id,
                                            win_depth, visual)) {
            sparse_surface_free(conn, sparse_surface);
            return;
        }
    }
}

/*
 * Destroys the sparse surface.
 *
 */
void sparse_surface_free(xcb_connection_t *conn, sparse_surface_t *sparse_surface) {
    for (int i = 0; i < SPARSE_SURFACE_SIDE_COUNT; i++) {
        sparse_surface_free_one_side(conn, sparse_surface, i);
    }
}

/*
 * Destroys one side of the sparse surface.
 *
 */
static void sparse_surface_free_one_side(xcb_connection_t *conn, sparse_surface_t *sparse_surface, sparse_surface_side side) {
    if (sparse_surface->side_surface[side].id != XCB_NONE) {
        surface_free(conn,
                     &(sparse_surface->side_surface[side].cr),
                     &(sparse_surface->side_surface[side].gc),
                     &(sparse_surface->side_surface[side].surface));

        xcb_free_pixmap(conn, sparse_surface->side_surface[side].id);

        sparse_surface->side_surface[side].id = XCB_NONE;
    }
}

/*
 * Clears a sparse surface with the given color.
 *
 */
void sparse_surface_clear(sparse_surface_t *sparse_surface, color_t color) {
    for (int i = 0; i < SPARSE_SURFACE_SIDE_COUNT; i++) {
        if (sparse_surface->side_surface[i].id != XCB_NONE) {
            draw_util_clear_surface(&(sparse_surface->side_surface[i]), color);
        }
    }
}

/**
 * Draw the given text using libi3.
 *
 */
void sparse_surface_text(i3String *text, sparse_surface_t *surface, color_t fg_color, color_t bg_color, int x, int y, int max_width) {
    /* Draw to each of the four border pixmaps. This ensures that any draws that cross pixmap boundaries are
     * drawn correctly. */
    for (int i = 0; i < SPARSE_SURFACE_SIDE_COUNT; i++) {
        if (surface->side_surface[i].id != XCB_NONE) {
            draw_util_text(text, &(surface->side_surface[i]), fg_color, bg_color,
                           x - surface->x_offset[i], y - surface->y_offset[i],
                           max_width);
        }
    }
}

/**
 * Draws a filled rectangle.
 *
 */
void sparse_surface_rectangle(sparse_surface_t *surface, color_t color, double x, double y, double w, double h) {
    /* Draw to each of the four border pixmaps. This ensures that any draws that cross pixmap boundaries are
     * drawn correctly. */
    for (int i = 0; i < SPARSE_SURFACE_SIDE_COUNT; i++) {
        if (surface->side_surface[i].id != XCB_NONE) {
            draw_util_rectangle(&(surface->side_surface[i]), color, x, y, w, h);
        }
    }
}

/**
 * Draw the given image using libi3.
 * This function is a convenience wrapper and takes care of flushing the
 * surface as well as restoring the cairo state.
 *
 */
void sparse_surface_image(cairo_surface_t *image, sparse_surface_t *surface, int x, int y, int width, int height) {
    /* Draw to each of the four border pixmaps. This ensures that any draws that cross pixmap boundaries are
     * drawn correctly. */
    for (int i = 0; i < SPARSE_SURFACE_SIDE_COUNT; i++) {
        if (surface->side_surface[i].id != XCB_NONE) {
            draw_util_image(image, &(surface->side_surface[i]), x, y, width, height);
        }
    }
}

/**
 * Disable GraphicsExposure events
 */
void sparse_surface_disable_graphics_exposure_events(xcb_connection_t *conn, sparse_surface_t *surface) {
    for (int i = 0; i < SPARSE_SURFACE_SIDE_COUNT; i++) {
        if (surface->side_surface[i].id != XCB_NONE) {
            xcb_change_gc(conn, surface->side_surface[i].gc, XCB_GC_GRAPHICS_EXPOSURES, (uint32_t[]){0});
        }
    }
}
