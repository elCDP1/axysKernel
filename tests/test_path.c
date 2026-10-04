#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "axys/path.h"
#include "axys/vfs.h"

static void check(const char *in, const char *want)
{
    char out[AXYS_VFS_MAX_PATH];
    int rc = axys_path_normalize(in, out, sizeof(out));

    assert(rc == 0);
    assert(strcmp(out, want) == 0);
}

int main(void)
{
    char out[AXYS_VFS_MAX_PATH];

    check("/", "/");
    check("///", "/");
    check("/a", "/a");
    check("/a/", "/a");
    check("/a//b", "/a/b");
    check("/a/./b", "/a/b");
    check("/a/../b", "/b");
    check("/a/b/../../c", "/c");
    check("/../..", "/");
    check("/a/b/..", "/a");
    check("/a/b/.", "/a/b");

    /* Relative paths are rejected. */
    assert(axys_path_normalize("", out, sizeof(out)) == -1);
    assert(axys_path_normalize("a/b", out, sizeof(out)) == -1);
    assert(axys_path_normalize("./a", out, sizeof(out)) == -1);

    /* Overlong components and paths do not fit. */
    {
        char long_comp[AXYS_VFS_NAME_MAX + 8];
        char input[sizeof(long_comp) + 8];

        memset(long_comp, 'n', sizeof(long_comp) - 1);
        long_comp[sizeof(long_comp) - 1] = '\0';
        snprintf(input, sizeof(input), "/tmp/%s", long_comp);
        assert(axys_path_normalize(input, out, sizeof(out)) == -2);
    }
    printf("test_path: ok\n");
    return 0;
}
