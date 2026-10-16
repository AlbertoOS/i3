/*
 * i3 - sparse surface X11 regression tests
 *
 * Resource creation failures and repeated reinitialization must release every
 * sparse edge, including the X resources already allocated before a failure.
 */

#include <stdio.h>
#include <stdlib.h>

#include <xcb/xcb.h>

#include "sparse_surface.h"

xcb_visualtype_t *visual_type = NULL;

void verboselog(char *fmt, ...) {
    (void)fmt;
}

void errorlog(char *fmt, ...) {
    (void)fmt;
}

static void require(bool condition, const char *description) {
    if (!condition) {
        fprintf(stderr, "%s\n", description);
        exit(EXIT_FAILURE);
    }
}

/* Test-only linker seam: --wrap=xcb_create_pixmap_checked. The second
 * allocation still goes through the real X server, but uses an invalid
 * drawable. No production failure switch or synthetic X error is needed. */
xcb_void_cookie_t __real_xcb_create_pixmap_checked(
    xcb_connection_t *connection, uint8_t depth, xcb_pixmap_t pixmap,
    xcb_drawable_t drawable, uint16_t width, uint16_t height);
xcb_void_cookie_t __wrap_xcb_create_pixmap_checked(
    xcb_connection_t *connection, uint8_t depth, xcb_pixmap_t pixmap,
    xcb_drawable_t drawable, uint16_t width, uint16_t height);

static sparse_surface_t *failing_surface;
static int allocation_count;
static xcb_pixmap_t first_pixmap;
static xcb_gcontext_t first_gc;

xcb_void_cookie_t __wrap_xcb_create_pixmap_checked(
    xcb_connection_t *connection, uint8_t depth, xcb_pixmap_t pixmap,
    xcb_drawable_t drawable, uint16_t width, uint16_t height) {
    if (failing_surface != NULL && ++allocation_count == 2) {
        surface_t *first = &failing_surface->side_surface[SPARSE_SURFACE_TOP];
        require(first->id != XCB_NONE && first->gc != XCB_NONE &&
                    first->surface != NULL && first->cr != NULL,
                "second allocation must fail after the first side is initialized");
        first_pixmap = first->id;
        first_gc = first->gc;
        drawable = XCB_NONE;
    }
    return __real_xcb_create_pixmap_checked(connection, depth, pixmap,
                                            drawable, width, height);
}

static xcb_screen_t *root_screen_for_connection(xcb_connection_t *connection,
                                                int screen_number) {
    xcb_screen_iterator_t iterator =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int index = 0; iterator.rem > 0 && index < screen_number; index++) {
        xcb_screen_next(&iterator);
    }
    return iterator.rem > 0 ? iterator.data : NULL;
}

static xcb_visualtype_t *root_visual_for_screen(xcb_screen_t *screen) {
    for (xcb_depth_iterator_t depth = xcb_screen_allowed_depths_iterator(screen);
         depth.rem > 0; xcb_depth_next(&depth)) {
        for (xcb_visualtype_iterator_t visual =
                 xcb_depth_visuals_iterator(depth.data);
             visual.rem > 0; xcb_visualtype_next(&visual)) {
            if (visual.data->visual_id == screen->root_visual) {
                return visual.data;
            }
        }
    }
    return NULL;
}

static void expect_empty(sparse_surface_t *surface) {
    require(!sparse_surface_initialized(surface),
            "freed or failed sparse surface must not be initialized");
    for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
        surface_t *edge = &surface->side_surface[side];
        require(edge->id == XCB_NONE && edge->gc == XCB_NONE &&
                    edge->cr == NULL && edge->surface == NULL,
                "freed or failed side must clear every resource handle");
    }
}

static void expect_released(xcb_connection_t *connection,
                            xcb_pixmap_t pixmap, xcb_gcontext_t gc) {
    xcb_generic_error_t *error = NULL;
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
        connection, xcb_get_geometry(connection, pixmap), &error);
    require(geometry == NULL && error != NULL && error->error_code == XCB_DRAWABLE,
            "released pixmap must no longer exist on the X server");
    free(geometry);
    free(error);

    error = xcb_request_check(connection, xcb_free_gc_checked(connection, gc));
    require(error != NULL && error->error_code == XCB_G_CONTEXT,
            "released graphics context must no longer exist on the X server");
    free(error);
}

static void expect_live(xcb_connection_t *connection, sparse_surface_t *surface) {
    require(sparse_surface_initialized(surface), "created surface must be initialized");
    for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
        surface_t *edge = &surface->side_surface[side];
        if (edge->width == 0 || edge->height == 0) {
            require(edge->id == XCB_NONE && edge->gc == XCB_NONE &&
                        edge->cr == NULL && edge->surface == NULL,
                    "absent edge must not retain resources");
            continue;
        }
        require(edge->id != XCB_NONE && edge->gc != XCB_NONE &&
                    edge->surface != NULL && edge->cr != NULL,
                "nonempty edge must own all its resources");
        require(cairo_status(edge->cr) == CAIRO_STATUS_SUCCESS &&
                    cairo_surface_status(edge->surface) == CAIRO_STATUS_SUCCESS,
                "created edge must have healthy Cairo objects");
        xcb_generic_error_t *error = NULL;
        xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
            connection, xcb_get_geometry(connection, edge->id), &error);
        require(error == NULL && geometry != NULL &&
                    geometry->width == edge->width && geometry->height == edge->height,
                "created edge must have the requested X pixmap geometry");
        free(geometry);
        free(error);
    }
}

int main(void) {
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    require(xcb_connection_has_error(connection) == 0,
            "could not connect to the isolated X server");

    xcb_screen_t *screen = root_screen_for_connection(connection, screen_number);
    xcb_visualtype_t *visual = screen == NULL ? NULL : root_visual_for_screen(screen);
    require(screen != NULL && visual != NULL,
            "could not resolve isolated X server root visual");

    sparse_surface_t surface = {0};
    sparse_surface_free(connection, &surface);
    expect_empty(&surface);
    sparse_surface_create_and_init(connection, &surface, XCB_NONE,
                                   screen->root_depth, visual,
                                   10, 10, 1, 1, 1, 1);
    expect_empty(&surface);

    failing_surface = &surface;
    sparse_surface_create_and_init(connection, &surface, screen->root,
                                   screen->root_depth, visual,
                                   10, 10, 1, 1, 1, 1);
    failing_surface = NULL;
    require(allocation_count == 2, "failure must stop at the second allocation");
    expect_empty(&surface);
    expect_released(connection, first_pixmap, first_gc);
    sparse_surface_free(connection, &surface);
    expect_empty(&surface);

    /* Recover after failure, then alternate sparse/full/missing-edge layouts
     * without an intervening free. Every replacement must release old XIDs. */
    visual_type = visual;
    for (int iteration = 0; iteration < 8; iteration++) {
        for (int layout = 0; layout < 3; layout++) {
            xcb_pixmap_t old_pixmaps[SPARSE_SURFACE_SIDE_COUNT];
            xcb_gcontext_t old_gcs[SPARSE_SURFACE_SIDE_COUNT];
            for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
                old_pixmaps[side] = surface.side_surface[side].id;
                old_gcs[side] = surface.side_surface[side].gc;
            }
            sparse_surface_create_and_init(connection, &surface, screen->root,
                                           screen->root_depth, NULL,
                                           10 + iteration, 12 + iteration,
                                           layout == 1 ? 0 : 2,
                                           layout == 0 ? 1 : 0,
                                           layout == 0 ? 1 : 0,
                                           layout == 1 ? 0 : 2);
            expect_live(connection, &surface);
            for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
                if (old_pixmaps[side] != XCB_NONE) {
                    expect_released(connection, old_pixmaps[side], old_gcs[side]);
                }
            }
        }
        xcb_pixmap_t pixmaps[SPARSE_SURFACE_SIDE_COUNT];
        xcb_gcontext_t gcs[SPARSE_SURFACE_SIDE_COUNT];
        for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
            pixmaps[side] = surface.side_surface[side].id;
            gcs[side] = surface.side_surface[side].gc;
        }
        sparse_surface_free(connection, &surface);
        expect_empty(&surface);
        for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
            if (pixmaps[side] != XCB_NONE) {
                expect_released(connection, pixmaps[side], gcs[side]);
            }
        }
        sparse_surface_free(connection, &surface);
        expect_empty(&surface);
    }

    /* A round trip makes unchecked double-free errors observable. */
    xcb_get_input_focus_reply_t *focus = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), NULL);
    require(focus != NULL, "final X server round trip failed");
    free(focus);
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_event(connection)) != NULL) {
        require(event->response_type != 0, "unexpected asynchronous X error during lifecycle test");
        free(event);
    }
    require(xcb_connection_has_error(connection) == 0, "X connection failed during lifecycle test");
    xcb_disconnect(connection);
    puts("sparse surface lifecycle: partial failure and 24 recreations passed");
    return EXIT_SUCCESS;
}
