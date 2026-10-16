/* Private-Xvfb pixel probe; build with: cc probe_frame_pixels.c -o probe -lxcb */
#include <xcb/xcb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 4 || (strcmp(argv[3], "sample") && strcmp(argv[3], "clear")) ||
        (argc - 4) % 2 != 0) {
        fail("usage: probe DISPLAY CLIENT sample [X Y ...] | clear");
    }
    xcb_connection_t *conn = xcb_connect(argv[1], NULL);
    if (xcb_connection_has_error(conn)) {
        fail("cannot connect to private display");
    }
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_window_t client = strtoul(argv[2], NULL, 0);
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(conn, xcb_query_tree(conn, client), NULL);
    if (!tree || tree->parent == tree->root) {
        fail("client has no WM frame parent");
    }
    xcb_window_t frame = tree->parent;
    free(tree);
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, frame), NULL);
    if (!geometry || geometry->depth != 24) {
        fail("expected a depth-24 frame on private Xvfb");
    }
    int width = geometry->width, height = geometry->height;
    free(geometry);
    /* The harness starts a depth-24 Xvfb, whose pixels are TrueColor RGB. */
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    int rgb = 0;
    for (xcb_depth_iterator_t depth = xcb_screen_allowed_depths_iterator(screen); depth.rem; xcb_depth_next(&depth)) {
        for (xcb_visualtype_iterator_t visual = xcb_depth_visuals_iterator(depth.data); visual.rem; xcb_visualtype_next(&visual)) {
            if (visual.data->visual_id == screen->root_visual) {
                rgb = visual.data->red_mask == 0xff0000 && visual.data->green_mask == 0xff00 && visual.data->blue_mask == 0xff;
            }
        }
    }
    int bpp = 0;
    for (xcb_format_iterator_t format = xcb_setup_pixmap_formats_iterator(setup); format.rem; xcb_format_next(&format)) {
        if (format.data->depth == 24) {
            bpp = format.data->bits_per_pixel;
        }
    }
    if (!rgb || bpp != 32) {
        fail("expected depth-24 RGB visuals with 32-bit image pixels");
    }
    if (!strcmp(argv[3], "clear")) {
        /* A non-default, non-palette poison color, not an assumed black background.
         * ClearArea both damages the visible frame and requests genuine Expose events. */
        uint32_t poison = 0xa143d7;
        xcb_generic_error_t *error = xcb_request_check(conn, xcb_change_window_attributes_checked(conn, frame, XCB_CW_BACK_PIXEL, &poison));
        if (error) {
            fail("cannot set frame poison background");
        }
        error = xcb_request_check(conn, xcb_clear_area_checked(conn, 1, frame, 0, 0, 0, 0));
        if (error) {
            fail("cannot clear frame with Expose events");
        }
    }
    printf("%u %d %d", frame, width, height);
    for (int i = 4; i < argc; i += 2) {
        int x = atoi(argv[i]), y = atoi(argv[i + 1]);
        if (x < 0 || y < 0 || x >= width || y >= height) {
            fail("pixel lies outside frame");
        }
        xcb_get_image_reply_t *image = xcb_get_image_reply(conn, xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, frame, x, y, 1, 1, UINT32_MAX), NULL);
        if (!image || xcb_get_image_data_length(image) < 4) {
            fail("cannot snapshot frame pixel");
        }
        const uint8_t *bytes = xcb_get_image_data(image);
        uint32_t pixel = 0;
        for (int j = 0; j < 4; j++) {
            int shift = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ? j : 3 - j;
            pixel |= (uint32_t)bytes[j] << (8 * shift);
        }
        printf(" %06x", pixel & 0xffffff);
        free(image);
    }
    putchar('\n');
    xcb_disconnect(conn);
    return 0;
}
