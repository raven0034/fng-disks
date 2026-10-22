/*
* Private host-bridge ifaces.
* Globals describe one container session, accessed from event loop.
* Method/state lifetime + ownership rules kept in bridge-mounts.c.
*/

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-bus.h>
#include "../common/fng-common.h"
#include "../common/removable-protocol.h"
#include "../common/unix-socket.h"

/*
* Single tracked UDisks fs obj. Persists after unmount.
* UDisks obj may privately persist (gone) until mount can be detached.
*
* host_path: host UDisks mount if known (only host_mounted_by_bridge implies responsibility for mount)
*/
struct mount_entry {
    struct removable_info info;
    char *host_path;
    // only successful UDisks Mount() by bridge --> cleanup ownership
    bool host_mounted_by_bridge;
    uint64_t host_mount_id;
    // UDisks removed obj; retain only whilst detach needs retry
    bool gone;
    // failed to reconcile ext host mount change
    bool refresh_pending;
    struct mount_entry *next;
};

extern struct mount_entry *mounts;
extern int subscriber_fd;
extern const char *container_pid_text;
extern int container_pidfd;
extern const char *container_root;
extern const char *mountinject_path;
extern bool automount;
extern const char *control_socket_path;
extern int control_fd;

// mount state + ownership + ops
struct mount_entry **find_entry(const char *object_path);
void remove_entry(struct mount_entry **pp);
int run_mountinject(const char *source, const char *destination);
int read_existing_mount_path(sd_bus *bus, const char *object_path, char **ret);
char *get_host_mount_path(sd_bus *bus, const char *object_path, bool *created);
void forget_host_mount_ownership(struct mount_entry *e);
void update_host_mount_ownership(struct mount_entry *e, const char *path, bool created);
int cleanup_host_mount(sd_bus *bus, struct mount_entry *e);
int detach_container(struct mount_entry *e);
int mount_entry(sd_bus *bus, struct mount_entry *e);
int unmount_entry(sd_bus *bus, struct mount_entry *e);
int request_mount(sd_bus *bus, const char *object_path);
int request_unmount(sd_bus *bus, const char *object_path);
int request_drive_action(sd_bus *bus, const char *drive_path, bool eject);
char *make_container_path(const char *object_path, const char *label, const char *uuid);
int cleanup_all(sd_bus *bus);

// host UDisks discovery + updates + retry
void try_add_filesystem(sd_bus *bus, const char *object_path);
bool have_pending_work(void);
void retry_pending_work(sd_bus *bus);
int properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
int interfaces_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
void enumerate_existing(sd_bus *bus);

// socket control + state sub
void notify_subscriber(void);
void handle_control_connection(sd_bus *bus);
