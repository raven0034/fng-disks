/* Management of state snapshots */

#define _GNU_SOURCE
#include "disks-internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

struct device *find_in(struct device *head, const char *path, bool by_drive) {
    for (struct device *d = head; d; d = d->next) {
        const char *candidate = by_drive ? d->info.drive_path : d->info.object_path;

        if (!strcmp(candidate, path))
            return d;
    }

    return NULL;
}

struct device *find_device(const char *path, bool by_drive) {
    return find_in(devices, path, by_drive);
}

// parse whole snapshot before touching exported obj tree
// malformed/partial packet must not remove prev published devices
static bool unescape_field(char *value) {
    char *write = value;
    for (char *read = value; *read; read++) {
        if (*read != '\\') {
            *write++ = *read;
            continue;
        }

        read++;
        if (!*read)
            return false;

        switch (*read) {
            case '\\': *write++ = '\\'; break;
            case 't': *write++ = '\t'; break;
            case 'n': *write++ = '\n'; break;
            case 'r': *write++ = '\r'; break;
            default: return false;
        }
    }

    *write = '\0';
    return true;
}

/*
* Deserialise single tab-sep record into one allocation.
* Record copied into storage and split in-place.
* Field ptrs borrow said storage until freed.
* All fields to be validated before publishing (obj-path syntax, abs mnt paths, and 0/1 flags).
* Returns null on malformed record/alloc failure.
*/
static struct device *parse_device(char *record) {
    size_t len = strlen(record) + 1;
    struct device *d = calloc(1, sizeof(*d) + len);
    if (!d)
        return NULL;

    char *p = (char *)(d + 1);
    memcpy(p, record, len);
    d->info.object_path = strsep(&p, "\t");
    d->info.drive_path = strsep(&p, "\t");
    d->info.mount_path = strsep(&p, "\t");
    d->info.label = strsep(&p, "\t");
    d->info.uuid = strsep(&p, "\t");
    d->info.fstype = strsep(&p, "\t");
    char *mounted = strsep(&p, "\t");
    char *can_power_off = strsep(&p, "\t");
    char *media_removable = strsep(&p, "\t");
    char *ejectable = strsep(&p, "\t");

    if (!d->info.object_path || !d->info.drive_path || !d->info.mount_path ||
        !d->info.label || !d->info.uuid || !d->info.fstype ||
        !mounted || !can_power_off || !media_removable || !ejectable || p ||
        !unescape_field(d->info.object_path) || !unescape_field(d->info.drive_path) ||
        !unescape_field(d->info.mount_path) || !unescape_field(d->info.label) ||
        !unescape_field(d->info.uuid) || !unescape_field(d->info.fstype) ||
        sd_bus_object_path_is_valid(d->info.object_path) != 1 ||
        sd_bus_object_path_is_valid(d->info.drive_path) != 1 ||
        d->info.mount_path[0] != '/' ||
        strlen(mounted) != 1 || (mounted[0] != '0' && mounted[0] != '1') ||
        strlen(can_power_off) != 1 || (can_power_off[0] != '0' && can_power_off[0] != '1') ||
        strlen(media_removable) != 1 || (media_removable[0] != '0' && media_removable[0] != '1') ||
        strlen(ejectable) != 1 || (ejectable[0] != '0' && ejectable[0] != '1')) {
        free(d);
        return NULL;
    }

    d->info.mounted = mounted[0] == '1';
    d->info.can_power_off = can_power_off[0] == '1';
    d->info.media_removable = media_removable[0] == '1';
    d->info.ejectable = ejectable[0] == '1';
    return d;
}

// free complete parsed lists. also used as rollback if snapshot malformed.
void free_devices(struct device *head) {
    while (head) {
        struct device *next = head->next;
        free(head);
        head = next;
    }
}

/*
* Apply single complete snapshot:
*   - parse tmp list
*   - reject dupes & malformed fields
*   - then swap published lists
*   - publish D-Bus add/remove/PropertiesChanged events comparing old & new
*       - ie then clients update view w/o object recreation
*
* Empty snapshot removes all objs (e.g. sub loss).
* O(n^2) list scan sub-optimal but acceptable for expected small set.
*
*/
int apply_state(char *state) {
    struct device *updated = NULL;
    char *save = NULL;
    for (char *line = strtok_r(state, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        struct device *d = parse_device(line);
        if (!d || find_in(updated, d->info.object_path, false)) {
            free(d);
            free_devices(updated);
            fprintf(stderr, "fng-disks: ignoring invalid state snapshot\n");
            return -EINVAL;
        }
        d->next = updated;
        updated = d;
    }

    struct device *old = devices;
    devices = updated;

    for (struct device *d = old; d; d = d->next) {
        if (!find_device(d->info.object_path, false)) {
            sd_bus_emit_object_removed(bus, d->info.object_path);
            fprintf(stderr, "fng-disks: removed %s\n", d->info.object_path);
        }
        if (!find_device(d->info.drive_path, true) && find_in(old, d->info.drive_path, true) == d)
            sd_bus_emit_object_removed(bus, d->info.drive_path);
    }

    for (struct device *d = devices; d; d = d->next) {
        struct device *previous = find_in(old, d->info.object_path, false);
        struct device *old_drive = find_in(old, d->info.drive_path, true);
        if (find_device(d->info.drive_path, true) == d) {
            if (!old_drive)
                sd_bus_emit_object_added(bus, d->info.drive_path);
            else if (old_drive->info.media_removable != d->info.media_removable ||
                     old_drive->info.ejectable != d->info.ejectable)
                sd_bus_emit_properties_changed(bus, d->info.drive_path, IFACE_DRIVE, "MediaRemovable", "Ejectable", NULL);
        }

        if (!previous) {
            sd_bus_emit_object_added(bus, d->info.object_path);
            fprintf(stderr, "fng-disks: added %s mount=%s label=%s type=%s\n", d->info.object_path, d->info.mount_path, d->info.label, d->info.fstype);
            continue;
        }
        if (strcmp(previous->info.mount_path, d->info.mount_path) || previous->info.mounted != d->info.mounted)
            sd_bus_emit_properties_changed(bus, d->info.object_path, IFACE_FILESYSTEM, "MountPoints", NULL);

        if (strcmp(previous->info.drive_path, d->info.drive_path) ||
            strcmp(previous->info.label, d->info.label) ||
            strcmp(previous->info.uuid, d->info.uuid) ||
            strcmp(previous->info.fstype, d->info.fstype))
            sd_bus_emit_properties_changed(bus, d->info.object_path, IFACE_BLOCK, "Id", "IdLabel", "IdUUID", "IdType", "HintName", "Drive", NULL);
    }

    free_devices(old);
    return 0;
}
