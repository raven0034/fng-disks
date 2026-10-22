/* Host UDisks2 coldplug, hotplug, property reconciliation and retries. */

#define _GNU_SOURCE
#include "bridge-internal.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


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

/*
* Discover candidate fs from host UDisks properties.
*
* Ignore sys/hidden devices & non-fs/non-removable blocks.
*
* --no-automount controls injection at point of discovery, not visibility.
*   - Effectively, flag decides whether or not to delegate automount to DE in container or not.
*/
void try_add_filesystem(sd_bus *bus, const char *object_path) {
    struct mount_entry **existing = find_entry(object_path);
    if (existing) {
        if (!(*existing)->gone || detach_container(*existing) < 0)
            return;

        // obj path recycled whilst detached mount pending
        remove_entry(existing);
    }

    int ignore = 0;
    int system = 0;
    int removable = 0;

    if (
        sd_bus_get_property_trivial(bus, UDISKS_NAME, object_path, IFACE_BLOCK, "HintIgnore", NULL, 'b', &ignore) < 0 ||
        sd_bus_get_property_trivial(bus, UDISKS_NAME, object_path, IFACE_BLOCK, "HintSystem", NULL, 'b', &system) < 0 || ignore || system
    )
        return;

    sd_bus_message *reply = NULL;
    const char *value;

    if (
        sd_bus_get_property(bus, UDISKS_NAME, object_path, IFACE_BLOCK, "IdUsage", NULL, &reply, "s") < 0 ||
        sd_bus_message_read(reply, "s", &value) <= 0 || strcmp(value, "filesystem")
    ) {
        sd_bus_message_unref(reply);
        return;
    }

    sd_bus_message_unref(reply);
    reply = NULL;

    const char *drive_path;
    if (
        sd_bus_get_property(bus, UDISKS_NAME, object_path, IFACE_BLOCK, "Drive", NULL, &reply, "o") < 0 ||
        sd_bus_message_read(reply, "o", &drive_path) <= 0 ||
        !strcmp(drive_path, "/") ||
        sd_bus_get_property_trivial(bus, UDISKS_NAME, drive_path, IFACE_DRIVE, "Removable", NULL, 'b', &removable) < 0 ||
        !removable
    ) {
        sd_bus_message_unref(reply);
        return;
    }

    struct mount_entry *e = calloc(1, sizeof(*e));
    if (!e) {
        sd_bus_message_unref(reply);
        goto out; // ENOMEM
    }

    e->info.object_path = strdup(object_path);
    e->info.drive_path = strdup(drive_path);
    sd_bus_message_unref(reply);

    if (!e->info.object_path || !e->info.drive_path)
        goto out; // ENOMEM

    const char *drive_props[] = { "CanPowerOff", "MediaRemovable", "Ejectable" };
    bool *drive_fields[] = { &e->info.can_power_off, &e->info.media_removable, &e->info.ejectable };
    for (size_t i = 0; i < 3; i++) {
        int value = 0;
        if (sd_bus_get_property_trivial(bus, UDISKS_NAME, e->info.drive_path, IFACE_DRIVE, drive_props[i], NULL, 'b', &value) >= 0)
            *drive_fields[i] = value;
    }

    const char *properties[] = { "IdLabel", "IdUUID", "IdType" };
    char **values[] = { &e->info.label, &e->info.uuid, &e->info.fstype };
    for (size_t i = 0; i < 3; i++) {
        if (sd_bus_get_property_string(bus, UDISKS_NAME, object_path, IFACE_BLOCK, properties[i], NULL, values[i]) < 0)
            *values[i] = strdup("");

        if (!*values[i])
            goto out; // ENOMEM
    }

    // don't discard discoverable fs because automounting/ns injection failed. keep avail for retry
    if (automount) {
        bool created = false;
        e->host_path = get_host_mount_path(bus, object_path, &created);
        if (e->host_path)
            update_host_mount_ownership(e, e->host_path, created);
    } else {
        // respect already-mounted fs. shouldn't be injected just because it was discovered
        int r = read_existing_mount_path(bus, object_path, &e->host_path);
        if (r < 0)
            goto out;
    }

    if (automount && !e->host_path) {
        // failed mount is recoverable; missing fs interface is not (yet) discoverable device ie check pre-publish
        char *existing_path = NULL;
        int r = read_existing_mount_path(bus, object_path, &existing_path);
        free(existing_path);

        if (r < 0)
            goto out;
    }

    e->info.mount_path = make_container_path(object_path, e->info.label, e->info.uuid);
    if (!e->info.mount_path)
        goto out; // ENOMEM

    if (automount && e->host_path) {
        fprintf(stderr, "removable-bridge: mounting %s\n  host:      %s\n  container: %s\n", object_path, e->host_path, e->info.mount_path);
        if (run_mountinject(e->host_path, e->info.mount_path) >= 0) {
            e->info.mounted = true;
            fprintf(stderr, "removable-bridge: injected %s -> %s\n", e->host_path, e->info.mount_path);
        } else {
            fprintf(stderr, "removable-bridge: injection failed for %s (will retry)\n", object_path);
            e->refresh_pending = true;
        }
    }

    // required allocations complete prior to obj publish
    e->next = mounts;
    mounts = e;
    fprintf(stderr, "removable-bridge: discovered %s host-mounted=%s container-mounted=%s\n",
            e->info.object_path, e->host_path ? "yes" : "no",
            e->info.mounted ? "yes" : "no");

    notify_subscriber();
    return;

out:
    if (e)
        remove_entry(&e);
}

// Driven by loss of block/fs interface
// Failed detach leaves hidden "gone" tombstone to try cleanup w/o telling container removed fs still exists.
static void remove_filesystem(const char *object_path) {
    struct mount_entry **pp = find_entry(object_path);
    if (!pp)
        return;

    struct mount_entry *e = *pp;
    e->gone = true;
    forget_host_mount_ownership(e);
    e->refresh_pending = false;
    if (detach_container(e) >= 0)
        remove_entry(pp);
    // keep entry on failure for detached mount retry, but don't expose host device not present from UDisks
    notify_subscriber();
}

bool have_pending_work(void) {
    for (struct mount_entry *e = mounts; e; e = e->next)
        if (e->gone || e->refresh_pending)
            return true;
    return false;
}

// Publish snapshot iff value changed for only mutable Block metadata.
static void refresh_metadata(sd_bus *bus, struct mount_entry *e) {
    const char *props[] = {
                            "IdLabel",
                            "IdUUID",
                            "IdType"
                        };

    char **fields[] = {
                        &e->info.label,
                        &e->info.uuid,
                        &e->info.fstype
                    };

    bool changed = false;
    for (size_t i = 0; i < 3; i++) {
        char *value = NULL;
        if (sd_bus_get_property_string(bus, UDISKS_NAME, e->info.object_path, IFACE_BLOCK, props[i], NULL, &value) < 0)
            continue;

        if (strcmp(*fields[i], value)) {
            free(*fields[i]);
            *fields[i] = value;
            changed = true;
        } else
            free(value);
    }

    if (changed)
        notify_subscriber();
}

/*
* Reconcile host MountPoints change.
* Host query failure != unmount --> preseve & retry later.
* --no-automount will not inject refreshed mounts automatically.
*/
static void refresh_host_mount(sd_bus *bus, struct mount_entry *e) {
    char *path = NULL;

    int r = read_existing_mount_path(bus, e->info.object_path, &path);
    if (r < 0) {
        e->refresh_pending = true;
        return; // unavail != unmounted
    }

    if (!automount && !e->info.mounted) {
        // mirror host mount meta, but not automount policy
        // host mount must remain unmounted in container until Mount()
        if (path)
            update_host_mount_ownership(e, path, false);
        else
            forget_host_mount_ownership(e);

        free(e->host_path);
        e->host_path = path;
        e->refresh_pending = false;
        return;
    }

    if (!path) {
        // other host component unmounted fs
        if (detach_container(e) < 0) {
            e->refresh_pending = true;
            return;
        }

        forget_host_mount_ownership(e);
        free(e->host_path);
        e->host_path = NULL;
        e->refresh_pending = false;
        return;
    }

    if (!e->info.mounted || !e->host_path || strcmp(path, e->host_path)) {
        // other host component mounted/moved fs
        if (detach_container(e) < 0 || run_mountinject(path, e->info.mount_path) < 0) {
            e->refresh_pending = true;
            free(path);
            return;
        }

        e->info.mounted = true;
        notify_subscriber();
    }

    update_host_mount_ownership(e, path, false);
    free(e->host_path);
    e->host_path = path;
    e->refresh_pending = false;
}

// Iterate point-to-pointer - removing "gone" entry changes curr link.
// Failed detach stays queued for later retry.
void retry_pending_work(sd_bus *bus) {
    for (struct mount_entry **pp = &mounts; *pp;) {
        struct mount_entry *e = *pp;

        if (e->gone) {
            if (detach_container(e) < 0) {
                pp = &e->next;
                continue;
            }

            remove_entry(pp);
        } else {
            if (e->refresh_pending)
                refresh_host_mount(bus, e);

            pp = &e->next;
        }
    }
}

/*
* PropertiesChanged signal identifies interface + obj path.
* Fetch curr vals from host UDisks rather than trust partial signal.
* Late property arrival can complete discovery of device whose InterfacesAdded came prematurely.
*/
int properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;
    sd_bus *bus = userdata;
    const char *interface;

    if (sd_bus_message_read(m, "s", &interface) <= 0)
        return 0;

    const char *path = sd_bus_message_get_path(m);
    if (!strcmp(interface, IFACE_DRIVE)) {
        const char *props[] = {
                                "CanPowerOff",
                                "MediaRemovable",
                                "Ejectable"
                            };

        int values[3] = {0};
        bool valid[3];

        for (size_t i = 0; i < 3; i++)
            valid[i] = sd_bus_get_property_trivial(bus, UDISKS_NAME, path, IFACE_DRIVE, props[i], NULL, 'b', &values[i]) >= 0;

        bool changed = false;
        for (struct mount_entry *e = mounts; e; e = e->next) {
            if (e->gone || strcmp(e->info.drive_path, path))
                continue;

            bool *fields[] = {
                                &e->info.can_power_off,
                                &e->info.media_removable,
                                &e->info.ejectable
                            };

            for (size_t i = 0; i < 3; i++) {
                if (valid[i] && *fields[i] != !!values[i]) {
                    *fields[i] = values[i];
                    changed = true;
                }
            }
        }
        if (changed)
            notify_subscriber();

        return 0;
    }

    struct mount_entry **pp = find_entry(path);
    if (!pp || (*pp)->gone) {
        // InterfacesAdded can precede IdUsage/Drive
        // later property update needs another chance to discover fs
        if (!strcmp(interface, IFACE_BLOCK) || !strcmp(interface, IFACE_FILESYSTEM))
            try_add_filesystem(bus, path);

        return 0;
    }
    if (!strcmp(interface, IFACE_BLOCK))
        refresh_metadata(bus, *pp);
    else if (!strcmp(interface, IFACE_FILESYSTEM))
        refresh_host_mount(bus, *pp);

    return 0;
}

/*
* ObjectManager signals relate to INTERFACES on objs.
* InterfacesRemoved removes only Partition/other unrelated interface.
* Not to be treated as deletion of fs until Block/Filesystem disappears.
*/
int interfaces_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;

    const char *object_path;

    int r = sd_bus_message_read(m, "o", &object_path);
    if (r < 0)
        return r;

    const char *member = sd_bus_message_get_member(m);
    if (!strcmp(member, "InterfacesAdded")) {
        try_add_filesystem(userdata, object_path);
    } else if (!strcmp(member, "InterfacesRemoved")) {
        // losing e.g. partition != fs disappearing
        r = sd_bus_message_enter_container(m, 'a', "s");
        if (r <= 0)
            return r;

        const char *interface;
        while ((r = sd_bus_message_read(m, "s", &interface)) > 0) {
            if (!strcmp(interface, IFACE_FILESYSTEM) || !strcmp(interface, IFACE_BLOCK)) {
                remove_filesystem(object_path);
                break;
            }
        }
        return r < 0 ? r : 0;
    }

    return 0;
}

// coldplug
void enumerate_existing(sd_bus *bus) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;

    int r = sd_bus_call_method(bus, UDISKS_NAME, UDISKS_ROOT "/Manager", IFACE_MANAGER, "GetBlockDevices", &error, &reply, "a{sv}", 0);
    if (r < 0) {
        fprintf(stderr, "removable-bridge: GetBlockDevices failed: %s\n", error.message ? error.message : strerror(-r));
    } else if (sd_bus_message_enter_container(reply, 'a', "o") > 0) {
        const char *object_path;
        size_t enumerated = 0;

        int read_r;
        while ((read_r = sd_bus_message_read(reply, "o", &object_path)) > 0) {
            enumerated++;
            try_add_filesystem(bus, object_path);
        }

        if (read_r < 0)
            fprintf(stderr, "removable-bridge: GetBlockDevices read failed: %s\n", strerror(-read_r));

        fprintf(stderr, "removable-bridge: startup enumerated %zu host block device(s)\n", enumerated);
    }

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
}
