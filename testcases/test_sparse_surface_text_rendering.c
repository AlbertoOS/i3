/*
 * i3 - sparse surface real-font regression tests
 *
 * Compare each actual X pixmap with the corresponding crop of a normal
 * surface. This exercises libi3's real Pango and core-X font drawing paths,
 * not a draw_util_text stub, including glyphs crossing edge boundaries.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sparse_surface.h"

xcb_connection_t *conn;
xcb_screen_t *root_screen;
xcb_visualtype_t *visual_type;

void verboselog(char *fmt, ...) {
    (void)fmt;
}

void debuglog(char *fmt, ...) {
    (void)fmt;
}

void errorlog(char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
}

static cairo_surface_t *snapshot(surface_t *surface) {
    cairo_surface_flush(surface->surface);
    cairo_surface_t *image = cairo_image_surface_create(
        CAIRO_FORMAT_RGB24, surface->width, surface->height);
    cairo_t *cr = cairo_create(image);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, surface->surface, 0, 0);
    cairo_paint(cr);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        fail("could not read back the X pixmap");
    }
    cairo_destroy(cr);
    cairo_surface_flush(image);
    return image;
}

static void compare_side(surface_t *side, int offset_x, int offset_y,
                         cairo_surface_t *reference, const char *font) {
    cairo_surface_t *actual = snapshot(side);
    const unsigned char *actual_data = cairo_image_surface_get_data(actual);
    const unsigned char *reference_data = cairo_image_surface_get_data(reference);
    const int actual_stride = cairo_image_surface_get_stride(actual);
    const int reference_stride = cairo_image_surface_get_stride(reference);
    for (int y = 0; y < side->height; y++) {
        for (int x = 0; x < side->width; x++) {
            uint32_t a, b;
            memcpy(&a, actual_data + y * actual_stride + x * 4, sizeof(a));
            memcpy(&b, reference_data + (y + offset_y) * reference_stride + (x + offset_x) * 4, sizeof(b));
            /* RGB24's unused byte has no specified value. */
            if ((a & 0xffffff) != (b & 0xffffff)) {
                fprintf(stderr, "%s: glyph mismatch at local (%d,%d), offset (%d,%d)\n",
                        font, x, y, offset_x, offset_y);
                exit(EXIT_FAILURE);
            }
        }
    }
    cairo_surface_destroy(actual);
}

static void exercise_font(const char *pattern, int expected_type) {
    i3Font font = load_font(pattern, false);
    if ((int)font.type != expected_type) {
        fail("requested font backend was not loaded");
    }
    set_font(&font);
    sparse_surface_t sparse = {0};
    sparse_surface_create_and_init(conn, &sparse, root_screen->root,
                                   root_screen->root_depth, visual_type,
                                   160, 80, 28, 22, 12, 18);
    if (!sparse_surface_initialized(&sparse)) {
        fail("could not allocate sparse X pixmaps");
    }
    surface_t full = {0};
    xcb_pixmap_t pixmap = xcb_generate_id(conn);
    xcb_void_cookie_t cookie = xcb_create_pixmap_checked(
        conn, root_screen->root_depth, pixmap, root_screen->root, 160, 80);
    xcb_generic_error_t *error = xcb_request_check(conn, cookie);
    if (error != NULL) {
        free(error);
        fail("could not allocate reference X pixmap");
    }
    draw_util_surface_init(conn, &full, pixmap, visual_type, 160, 80);
    const color_t black = {.alpha = 1.0};
    const color_t white = {.red = 1.0, .green = 1.0, .blue = 1.0, .alpha = 1.0, .colorpixel = root_screen->white_pixel};
    const int positions[][2] = {{8, 3}, {112, 3}, {8, 60}, {112, 60}};
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        sparse_surface_clear(&sparse, black);
        draw_util_clear_surface(&full, black);
        i3String *text = i3string_from_utf8("MMMMMM");
        sparse_surface_text(text, &sparse, white, black,
                            positions[i][0], positions[i][1], 100);
        draw_util_text(text, &full, white, black,
                       positions[i][0], positions[i][1], 100);
        i3string_free(text);
        cairo_surface_t *reference = snapshot(&full);
        /* Ensure equality is not vacuously comparing two blank images. */
        const unsigned char *data = cairo_image_surface_get_data(reference);
        const int stride = cairo_image_surface_get_stride(reference);
        bool has_ink = false;
        for (int y = 0; y < 80; y++) {
            for (int x = 0; x < 160; x++) {
                uint32_t pixel;
                memcpy(&pixel, data + y * stride + x * 4, sizeof(pixel));
                has_ink |= (pixel & 0xffffff) != 0;
            }
        }
        if (!has_ink) {
            fail("reference font rendering produced no glyph pixels");
        }
        for (int side = 0; side < SPARSE_SURFACE_SIDE_COUNT; side++) {
            compare_side(&sparse.side_surface[side], sparse.x_offset[side],
                         sparse.y_offset[side], reference, pattern);
        }
        cairo_surface_destroy(reference);
    }
    sparse_surface_free(conn, &sparse);
    draw_util_surface_free(conn, &full);
    xcb_free_pixmap(conn, pixmap);
    free_font();
    printf("real-font sparse pixels: %s passed\n", pattern);
}

int main(void) {
    int screen_number;
    conn = xcb_connect(NULL, &screen_number);
    if (xcb_connection_has_error(conn)) {
        fail("could not connect to isolated X server");
    }
    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; i < screen_number; i++) {
        xcb_screen_next(&iterator);
    }
    root_screen = iterator.data;
    visual_type = get_visualtype(root_screen);
    init_dpi();
    exercise_font("pango:monospace 10", FONT_TYPE_PANGO);
    exercise_font("fixed", FONT_TYPE_XCB);
    xcb_disconnect(conn);
    return EXIT_SUCCESS;
}
