/* XRes accounting for one explicitly identified, isolated i3 X connection.
 * Build: cc -std=c11 -Wall -Wextra -Werror query_i3_pixmap_bytes.c \
 *           $(pkg-config --cflags --libs xcb-res xcb) -o query-i3-pixmaps
 * Never discovers an ambient display or IPC socket.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/res.h>

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
}

static xcb_atom_t atom(xcb_connection_t *c, const char *name) {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 1, (uint16_t)strlen(name), name), NULL);
    if (!r || r->atom == XCB_NONE) {
        fail("Required i3 property atom absent");
    }
    xcb_atom_t result = r->atom;
    free(r);
    return result;
}

static xcb_get_property_reply_t *property(xcb_connection_t *c, xcb_window_t w,
                                          const char *name, xcb_atom_t type) {
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom(c, name), type, 0, 4096), NULL);
    if (!r || r->type != type || r->bytes_after) {
        fail("Required i3 property missing or malformed");
    }
    return r;
}

static uint32_t cardinal(xcb_connection_t *c, xcb_window_t w, const char *name,
                         xcb_atom_t type) {
    xcb_get_property_reply_t *r = property(c, w, name, type);
    if (r->format != 32 || xcb_get_property_value_length(r) != 4) {
        fail("Expected a single 32-bit property");
    }
    uint32_t value;
    memcpy(&value, xcb_get_property_value(r), sizeof(value));
    free(r);
    return value;
}

int main(int argc, char **argv) {
    if (argc != 4 || argv[1][0] != ':' || !argv[2][0]) {
        fail("Usage: query-i3-pixmaps :PRIVATE_DISPLAY PRIVATE_SOCKET I3_PID");
    }
    char *end;
    unsigned long pid = strtoul(argv[3], &end, 10);
    if (*end || !pid || pid > UINT32_MAX) {
        fail("Invalid i3 PID");
    }
    int screen_number;
    xcb_connection_t *c = xcb_connect(argv[1], &screen_number);
    if (xcb_connection_has_error(c)) {
        fail("Cannot connect to explicit private display");
    }
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_number && screens.rem; i++) {
        xcb_screen_next(&screens);
    }
    if (!screens.rem) {
        fail("No screen");
    }
    xcb_window_t root = screens.data->root;
    xcb_get_property_reply_t *socket = property(c, root, "I3_SOCKET_PATH", atom(c, "UTF8_STRING"));
    if (socket->format != 8 || xcb_get_property_value_length(socket) != (int)strlen(argv[2]) ||
        memcmp(xcb_get_property_value(socket), argv[2], strlen(argv[2]))) {
        fail("Private IPC socket does not match isolated root property");
    }
    free(socket);
    if (cardinal(c, root, "I3_PID", XCB_ATOM_CARDINAL) != pid) {
        fail("Isolated root I3_PID does not match launched process");
    }
    xcb_window_t support = cardinal(c, root, "_NET_SUPPORTING_WM_CHECK", XCB_ATOM_WINDOW);
    if (cardinal(c, support, "_NET_SUPPORTING_WM_CHECK", XCB_ATOM_WINDOW) != support) {
        fail("Supporting WM window does not point to itself");
    }
    xcb_res_query_version_reply_t *version = xcb_res_query_version_reply(
        c, xcb_res_query_version(c, 1, 2), NULL);
    if (!version || version->server_major < 1) {
        fail("XRes unavailable");
    }
    xcb_res_query_clients_reply_t *clients = xcb_res_query_clients_reply(
        c, xcb_res_query_clients(c), NULL);
    if (!clients) {
        fail("XRes client query failed");
    }
    xcb_res_client_t *list = xcb_res_query_clients_clients(clients);
    uint32_t base = 0, mask = 0;
    int matches = 0;
    for (int i = 0; i < xcb_res_query_clients_clients_length(clients); i++) {
        if ((support & ~list[i].resource_mask) == list[i].resource_base) {
            base = list[i].resource_base;
            mask = list[i].resource_mask;
            matches++;
        }
    }
    if (matches != 1) {
        fail("Supporting window must identify exactly one XRes client");
    }
    xcb_res_query_client_pixmap_bytes_reply_t *bytes = xcb_res_query_client_pixmap_bytes_reply(
        c, xcb_res_query_client_pixmap_bytes(c, base), NULL);
    if (!bytes) {
        fail("XRes pixmap byte query failed");
    }
    uint64_t total = ((uint64_t)bytes->bytes_overflow << 32) | bytes->bytes;
    printf("{\"pixmap_bytes\":%" PRIu64 ",\"supporting_window\":%" PRIu32
           ",\"resource_base\":%" PRIu32 ",\"resource_mask\":%" PRIu32
           ",\"i3_pid\":%lu,\"private_socket_verified\":true,\"xres_version\":\"%u.%u\"}\n",
           total, support, base, mask, pid, version->server_major, version->server_minor);
    free(bytes);
    free(clients);
    free(version);
    xcb_disconnect(c);
    return EXIT_SUCCESS;
}
