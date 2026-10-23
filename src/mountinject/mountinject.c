/*
* Intentionally short-lived mount namespace helper.
*
* Feats:
*   - ADD:
*       - clone host mount w/ open_tree
*       - join container mount ns
*       - attach to symlink-safe dst w/ move_mount
*   - REMOVE:
*       - join container mount ns
*       - lazily detach mount
*       - verify dst no longer active mountpoint
*
* Runs via podman unshare - gives user ns ctx needed for setns/mount in rootless container.
*
* This helper assumes trusted valid args from bridge (should be under normal usage).
*/

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <linux/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
* Bridge invokes helper through podman unshare. src resolved before setns, dst paths resolved after.
*
* Join /proc/PID/ns/mnt, keeping ns fd open via setns.
*
* Switching mount ns does not change proc root (ie chroot)
*   - dst res intentionally starts at root helper sees *post* setns
*/
static int enter_mount_ns(const char *pid) {
    if (!*pid || strspn(pid, "0123456789") != strlen(pid)) {
        fprintf(stderr, "invalid PID: %s\n", pid);
        return -EINVAL;
    }

    char path[64];
    if (snprintf(path, sizeof(path), "/proc/%s/ns/mnt", pid) >= (int)sizeof(path))
        return -EINVAL;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return -errno;
    }

    int r = setns(fd, CLONE_NEWNS);
    if (r < 0) {
        r = -errno;
        fprintf(stderr, "setns(%s): %s\n", path, strerror(-r));
    }

    close(fd);
    return r;
}

// both fds: pinned O_PATH descs in dst mnt ns
// name: last path comp relative to parent_fd
// don't turn into raw concat path for ops that can use dirfd (want to avoid pathname/symlink races)
struct destination {
    int parent_fd;
    int target_fd;
    char *name;
};

// release all owned resources; return dst to invalid state
// all return path from successful resolve_destination must call
static void close_destination(struct destination *dst) {
    if (dst->target_fd >= 0)
        close(dst->target_fd);

    if (dst->parent_fd >= 0)
        close(dst->parent_fd);

    free(dst->name);

    *dst = (struct destination){ .parent_fd = -1, .target_fd = -1 };
}

/*
* Resolve from current ns root *without following symlinks*
* Directory FDs pin each comp (incl. final mount target)
* Final comp kept rel to parent (for safe unlinkat)
*
* Return 0 w/ parent_fd, target_fd & name populated, or -errno
* create: true (add), false (remove)
* Reject ".", "..", empty comps and symlink traversal (incl. in leaf)
* Validate whole path before mkdirat (avoid partial creation of rejected path)
*
* O_PATH|O_NOFOLLOW|O_DIRECTORY pins each dir w/o following links
*   - Reduces pathname races
*   - Potential final-comp race with umount2?
*/
static int resolve_destination(const char *path, bool create, struct destination *dst) {
    *dst = (struct destination){ .parent_fd = -1, .target_fd = -1 };

    if (path[0] != '/' || path[1] == '\0')
        return -EINVAL;

    // validate whole path before mkdirat
    // e.g. "/big/big/../tesco" must not create "/big/big"
    for (const char *p = path + 1; ; ) {
        const char *end = strchr(p, '/');

        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (!len || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.'))
            return -EINVAL;

        if (!end)
            break;

        p = end + 1;
    }

    char *copy = strdup(path + 1);
    if (!copy)
        return -ENOMEM;

    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        int r = -errno;
        free(copy);
        return r;
    }

    int r = 0;
    char *next = copy;
    for (char *component; (component = strsep(&next, "/"));) {
        if (!*component || !strcmp(component, ".") || !strcmp(component, "..")) {
            r = -EINVAL;
            break;
        }

        if (create && mkdirat(fd, component, 0755) < 0 && errno != EEXIST) {
            r = -errno;
            break;
        }

        int child = openat(fd, component, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        if (child < 0) {
            r = -errno;
            break;
        }

        if (!next) {
            dst->parent_fd = fd;
            dst->target_fd = child;
            dst->name = strdup(component);

            if (!dst->name) {
                r = -ENOMEM;
                close_destination(dst);
            }

            fd = -1;
            break;
        }

        close(fd);
        fd = child;
    }

    if (fd >= 0)
        close(fd);

    free(copy);
    return r;
}

// pre-existing mount at final dst should not be mounted over, refused as such
// compare STATX_MNT_ID on pinned parent & target
// diff mountIDs --> target is mountpoint. 0 (ordinary dir), 1 (mountpoint), <0 (no safe id)
static int destination_is_mountpoint(const struct destination *dst) {
    struct statx parent, target;

    if (statx(dst->parent_fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &parent) < 0 ||
        statx(dst->target_fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &target) < 0)
        return -errno;

    if (!(parent.stx_mask & STATX_MNT_ID) || !(target.stx_mask & STATX_MNT_ID))
        return -EOPNOTSUPP;

    return parent.stx_mnt_id != target.stx_mnt_id;
}

// src opened and cloned before setns. src path meaningful in host ns.
// cloned fd --> not attached mount obj, can be attached by move_mount w/o src path lookup
// dst lookup and move_mount after setns. target is opened O_PATH desc.
// on error, helper exits <0, bridge retains state for retry.
static int do_add(const char *src, const char *pid, const char *dst_path) {
    // pin src mount dir before entering mount ns
    int source_fd = open(src, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (source_fd < 0) {
        fprintf(stderr, "open source(%s): %s\n", src, strerror(errno));
        return 1;
    }

    int mount_fd = syscall(SYS_open_tree, source_fd, "", AT_EMPTY_PATH | OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
    int saved_errno = errno;
    close(source_fd);
    if (mount_fd < 0) {
        fprintf(stderr, "open_tree(%s): %s\n", src, strerror(saved_errno));
        return 1;
    }

    int result = 1;
    struct destination dst = { .parent_fd = -1, .target_fd = -1 };
    if (enter_mount_ns(pid) < 0)
        goto out;

    int r = resolve_destination(dst_path, true, &dst);
    if (r < 0) {
        fprintf(stderr, "destination(%s): %s\n", dst_path, strerror(-r));
        goto out;
    }

    r = destination_is_mountpoint(&dst);
    if (r != 0) {
        fprintf(stderr, "destination(%s): %s\n", dst_path, r > 0 ? "already a mountpoint" : strerror(-r));
        goto out;
    }

    if (syscall(SYS_move_mount, mount_fd, "", dst.target_fd, "", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) < 0) {
        fprintf(stderr, "move_mount(%s): %s\n", dst_path, strerror(errno));
        goto out;
    }

    fprintf(stderr, "mounted %s -> %s\n", src, dst_path);
    result = 0;

out:
    close_destination(&dst);
    close(mount_fd);
    return result;
}

/*
* Remove container-side mount only (never calls Unmount on host - job of bridge post-successful detach).
*
* MNT_DETACH is lazy - other procs can hold refs to old fs.
* Confirm currently visible dst no longer has mount --> close target_fd --> unlink now empty dir.
* Don't announce success if dir removal fails - avoids incorrect mounted=false reporting which can cause remount rejection.
*/
static int do_remove(const char *pid, const char *dst_path) {
    if (enter_mount_ns(pid) < 0)
        return 1;

    struct destination dst;
    int r = resolve_destination(dst_path, false, &dst);
    if (r == -ENOENT)
        return 0;

    if (r < 0) {
        fprintf(stderr, "destination(%s): %s\n", dst_path, strerror(-r));
        return 1;
    }

    int result = 1;
    r = destination_is_mountpoint(&dst);
    if (r < 0) {
        fprintf(stderr, "mountpoint check(%s): %s\n", dst_path, strerror(-r));
        goto out;
    }

    if (r > 0) {
        // Refuse to unmount a different object if the final directory was
        // replaced between the pinned lookup and the unmount request.
        // don't unmount diff obj if final dir replaced between pinned lookup & unmount req
        struct statx pinned, visible;
        if (
            statx(dst.target_fd, "", AT_EMPTY_PATH, STATX_MNT_ID | STATX_INO, &pinned) < 0 ||
            statx(dst.parent_fd, dst.name, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID | STATX_INO, &visible) < 0
        ) {
            fprintf(stderr, "mount identity(%s): %s\n", dst_path, strerror(errno));
            goto out;
        }

        if (
            (pinned.stx_mask & (STATX_MNT_ID | STATX_INO)) != (STATX_MNT_ID | STATX_INO) ||
            (visible.stx_mask & (STATX_MNT_ID | STATX_INO)) != (STATX_MNT_ID | STATX_INO) ||
            pinned.stx_mnt_id != visible.stx_mnt_id || pinned.stx_ino != visible.stx_ino
        ) {
            fprintf(stderr, "destination(%s): mount changed during lookup\n", dst_path);
            goto out;
        }

        // umount2 takes pathname not mount fd
        // only resolve final component rel to pinned parent
        //  - avoids reliance on /proc/self/fd/N for correct mount
        // UNMOUNT_NOFOLLOW: stop swapped-in symlink being followed
        if (fchdir(dst.parent_fd) < 0) {
            fprintf(stderr, "fchdir parent(%s): %s\n", dst_path, strerror(errno));
            goto out;
        }

        if (umount2(dst.name, MNT_DETACH | UMOUNT_NOFOLLOW) < 0) {
            fprintf(stderr, "umount2(%s): %s\n", dst_path, strerror(errno));
            goto out;
        }
    }

    // verify curr visible dst not pinned target fd
    //  - after lazy unmount fd can refer to detached mount still
    // if other mount still present, track entry for retry
    struct statx parent, current;
    if (statx(dst.parent_fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &parent) < 0) {
        fprintf(stderr, "statx parent(%s): %s\n", dst_path, strerror(errno));
        goto out;
    }

    if (!(parent.stx_mask & STATX_MNT_ID)) {
        fprintf(stderr, "statx parent(%s): mount ID unavailable\n", dst_path);
        goto out;
    }

    if (statx(dst.parent_fd, dst.name, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &current) < 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "statx destination(%s): %s\n", dst_path, strerror(errno));
            goto out;
        }
    } else {
        if (!(current.stx_mask & STATX_MNT_ID)) {
            fprintf(stderr, "statx destination(%s): mount ID unavailable\n", dst_path);
            goto out;
        }

        if (parent.stx_mnt_id != current.stx_mnt_id) {
            fprintf(stderr, "destination(%s): still a mountpoint after detach\n", dst_path);
            goto out;
        }
    }

    // target fd only needed for init mount-id check
    close(dst.target_fd);
    dst.target_fd = -1;
    if (unlinkat(dst.parent_fd, dst.name, AT_REMOVEDIR) < 0 && errno != ENOENT) {
        fprintf(stderr, "rmdir(%s): %s\n", dst_path, strerror(errno));
        goto out;
    }

    fprintf(stderr, "removed %s\n", dst_path);
    result = 0;

out:
    close_destination(&dst);
    return result;
}

/*
* CLI usage:
*   add HOST_SOURCE CONTAINER_PID DEST -> create dst & inject mount
*   remove CONTAINER_PID DEST -> detach mount & remove dir
*/
int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "add"))
        return do_add(argv[2], argv[3], argv[4]);

    if (argc == 4 && !strcmp(argv[1], "remove"))
        return do_remove(argv[2], argv[3]);

    fprintf(stderr, "usage:\n  %s add HOST_SOURCE CONTAINER_PID DEST\n  %s remove CONTAINER_PID DEST\n", argv[0], argv[0]);
    return 2;
}
