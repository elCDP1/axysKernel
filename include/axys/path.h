#ifndef AXYS_PATH_H
#define AXYS_PATH_H

#include "axys/types.h"

/* Canonicalize an absolute VFS path in place semantics.
 *
 * Collapses "//", "/./" and trailing slashes, resolves "/../" without ever
 * escaping the root, and rejects relative paths outright. Each component must
 * fit in AXYS_VFS_NAME_MAX bytes (including NUL) and the result in `out_cap`
 * bytes. Returns 0 on success, -1 when the input is relative/malformed and
 * -2 when a component or the result does not fit (caller maps to EINVAL and
 * ENAMETOOLONG respectively). `in` and `out` may be the same buffer.
 */
int axys_path_normalize(const char *in, char *out, axys_size_t out_cap);

#endif
