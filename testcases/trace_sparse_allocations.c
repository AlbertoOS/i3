/* Test-only LD_PRELOAD observer for allocations in a private i3 process. */
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#include <xcb/xcb.h>

xcb_void_cookie_t xcb_create_pixmap_checked(xcb_connection_t *connection, uint8_t depth,
                                            xcb_pixmap_t pixmap, xcb_drawable_t drawable,
                                            uint16_t width, uint16_t height) {
    typedef xcb_void_cookie_t (*create_fn)(xcb_connection_t *, uint8_t, xcb_pixmap_t,
                                           xcb_drawable_t, uint16_t, uint16_t);
    create_fn original = (create_fn)dlsym(RTLD_NEXT, "xcb_create_pixmap_checked");
    if (original == NULL) {
        abort();
    }
    static const char event[] = "SPARSE_TEST_PIXMAP\n";
    (void)write(STDERR_FILENO, event, sizeof(event) - 1);
    return original(connection, depth, pixmap, drawable, width, height);
}

xcb_generic_error_t *xcb_request_check(xcb_connection_t *connection, xcb_void_cookie_t cookie) {
    typedef xcb_generic_error_t *(*check_fn)(xcb_connection_t *, xcb_void_cookie_t);
    check_fn original = (check_fn)dlsym(RTLD_NEXT, "xcb_request_check");
    if (original == NULL) {
        abort();
    }
    static const char event[] = "SPARSE_TEST_CHECK\n";
    (void)write(STDERR_FILENO, event, sizeof(event) - 1);
    return original(connection, cookie);
}
