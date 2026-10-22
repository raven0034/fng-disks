/* UDisks2 object tree + D-Bus method & properties */

#define _GNU_SOURCE
#include "disks-internal.h"

#include <errno.h>
#include <stddef.h>
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

#define INFO_FIELD(field) offsetof(struct device, info.field)

/*
* Open separate tmp socket for user action (ie not state SUBSCRIBE socket).
* Returns 0 (host "OK"), or transport "ERR" response to D-Bus method err visible to desktop.
* Host decides whether ref'd obj can be acted upon.
*/
static int bridge_request(const char *command, const char *object_path, sd_bus_error *ret_error) {
    char request[4096];
    char reply[4];

    int n = snprintf(request, sizeof(request), "%s%s", command, object_path);
    if (n < 0 || (size_t)n >= sizeof(request))
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS, "Removable bridge request too long");

    int fd = fng_seqpacket_connect(socket_path);
    if (fd < 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "Host removable bridge unavailable: %s", strerror(-fd));

    int r = fng_seqpacket_send(fd, request);
    if (r >= 0) {
        ssize_t received = fng_seqpacket_recv(fd, reply, sizeof(reply));
        r = received < 0 ? (int)received : received == 0 ? -EPIPE : 0;
    }

    fng_close_fd(&fd);
    if (r < 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "Host removable bridge unavailable: %s", strerror(-r));

    if (strcmp(reply, "OK"))
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "Host removable bridge rejected %s", command);

    return 0;
}

/*
* vtable handler for Unmount, Eject & PowerOff.
* userdataL offset pointer to object_path/drive_path field.
* Member names choose restricted wire cmd (ie not arbitrary).
*/
static int bridge_action(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    const char *member = sd_bus_message_get_member(m);
    const char *command = !strcmp(member, "PowerOff") ? REMOVABLE_CMD_POWER_OFF :
                          !strcmp(member, "Eject") ? REMOVABLE_CMD_EJECT : REMOVABLE_CMD_UNMOUNT;
    int r = bridge_request(command, *(char * const *)userdata, ret_error);
    return r < 0 ? r : sd_bus_reply_method_return(m, NULL);
}

/*
* sd-bus fallback: only advertise path in current snapshot.
*   - Drive: search by drive_path
*   - Block/Filesystem: search by object_path
*/
static int find_object(sd_bus *bus_unused, const char *path, const char *interface, void *userdata, void **ret_found, sd_bus_error *ret_error) {
    (void)bus_unused;
    (void)userdata;
    (void)ret_error;

    struct device *d = find_device(path, !strcmp(interface, IFACE_DRIVE));
    if (!d)
        return 0;

    *ret_found = d;
    return 1;
}

// D-Bus Property Helpers
static int append_byte_string(sd_bus_message *reply, const char *value) {
    // UDisks represents fs/dev paths as byte arrs, terminating NUL incl conventionally
    return sd_bus_message_append_array(reply, 'y', value, strlen(value) + 1);
}

// MountPoints has type "aay": array of byte arrays (NUL-term POSIX path strings)
// Empty "aay" --> not mounted in container (even if pre-existing host mount recorded by bridge)
static int mountpoints_property(sd_bus *bus_unused, const char *path, const char *interface, const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error) {
    (void)bus_unused;
    (void)path;
    (void)interface;
    (void)property;
    (void)ret_error;

    struct device *d = userdata;
    int r = sd_bus_message_open_container(reply, 'a', "ay");
    if (r >= 0 && d->info.mounted)
        r = append_byte_string(reply, d->info.mount_path);
    if (r >= 0)
        r = sd_bus_message_close_container(reply);

    return r;
}

/*
* Provide stripped down set of UDisks props for Solid.
* Several identity props are simplified ie not guaranteed hw attrs.
    - Not going for full UDisks2 emu lmao
* CanPowerOff=false override to stop container Solid from auto-poweroff bog standard USBs post-unmount
*   - physical poweroff separate op
*/
static int export_property(sd_bus *bus_unused, const char *path, const char *interface, const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error) {
    (void)bus_unused;
    (void)path;
    (void)ret_error;

    struct device *d = userdata;
    if (!strcmp(property, "Device") || !strcmp(property, "PreferredDevice")) {
        char *devnode = NULL;
        if (asprintf(&devnode, "/dev/%s", fng_path_basename(d->info.object_path)) < 0)
            return -ENOMEM;

        int r = append_byte_string(reply, devnode);
        free(devnode);
        return r;
    }

    if (!strcmp(interface, IFACE_BLOCK)) {
        if (!strcmp(property, "HintSystem") || !strcmp(property, "HintIgnore") || !strcmp(property, "HintAuto") || !strcmp(property, "HintPartitionable") || !strcmp(property, "ReadOnly")) {
            return sd_bus_message_append(reply, "b", !strcmp(property, "HintAuto"));
        }

        const char *value;
        if (!strcmp(property, "Id"))
            value = *d->info.uuid ? d->info.uuid : fng_path_basename(d->info.object_path);
        else if (!strcmp(property, "IdUsage"))
            value = "filesystem";
        else if (!strcmp(property, "HintIconName"))
            value = "drive-removable-media";
        else if (!strcmp(property, "HintSymbolicIconName"))
            value = "drive-removable-media-symbolic";
        else
            value = "";

        return sd_bus_message_append(reply, "s", value);
    }

    if (!strcmp(property, "Removable") || !strcmp(property, "MediaAvailable") ||
        !strcmp(property, "MediaChangeDetected") || !strcmp(property, "CanPowerOff") ||
        !strcmp(property, "MediaRemovable") || !strcmp(property, "Ejectable")) {
        // override CanPowerOff to keep USB available for remounting
        bool value = !strcmp(property, "CanPowerOff") ? false :
                     !strcmp(property, "MediaRemovable") ? d->info.media_removable :
                     !strcmp(property, "Ejectable") ? d->info.ejectable : true;
        return sd_bus_message_append(reply, "b", value);
    }

    const char *value;
    if (!strcmp(property, "Model") || !strcmp(property, "Id"))
        value = fng_path_basename(d->info.drive_path);
    else if (!strcmp(property, "ConnectionBus"))
        value = "usb";
    else
        value = "";

    return sd_bus_message_append(reply, "s", value);
}

// successful mount returns container path
// bridge changes mounted state & subsequent snapshot updated MountPoints for other clients
static int filesystem_mount(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    struct device *d = userdata;
    if (!d->info.mounted) {
        int r = bridge_request(REMOVABLE_CMD_MOUNT, d->info.object_path, ret_error);
        if (r < 0)
            return r;
    }
    return sd_bus_reply_method_return(m, "s", d->info.mount_path);
}

// D-Bus VTables
/*
* sd-dbus vtables define D-Bus ABI (method i/o sigs and prop types)
* "ay" --> device-path bytes
* "aay" --> array of mount-path bytes arrays
* "o" --> obj path
* Sigs should be kept compat with Solid, not just here.
* SD_BUS_VTABLE_UNPRIVILEGED permits callers of container bus service
*   - host bridge/UDisks still enforce actual op boundary
*/
static const sd_bus_vtable block_vtable[] = {
    SD_BUS_VTABLE_START(0),

    SD_BUS_PROPERTY("Device", "ay", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PreferredDevice", "ay", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),

    SD_BUS_PROPERTY("Id", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IdUsage", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IdType", "s", NULL, INFO_FIELD(fstype), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IdVersion", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IdLabel", "s", NULL, INFO_FIELD(label), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IdUUID", "s", NULL, INFO_FIELD(uuid), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

    SD_BUS_PROPERTY("Drive", "o", NULL, INFO_FIELD(drive_path), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("HintSystem", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HintIgnore", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HintAuto", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HintPartitionable", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ReadOnly", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HintName", "s", NULL, INFO_FIELD(label), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("HintIconName", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HintSymbolicIconName", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),

    SD_BUS_VTABLE_END
};

static const sd_bus_vtable filesystem_vtable[] = {
    SD_BUS_VTABLE_START(0),

    SD_BUS_METHOD_WITH_OFFSET("Mount", "a{sv}", "s", filesystem_mount, 0, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_OFFSET("Unmount", "a{sv}", "", bridge_action, INFO_FIELD(object_path), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("MountPoints", "aay", mountpoints_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

    SD_BUS_VTABLE_END
};

static const sd_bus_vtable drive_vtable[] = {
    SD_BUS_VTABLE_START(0),

    SD_BUS_METHOD_WITH_OFFSET("PowerOff", "a{sv}", "", bridge_action, INFO_FIELD(drive_path), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_OFFSET("Eject", "a{sv}", "", bridge_action, INFO_FIELD(drive_path), SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_PROPERTY("Vendor", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Model", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Revision", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Serial", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("WWN", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Id", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ConnectionBus", "s", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),

    SD_BUS_PROPERTY("Removable", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MediaRemovable", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("MediaAvailable", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MediaChangeDetected", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Ejectable", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanPowerOff", "b", export_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),

    SD_BUS_VTABLE_END
};

// Manager
// One Block+Filesystem obj exported per fs, but one drive obj per unique drive_path (incl. multi-partition drives).
// Caller owns NULL-term'd array and strdup'd strings. sd-bus frees them per node-enum API contract.
static int enumerate_nodes(sd_bus *bus_unused, const char *prefix, void *userdata, char ***ret_nodes, sd_bus_error *ret_error) {
    (void)bus_unused;
    (void)prefix;
    (void)userdata;
    (void)ret_error;

    size_t count = 0;
    for (struct device *d = devices; d; d = d->next)
        count++;

    char **nodes = calloc(count * 2 + 1, sizeof(*nodes));
    if (!nodes)
        return -ENOMEM;

    size_t n = 0;
    for (struct device *d = devices; d; d = d->next) {
        nodes[n] = strdup(d->info.object_path);
        if (!nodes[n++])
            goto oom;

        if (find_device(d->info.drive_path, true) != d)
            continue;

        nodes[n] = strdup(d->info.drive_path);
        if (!nodes[n++])
            goto oom;
    }

    *ret_nodes = nodes;
    return 1;

oom:
    while (n)
        free(nodes[--n]);

    free(nodes);
    return -ENOMEM;
}

struct manager_export {
    const char *version;
    char **filesystems;
};

// min advertised mgr prop for compat
// list is descriptive - host UDisks2 & kernel drivers control actual mount support
static char *supported_filesystems[] = {
    "vfat", "exfat", "ntfs", "ntfs3",
    "ext4", "xfs", "btrfs", "iso9660",
    NULL
};

static struct manager_export manager = {
    .version = "fng-disks-0.1",
    .filesystems = supported_filesystems
};

static const sd_bus_vtable manager_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "s", NULL, offsetof(struct manager_export, version), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedFilesystems", "as", NULL, offsetof(struct manager_export, filesystems), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

int register_udisks_objects(sd_bus *instance) {
    int r;

    if (
        (r = sd_bus_add_object_vtable(instance, NULL, UDISKS_ROOT "/Manager", IFACE_MANAGER, manager_vtable, &manager)) < 0 ||
        (r = sd_bus_add_object_manager(instance, NULL, UDISKS_ROOT)) < 0 ||
        (r = sd_bus_add_fallback_vtable(instance, NULL, UDISKS_ROOT "/block_devices", IFACE_BLOCK, block_vtable, find_object, NULL)) < 0 ||
        (r = sd_bus_add_fallback_vtable(instance, NULL, UDISKS_ROOT "/block_devices", IFACE_FILESYSTEM, filesystem_vtable, find_object, NULL)) < 0 ||
        (r = sd_bus_add_fallback_vtable(instance, NULL, UDISKS_ROOT "/drives", IFACE_DRIVE, drive_vtable, find_object, NULL)) < 0 ||
        (r = sd_bus_add_node_enumerator(instance, NULL, UDISKS_ROOT, enumerate_nodes, NULL)) < 0
    )
        return r;
    return 0;
}
