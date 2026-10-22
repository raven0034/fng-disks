/* white-box pathname tests. Requires mountinject.c */

#define main mountinject_main
#include "../src/mountinject/mountinject.c"
#undef main
#include <assert.h>

static void checked_close(struct destination *dst) {
    close_destination(dst);
}

int main(void) {
    char root[] = "/tmp/fng-mountinject-test-XXXXXX";
    assert(mkdtemp(root));

    char outside[512];
    char link[512];
    char nested[512];
    char invalid[1024];
    char file[512];

    snprintf(outside, sizeof(outside), "%s/outside", root);
    snprintf(link, sizeof(link), "%s/link", root);
    snprintf(nested, sizeof(nested), "%s/a/b/c", root);
    snprintf(file, sizeof(file), "%s/file", root);

    assert(mkdir(outside, 0700) == 0);
    assert(symlink(outside, link) == 0);

    FILE *f = fopen(file, "w"); assert(f); fclose(f);

    struct destination dst;
    assert(resolve_destination(nested, true, &dst) == 0);
    assert(dst.parent_fd >= 0 && dst.target_fd >= 0);
    assert(destination_is_mountpoint(&dst) == 0);
    checked_close(&dst);

    assert(resolve_destination(nested, false, &dst) == 0);
    checked_close(&dst);

    snprintf(invalid, sizeof(invalid), "%s/link/escape", root);
    assert(resolve_destination(invalid, true, &dst) < 0);

    snprintf(invalid, sizeof(invalid), "%s/../escape", root);
    assert(resolve_destination(invalid, true, &dst) == -EINVAL);

    snprintf(invalid, sizeof(invalid), "%s//escape", root);
    assert(resolve_destination(invalid, true, &dst) == -EINVAL);

    snprintf(invalid, sizeof(invalid), "%s/./escape", root);
    assert(resolve_destination(invalid, true, &dst) == -EINVAL);

    snprintf(invalid, sizeof(invalid), "%s/trailing/", root);
    assert(resolve_destination(invalid, true, &dst) == -EINVAL);
    assert(resolve_destination("/", true, &dst) == -EINVAL);
    assert(resolve_destination("relative", true, &dst) == -EINVAL);
    assert(resolve_destination(file, true, &dst) < 0);
    checked_close(&dst);

    // only dirs inside validated root should've been created
    snprintf(invalid, sizeof(invalid), "%s/escape", outside);
    assert(access(invalid, F_OK) < 0 && errno == ENOENT);

    snprintf(invalid, sizeof(invalid), "%s/a/b/c", root);
    assert(rmdir(invalid) == 0);

    snprintf(invalid, sizeof(invalid), "%s/a/b", root);
    assert(rmdir(invalid) == 0);

    snprintf(invalid, sizeof(invalid), "%s/a", root);
    assert(rmdir(invalid) == 0);

    unlink(file);
    unlink(link);
    rmdir(outside);
    rmdir(root);

    puts("mountinject pathname tests passed");
    return 0;
}
