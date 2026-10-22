/* fake UDisks ifaces */

#pragma once
#include <stdbool.h>
#include <systemd/sd-bus.h>
#include "../common/fng-common.h"
#include "../common/removable-protocol.h"
#include "../common/unix-socket.h"

// str field in info point to same alloc as struct (whereas e.g. host mount_entry is indiv owned)
// don't free separately, free_devices() release complete record once
struct device {
    struct removable_info info;
    struct device *next;
};
extern const char *socket_path;
extern struct device *devices;
extern sd_bus *bus;

// state snapshot parsing & reconciliation
struct device *find_in(struct device *head, const char *path, bool by_drive);
struct device *find_device(const char *path, bool by_drive);
void free_devices(struct device *head);
int apply_state(char *state);

int register_udisks_objects(sd_bus *bus);
