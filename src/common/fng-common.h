/*
* shared defs
* consts are D-Bus names & object paths (not fs paths)
* procs use same name but on diff system buses
*   - bridge talks to actual UDisks2 on host
*   - proxy owns name in container
*   - no host system-bus socket passed to container
*
*/

#pragma once

#include <string.h>
#include <unistd.h>

#define UDISKS_NAME "org.freedesktop.UDisks2"
#define UDISKS_ROOT "/org/freedesktop/UDisks2"

#define IFACE_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define IFACE_MANAGER "org.freedesktop.UDisks2.Manager"
#define IFACE_BLOCK "org.freedesktop.UDisks2.Block"
#define IFACE_FILESYSTEM "org.freedesktop.UDisks2.Filesystem"
#define IFACE_DRIVE "org.freedesktop.UDisks2.Drive"

// ptr --> path; doesn't strip trailing /. Input UDisks obj path with final component as device ID
static inline const char *fng_path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// close & invalidate to avoid stale vals for if fd num reused (can result in double-close)
static inline void fng_close_fd(int *fd) {
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
