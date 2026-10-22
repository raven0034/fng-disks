/* Host filesystem lifecycle, container mount injection and ownership. */

#define _GNU_SOURCE
#include "bridge-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

// Collision detection (incl. pending/gone entries)
// Failed detach retains dst until mount cleared
static bool container_path_in_use(const char *path) {
    for (struct mount_entry *e = mounts; e; e = e->next) {
        if (!strcmp(e->info.mount_path, path))
            return true;
    }

    return false;
}

// Derive human-friendly container mount point from label/UUID/device name.
// Sanitised since fs labels untrusted.
// Collision suffix handles same-labels partitions + normalisation collisions.
char *make_container_path(const char *object_path, const char *label, const char *uuid) {
    const char *base = fng_path_basename(object_path);
    const char *source = *label ? label : *uuid ? uuid : *base ? base : "removable";
    char *path = NULL;

    if (asprintf(&path, "%s/%s", container_root, source) < 0)
        return NULL;

    char *name = path + strlen(container_root) + 1;
    for (char *p = name; *p; p++) {
        unsigned char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            *p = '_';
    }

    // fs label "."/".." should not select mount root/parent
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        name[0] = '_';

    if (!container_path_in_use(path))
        return path;

    // diff labels can normalise to same name/other fallback
    const char *suffix = *base ? base : "device";

    for (unsigned n = 0;; n++) {
        char *candidate = NULL;
        int r = n ? asprintf(&candidate, "%s_%s_%u", path, suffix, n) : asprintf(&candidate, "%s_%s", path, suffix);

        if (r < 0)
            break;

        if (!container_path_in_use(candidate)) {
            free(path);
            return candidate;
        }
        free(candidate);
    }

    free(path);
    return NULL;
}

/*
* Actual host UDisks2 call.
* 'a{sv}',0 constructs empty options map.
* Host service/polkit dependent for authorising action
*/
static int udisks_call(sd_bus *bus, const char *object_path, const char *interface, const char *method) {
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = sd_bus_call_method(bus, UDISKS_NAME, object_path, interface, method, &error, NULL, "a{sv}", 0);
    if (r < 0)
        fprintf(stderr, "removable-bridge: %s(%s) failed: %s\n", method, object_path, error.message ? error.message : strerror(-r));

    sd_bus_error_free(&error);
    return r;
}

// Unlink matching entry, lookup by fs object path
struct mount_entry **find_entry(const char *object_path) {
    for (struct mount_entry **pp = &mounts; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->info.object_path, object_path))
            return pp;
    }
    return NULL;
}

// unlink and free entry (no unmount)
void remove_entry(struct mount_entry **pp) {
    struct mount_entry *e = *pp;
    *pp = e->next;

    free(e->info.object_path);
    free(e->info.drive_path);
    free(e->info.mount_path);
    free(e->info.label);
    free(e->info.uuid);
    free(e->info.fstype);
    free(e->host_path);
    free(e);
}

/*
* Single-use helper to run mountinject over "podman unshare".
*
* pidfd --> can't use PID reuse as live container check
*   - on exit, ADD --> forbidden, REMOVE --> succeeds (no-op)
*   - ns already torn down hence ^^
*
* Synchronous child wait.
* Nonzero child exit reported as -EIO with dx via helper's stderr.
*/
int run_mountinject(const char *source, const char *destination) {
    bool add = source != NULL;
    struct pollfd pfd = { .fd = container_pidfd, .events = POLLIN };
    int r;

    do {
        r = poll(&pfd, 1, 0);
    } while (r < 0 && errno == EINTR);

    if (r < 0)
        return -errno;

    if (r > 0) {
        // dead container --> treat as detached
        // bad pidfd != container exited
        if (pfd.revents & POLLNVAL)
            return -EBADF;

        if (pfd.revents & (POLLIN | POLLHUP))
            return add ? -ESRCH : 0;

        return -EIO;
    }

    pid_t child = fork();
    if (child < 0)
        return -errno;

    if (!child) {
        if (add)
            execlp("podman", "podman", "unshare", mountinject_path, "add", source, container_pid_text, destination, (char *)NULL);
        else
            execlp("podman", "podman", "unshare", mountinject_path, "remove", container_pid_text, destination, (char *)NULL);
        _exit(127);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -errno;
    }

    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
}

// Returns 1 (mount), 0 (no mount), <0 (D-Bus/parse error).
// "not mounted" distinct from "could not check" on safely removing.
int read_existing_mount_path(sd_bus *bus, const char *object_path, char **ret) {
    sd_bus_message *reply = NULL;
    *ret = NULL;

    int r = sd_bus_get_property(bus, UDISKS_NAME, object_path, IFACE_FILESYSTEM, "MountPoints", NULL, &reply, "aay");
    if (r < 0)
        goto out;

    r = sd_bus_message_enter_container(reply, 'a', "ay");
    if (r <= 0) {
        r = r < 0 ? r : -EBADMSG;
        goto out;
    }

    const void *data;
    size_t size;

    r = sd_bus_message_read_array(reply, 'y', &data, &size);
    if (r <= 0)
        goto out; // zero = empty MountPoints array

    if (!size) {
        r = -EBADMSG;
        goto out;
    }

    const char *bytes = data;
    size_t len = bytes[size - 1] == '\0' ? size - 1 : size;
    if (!len || bytes[0] != '/' || memchr(bytes, '\0', len)) {
        r = -EBADMSG;
        goto out;
    }

    *ret = strndup(bytes, len);
    r = *ret ? 1 : -ENOMEM;

out:
    sd_bus_message_unref(reply);
    return r;
}

/*
* Prefer existing MountPoints where possible.
* "*created" only true if Mount() returned usable path, not just concurrent mount was discovered.
* Return str caller-owned; uncertain ownership should not mean host-unmount of fs (for consistency)
*/
char *get_host_mount_path(sd_bus *bus, const char *object_path, bool *created) {
    *created = false;
    char *out = NULL;

    int check = read_existing_mount_path(bus, object_path, &out);
    if (check < 0) {
        // failed property query != unmount fs
        // don't Mount() on already disconnected bus
        fprintf(stderr, "removable-bridge: MountPoints(%s) failed: %s (%d)\n", object_path, strerror(-check), check);
        return NULL;
    }

    if (out) {
        fprintf(stderr, "removable-bridge: %s already mounted at %s\n", object_path, out);
        return out;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *path;

    int r = sd_bus_call_method(bus, UDISKS_NAME, object_path, IFACE_FILESYSTEM, "Mount", &error, &reply, "a{sv}", 0);
    if (r >= 0)
        r = sd_bus_message_read(reply, "s", &path);
    if (r > 0) {
        out = strdup(path);
        if (out)
            *created = true;
    }

    if (!out) {
        // conc mount may succeed despite failed Mount() reply
        // don't hide bus/meta failure behind apparently empty MountPoints
        int fallback = read_existing_mount_path(bus, object_path, &out);
        if (fallback > 0)
            fprintf(stderr, "removable-bridge: %s became mounted at %s\n", object_path, out);
        else {
            fprintf(stderr, "removable-bridge: Mount(%s) failed: %s (r=%d, D-Bus error=%s)\n", object_path,
                    error.message ? error.message : strerror(-r), r, error.name ? error.name : "<none>");

            if (fallback < 0)
                fprintf(stderr, "removable-bridge: follow-up MountPoints(%s) failed: %s (%d)\n", object_path, strerror(-fallback), fallback);
        }
    }

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return out;
}

// Mount id stronger than mount path (latter reusable by unrelated host-side mount).
// STATX_MNT_ID provided by 5.8+.
static int read_host_mount_id(const char *path, uint64_t *ret) {
    struct statx st;
    if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &st) < 0)
        return -errno;

    if (!(st.stx_mask & STATX_MNT_ID))
        return -EOPNOTSUPP;

    *ret = st.stx_mnt_id;
    return 0;
}

// Call when id changes/is unknowable. Intentionally one-way (don't infer owner from pathname).
void forget_host_mount_ownership(struct mount_entry *e) {
    e->host_mounted_by_bridge = false;
    e->host_mount_id = 0;
}

// Preserve owner claim iff same mount still present. Failure to id --> don't unmount.
void update_host_mount_ownership(struct mount_entry *e, const char *path, bool created) {
    uint64_t id = 0;
    int r = read_host_mount_id(path, &id);

    if (r < 0) {
        if (created)
            fprintf(stderr, "removable-bridge: unable to track new host mount %s: %s (will leave it mounted at shutdown)\n", path, strerror(-r));

        forget_host_mount_ownership(e);
    } else if (created) {
        e->host_mounted_by_bridge = true;
        e->host_mount_id = id;
    } else if (e->host_mounted_by_bridge && (!e->host_path || strcmp(e->host_path, path) || e->host_mount_id != id))
        forget_host_mount_ownership(e);
}

/*
* Called only after:
*   - container mount detached
*   - container exited
*
* Don't touch host/ext replaced mounts.
*
* Returns 0 when host mount cleaned up, negative for failed host check/UDisks call.
* Failure retains owner claim ie retry possible.
* Changed identity not retried (not our mount any more).
*   - mount ID not persistent across bridge restarts
*/
int cleanup_host_mount(sd_bus *bus, struct mount_entry *e) {
    if (!e->host_mounted_by_bridge || e->gone)
        return 0;

    char *current = NULL;
    int r = read_existing_mount_path(bus, e->info.object_path, &current);
    if (r < 0) {
        fprintf(stderr, "removable-bridge: can't inspect FNG-created host mount %s: %s\n", e->info.object_path, strerror(-r));
        return r;
    }

    if (r == 0) {
        forget_host_mount_ownership(e);
        return 0;
    }

    uint64_t id = 0;
    r = read_host_mount_id(current, &id);

    if (r < 0 || !e->host_path || strcmp(current, e->host_path) || id != e->host_mount_id) {
        fprintf(stderr, "removable-bridge: preserving changed host mount for %s\n", e->info.object_path);
        free(current);

        forget_host_mount_ownership(e);

        return 0;
    }
    free(current);

    fprintf(stderr, "removable-bridge: unmounting FNG-created host filesystem %s\n", e->info.object_path);

    r = udisks_call(bus, e->info.object_path, IFACE_FILESYSTEM, "Unmount");
    if (r >= 0)
        forget_host_mount_ownership(e);

    return r;
}

// Detach injected (not host) mount. mounted=false reported iff mountinject reports success.
// After detach, still known UDisks obj, keep tracked for possible future mounts (and driver alive for PowerOff)
int detach_container(struct mount_entry *e) {
    if (!e->info.mounted)
        return 0;

    int r = run_mountinject(NULL, e->info.mount_path);
    if (r < 0) {
        fprintf(stderr, "removable-bridge: detach failed for %s\n", e->info.mount_path);
        return r;
    }

    e->info.mounted = false;
    // removed obj already hidden by removal notif. retrying shouldn't emit dupe snapshot
    if (!e->gone)
        notify_subscriber();
    return 0;
}

/*
* On-demand mount (also used by proxy's Filesystem.Mount).
* Preserve host path + ownership if failed injection, still need to clean-up host-side.
* "mounted" only set after ns helper succeeds.
*/
int mount_entry(sd_bus *bus, struct mount_entry *e) {
    if (e->info.mounted)
        return 0;

    bool created = false;
    char *path = get_host_mount_path(bus, e->info.object_path, &created);
    if (!path)
        return -EIO;

    update_host_mount_ownership(e, path, created);
    // clean up successfully created host mount even if ns inject fails
    free(e->host_path);
    e->host_path = path;

    int r = run_mountinject(path, e->info.mount_path);
    if (r < 0) {
        fprintf(stderr, "removable-bridge: injection failed for %s\n", e->info.object_path);
        return r;
    }

    e->info.mounted = true;
    e->refresh_pending = false;

    notify_subscriber();

    return 0;
}

/*
* Used by Filesystem.Unmount & drive actions.
* Detach container first; unmount host fs iff bridge owns mount (o/w remains avail on host).
* Non-owned host mounts --> removal of drive not inherently safe in these cases.
* (Worth noting that no-one should hit this edge case under intended use.)
*/
int unmount_entry(sd_bus *bus, struct mount_entry *e) {
    // unmount --> cancel pending reinject
    e->refresh_pending = false;

    int r = detach_container(e);
    if (r < 0)
        return r;

    // same ownership boundary as shutdown cleanup
    if (!e->host_mounted_by_bridge && e->host_path)
        fprintf(stderr, "removable-bridge: preserving pre-existing host mount %s\n", e->host_path);

    r = cleanup_host_mount(bus, e);
    if (r < 0)
        return r;

    free(e->host_path);
    e->host_path = NULL;

    forget_host_mount_ownership(e);

    e->refresh_pending = false;

    return 0;
}

int request_mount(sd_bus *bus, const char *object_path) {
    struct mount_entry **pp = find_entry(object_path);
    return pp && !(*pp)->gone ? mount_entry(bus, *pp) : -ENOENT;
}

int request_unmount(sd_bus *bus, const char *object_path) {
    struct mount_entry **pp = find_entry(object_path);
    return pp && !(*pp)->gone ? unmount_entry(bus, *pp) : -ENOENT;
}

/*
* Drive actions affect all partitions sharing drive_path. Handled separately from fs Mount/Unmount.
* Unmount all known partitions first (& checks host UDisks advertises relevant capability).
*
* Successful PowerOff/Eject reply not proof of device disappearance.
*   - Only UDisks obj-removal sigs determine when to hide records
*   - Don't force-hide from Plasma
*/
int request_drive_action(sd_bus *bus, const char *drive_path, bool eject) {
    bool found = false, permitted = false;
    for (struct mount_entry *e = mounts; e; e = e->next) {
        if (e->gone || strcmp(e->info.drive_path, drive_path))
            continue;

        found = true;
        if (eject ? (e->info.media_removable || e->info.ejectable) : e->info.can_power_off)
            permitted = true;
    }
    if (!found)
        return -ENOENT;
    if (!permitted)
        return -EOPNOTSUPP;

    // must unmount all partitions on drive before eject/power-off
    // keep still-present fs discoverable
    // successful host reply != proof UDisks removed device
    for (struct mount_entry *e = mounts; e; e = e->next) {
        if (e->gone || strcmp(e->info.drive_path, drive_path))
            continue;

        int r = unmount_entry(bus, e);
        if (r < 0)
            return r;
    }

    const char *method = eject ? "Eject" : "PowerOff";
    int r = udisks_call(bus, drive_path, IFACE_DRIVE, method);
    if (r < 0)
        return r;

    fprintf(stderr, "removable-bridge: %s %s\n", method, drive_path);
    return 0;
}

// best-effort shutdown (can't recover ownership post-SIGKILL/crash/reboot)
int cleanup_all(sd_bus *bus) {
    int failures = 0;

    while (mounts) {
        struct mount_entry *e = mounts;
        // attempt removal even for entries marked unmounted
        // previous failed injs may have left mount not in our state
        int r = run_mountinject(NULL, e->info.mount_path);
        if (r < 0) {
            fprintf(stderr, "removable-bridge: failed to detach %s during shutdown; leaving host mount untouched\n", e->info.mount_path);
            failures++;
        } else {
            e->info.mounted = false;
            if (cleanup_host_mount(bus, e) < 0)
                failures++;
        }

        remove_entry(&mounts);
    }

    return failures;
}
