#include "axys/path.h"
#include "axys/string.h"
#include "axys/vfs.h"

int axys_path_normalize(const char *in, char *out, axys_size_t out_cap)
{
    /* Component offsets into `out` (start of each accepted component). A fixed
     * scratch array avoids heap use; depth is bounded by path length. */
    axys_size_t comp_start[AXYS_VFS_MAX_PATH];
    axys_size_t depth = 0;
    axys_size_t len = 0;
    const char *p;

    if (in == AXYS_NULL || out == AXYS_NULL || out_cap == 0) {
        return -1;
    }
    if (in[0] != '/') {
        return -1; /* relative paths are rejected, never resolved */
    }
    out[0] = '\0';
    p = in;
    while (*p != '\0') {
        const char *seg;
        axys_size_t seg_len = 0;

        while (*p == '/') {
            ++p; /* collapse runs of slashes */
        }
        if (*p == '\0') {
            break;
        }
        seg = p;
        while (p[seg_len] != '\0' && p[seg_len] != '/') {
            ++seg_len;
        }
        if (seg_len == 1 && seg[0] == '.') {
            /* no-op */
        } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) {
                --depth;
                len = comp_start[depth];
                out[len] = '\0';
            }
            /* else: ".." above the root stays at the root */
        } else {
            axys_size_t need;

            if (seg_len >= AXYS_VFS_NAME_MAX) {
                return -2;
            }
            /* '/' + segment (+ NUL accounted by out_cap check below). */
            need = len + 1u + seg_len + 1u;
            if (need > out_cap || need > AXYS_VFS_MAX_PATH) {
                return -2;
            }
            if (depth >= AXYS_ARRAY_SIZE(comp_start)) {
                return -2;
            }
            comp_start[depth++] = len;
            out[len++] = '/';
            for (axys_size_t i = 0; i < seg_len; ++i) {
                out[len++] = seg[i];
            }
            out[len] = '\0';
        }
        p = seg + seg_len;
    }
    if (len == 0) {
        if (out_cap < 2u) {
            return -2;
        }
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}
