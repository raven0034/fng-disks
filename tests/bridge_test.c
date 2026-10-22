/* Host bridge logic regression tests w/ stubbed host UDisks & mountinject calls */
/* Doesn't validate live bus sigs or kernel mnt syscalls, just allows validation w/o needing actual hardware */

#define main removable_bridge_main
#include "../src/bridge/removable-bridge.c"
#include "../src/bridge/bridge-control.c"
#include "../src/bridge/bridge-mounts.c"
#include "../src/bridge/bridge-discovery.c"
#undef main
#include <assert.h>

struct sd_bus_message {
    int x;
};

static struct sd_bus_message message;

static int host_poweroff_calls;
static int host_unmount_calls;
static int host_eject_calls;
static int host_mount_calls;

static int property_error;
static int trivial_reads;
static int discovery_mode;

static const char *existing_host_mount;
static int poweroff_error;
static int unmount_error;
static const char *mount_method_path;
static bool mount_method_reply;

int sd_bus_get_property(
                        sd_bus *b, const char *service, const char *path, const char *iface, const char *prop,
                        sd_bus_error *error, sd_bus_message **reply, const char *signature
                    ) {
    (void)b;
    (void)service;
    (void)path;
    (void)iface;
    (void)prop;
    (void)error;
    (void)signature;

    if (property_error)
        return -EIO;

    *reply = &message;

    return 1;
}

int sd_bus_message_enter_container(sd_bus_message *m, char type, const char *s) {
    (void)m;
    (void)type;
    (void)s;

    return 1;
}

int sd_bus_message_read_array(sd_bus_message *m, char type, const void **buf, size_t *len) {
    (void)m;
    (void)type;

    if (!existing_host_mount)
        return 0;

    *buf = existing_host_mount;
    *len = strlen(existing_host_mount) + 1;

    return 1;
}
sd_bus_message *sd_bus_message_unref(sd_bus_message *m) {
    (void)m;

    return NULL;
}

const char *sd_bus_message_get_path(sd_bus_message *m) {
    (void)m;

    return "/org/freedesktop/UDisks2/block_devices/sdb2";
}

int sd_bus_message_read(sd_bus_message *m, const char *sig, ...) {
    (void)m;

    assert(!strcmp(sig, "s") || !strcmp(sig, "o"));

    va_list ap;
    va_start(ap, sig);

    const char **value=va_arg(ap, const char **);

    if (mount_method_reply && !strcmp(sig, "s")) {
        *value = mount_method_path;
        mount_method_reply = false;

        va_end(ap);

        return 1;
    }

    *value = discovery_mode ?
                (
                    !strcmp(sig, "o") ?
                    (
                        discovery_mode == 2 ?
                        "/org/freedesktop/UDisks2/drives/D1" :
                        "/org/freedesktop/UDisks2/drives/USB"
                    ) :
                    "filesystem"
                ) : IFACE_BLOCK;

    va_end(ap);

    return 1;
}
int sd_bus_get_property_trivial(
                                sd_bus *bus, const char *name, const char *path, const char *iface,
                                const char *property, sd_bus_error *error, char type, void *value
                            ) {
    (void)bus;
    (void)name;
    (void)path;
    (void)iface;
    (void)property;
    (void)error;
    (void)type;
    (void)value;

    trivial_reads++;
    if (discovery_mode) {
        *(int *)value = !strcmp(property, "Removable") || !strcmp(property, "CanPowerOff");
        return 1;
    }

    return -EIO;
}
int sd_bus_call_method(
                        sd_bus *b, const char *service, const char *path, const char *iface,
                        const char *method, sd_bus_error *err, sd_bus_message **reply, const char *sig, ...
                    ) {

    (void)b;
    (void)service;
    (void)path;
    (void)iface;
    (void)err;
    (void)reply;
    (void)sig;

    if (!strcmp(method, "PowerOff")) {
        host_poweroff_calls++;

        if (poweroff_error)
        return -EIO;
    }

    if (!strcmp(method, "Eject"))
        host_eject_calls++;

    if (!strcmp(method, "Unmount")) {
        host_unmount_calls++;

        if (unmount_error)
        return -EIO;
    }

    if (!strcmp(method, "Mount")) {
        host_mount_calls++;
        if (mount_method_path) {
            mount_method_reply = true;
            if (reply)
                *reply = &message;
       }
   }

   return 1;
}

int sd_bus_get_property_string(
                                sd_bus *b, const char *name, const char *path, const char *iface,
                                const char *prop, sd_bus_error *error, char **value
                            ) {

    (void)b;
    (void)name;
    (void)path;
    (void)iface;
    (void)prop;
    (void)error;
    (void)value;

    if (discovery_mode) {
        *value = strdup("test-volume");

        return *value ? 1 : -ENOMEM;
    }

    return -EIO;
}

void sd_bus_error_free(sd_bus_error *e) {
    (void)e;
}

int main(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

    subscriber_fd = fds[0];
    struct mount_entry *a = calloc(1, sizeof(*a));
    assert(a);

    a->info.object_path = strdup("/org/freedesktop/UDisks2/block_devices/sdb1");
    a->info.drive_path = strdup("/org/freedesktop/UDisks2/drives/D1");
    a->info.mount_path = strdup("/mnt/NAME");
    a->info.label = strdup("tab\tand\nnewline\\backslash");
    a->info.uuid = strdup("ID");
    a->info.fstype = strdup("vfat");
    a->info.mounted = false;
    a->info.can_power_off = true;
    a->info.media_removable = true;
    a->info.ejectable = true;

    mounts = a;
    notify_subscriber();
    char pkt[2048] = {0};

    assert(recv(fds[1], pkt, sizeof(pkt), 0) > 0);
    assert(strstr(pkt, "tab\\tand\\nnewline\\\\backslash"));
    assert(strstr(pkt, "\t0\t1\t1\t1\n"));
    assert(request_unmount(NULL, a->info.object_path) >= 0);
    assert(host_unmount_calls == 0); // already unmounted, skip redundant host call
    assert(request_drive_action(NULL, a->info.drive_path, true) >= 0 && host_eject_calls == 1);
    property_error = 1;

    // unowned fs doesn't need host D-Bus query to unmount
    assert(request_unmount(NULL, a->info.object_path) == 0);
    assert(host_unmount_calls == 0);
    property_error = 0;

    // retry transient host query failures rather than interpret as unmounted
    property_error = 1;
    refresh_host_mount(NULL, a);
    assert(a->refresh_pending && have_pending_work());

    property_error = 0;
    retry_pending_work(NULL);
    assert(!a->refresh_pending && !have_pending_work());

    // untracked block devs get rediscovered on late PropertiesChanged
    assert(properties_changed(&message, NULL, NULL) == 0 && trivial_reads == 1);

    // PowerOff reply != UDisks removal notif - if hardware enumerated --> must stay visible/remountable
    struct mount_entry *b = calloc(1, sizeof(*b));
    assert(b);

    b->info.object_path = strdup("/org/freedesktop/UDisks2/block_devices/sdb2");
    b->info.drive_path = strdup(a->info.drive_path);
    b->info.mount_path = strdup("/mnt/SECOND");
    b->info.label = strdup("SECOND");
    b->info.uuid = strdup("SECOND-ID");
    b->info.fstype = strdup("vfat");
    b->info.can_power_off = true;
    b->next = mounts;

    mounts = b;

    // keep tracked devices on host failure
    poweroff_error = 1;
    assert(request_drive_action(NULL, a->info.drive_path, false) < 0);
    assert(find_entry(a->info.object_path) && find_entry(b->info.object_path));
    poweroff_error = 0;

    // also keep if PowerOff succeeds but host still reports
    assert(request_drive_action(NULL, a->info.drive_path, false) >= 0);
    assert(host_poweroff_calls == 2 && mounts == b);
    assert(find_entry(a->info.object_path) && find_entry(b->info.object_path));
    assert(request_unmount(NULL, a->info.object_path) >= 0);
    assert(request_drive_action(NULL, a->info.drive_path, false) >= 0);
    assert(host_poweroff_calls == 3);

    // don't set false empty snapshot (Plasma should still see vol)
    assert(recv(fds[1], pkt, sizeof(pkt), MSG_DONTWAIT) < 0 && (errno==EAGAIN || errno==EWOULDBLOCK));

    // vanished mnt fs sends one removal snapshot. successful detach shouldn't emit second (identical) snapshot
    struct mount_entry *removed_mounted = calloc(1, sizeof(*removed_mounted));
    assert(removed_mounted);
    removed_mounted->info.object_path = strdup("/org/freedesktop/UDisks2/block_devices/vanished");
    removed_mounted->info.drive_path = strdup(a->info.drive_path);
    removed_mounted->info.mount_path = strdup("/mnt/VANISHED");
    removed_mounted->info.label = strdup("VANISHED");
    removed_mounted->info.uuid = strdup("");
    removed_mounted->info.fstype = strdup("vfat");
    removed_mounted->info.mounted = true;
    removed_mounted->next = mounts;
    mounts = removed_mounted;

    int exited[2];
    assert(pipe(exited) == 0);
    assert(write(exited[1], "x", 1) == 1);
    container_pidfd = exited[0];
    remove_filesystem("/org/freedesktop/UDisks2/block_devices/vanished");
    assert(!find_entry("/org/freedesktop/UDisks2/block_devices/vanished"));
    ssize_t snapshot_len = recv(fds[1], pkt, sizeof(pkt) - 1, MSG_DONTWAIT);
    assert(snapshot_len > 0);
    pkt[snapshot_len] = '\0';
    assert(!strstr(pkt, "block_devices/vanished"));
    assert(recv(fds[1], pkt, sizeof(pkt), MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    close(exited[0]);
    close(exited[1]);
    container_pidfd = -1;

    // POLLNVAL on a bad pidfd != container has exited
    int invalid_pipe[2];
    assert(pipe(invalid_pipe) == 0);
    container_pidfd = invalid_pipe[0];
    close(invalid_pipe[0]);
    close(invalid_pipe[1]);
    assert(run_mountinject(NULL, "/tmp/fng-should-not-run") == -EBADF);
    container_pidfd = -1;

    // new partitions on still-enum'd drive can also be discovered
    automount = false;
    container_root = "/tmp/fng-bridge-tests";
    discovery_mode = 2;
    try_add_filesystem(NULL, "/org/freedesktop/UDisks2/block_devices/new_partition");
    assert(find_entry("/org/freedesktop/UDisks2/block_devices/new_partition"));
    discovery_mode = 0;
    remove_filesystem("/org/freedesktop/UDisks2/block_devices/new_partition");

    // actual UDisks removal sigs are authoritative, only sim'd here
    remove_filesystem("/org/freedesktop/UDisks2/block_devices/sdb1");
    assert(!find_entry("/org/freedesktop/UDisks2/block_devices/sdb1") && find_entry(b->info.object_path));
    remove_filesystem("/org/freedesktop/UDisks2/block_devices/sdb2");
    assert(!mounts);
    assert(request_drive_action(NULL, "/org/freedesktop/UDisks2/drives/D1", false)==-ENOENT);

    // consume removal snapshots before next test
    while (recv(fds[1], pkt, sizeof(pkt), MSG_DONTWAIT) > 0) {}

    // with bridge automount disabled, unmounted UDisks fs must still be published without calling host Filesystem.Mount
    automount = false;
    container_root = "/tmp/fng-bridge-tests";
    discovery_mode = 1;
    existing_host_mount = "/run/media/host/already-mounted";
    try_add_filesystem(NULL, "/org/freedesktop/UDisks2/block_devices/sdd1");
    assert(mounts && !mounts->info.mounted && mounts->host_path && !strcmp(mounts->host_path, existing_host_mount));
    assert(!mounts->refresh_pending);
    existing_host_mount = "/run/media/host/moved";
    refresh_host_mount(NULL, mounts);
    assert(!mounts->info.mounted && !mounts->refresh_pending && !strcmp(mounts->host_path, existing_host_mount));
    assert(host_mount_calls == 0);
    remove_filesystem(mounts->info.object_path);
    assert(!mounts);

    // only owned after our own UDisks Mount suceeds
    char directory[] = "/tmp/fng-ownership-XXXXXX";
    assert(mkdtemp(directory));
    mount_method_path = directory;
    existing_host_mount = NULL;
    bool created = false;
    char *result = get_host_mount_path(NULL, "/org/freedesktop/UDisks2/block_devices/sdx1", &created);
    assert(result && created && !strcmp(result, directory));
    free(result);
    existing_host_mount = directory;
    result = get_host_mount_path(NULL, "/org/freedesktop/UDisks2/block_devices/sdx1", &created);
    assert(result && !created && !strcmp(result, directory));
    free(result);
    mount_method_path = NULL;

    // sim shutdown: owned mount unmounted, pre-existing not
    struct mount_entry *owned = calloc(1, sizeof(*owned));
    struct mount_entry *preexisting = calloc(1, sizeof(*preexisting));
    assert(owned && preexisting);

    owned->info.object_path = strdup("/org/freedesktop/UDisks2/block_devices/sdx1");
    owned->info.mount_path = strdup("/tmp/fng-ownership-mounted");
    owned->host_path = strdup(directory);
    owned->info.drive_path = strdup("/org/freedesktop/UDisks2/drives/Test1");
    owned->info.label = strdup("");
    owned->info.uuid = strdup("");
    owned->info.fstype = strdup("vfat");
    update_host_mount_ownership(owned, directory, true);
    assert(owned->host_mounted_by_bridge && owned->host_mount_id);
    preexisting->info.object_path = strdup("/org/freedesktop/UDisks2/block_devices/sdx2");
    preexisting->info.mount_path = strdup("/tmp/fng-preexisting");
    preexisting->info.drive_path = strdup("/org/freedesktop/UDisks2/drives/Test2");
    preexisting->info.label = strdup("");
    preexisting->info.uuid = strdup("");
    preexisting->info.fstype = strdup("vfat");
    preexisting->host_path = strdup(directory);
    preexisting->next = owned;
    mounts = preexisting;

    // fake exited container so run_mountinject takes no-ns-left path (ew to forking podman during unit tests lol)
    int pid_pipe[2];
    assert(pipe(pid_pipe) == 0);
    assert(write(pid_pipe[1], "x", 1) == 1);
    container_pidfd = pid_pipe[0];
    int prior_unmount_calls = host_unmount_calls;
    assert(cleanup_all(NULL) == 0);
    container_pidfd = -1;
    close(pid_pipe[0]);
    close(pid_pipe[1]);
    assert(!mounts && host_unmount_calls == prior_unmount_calls + 1);

    // don't unmount other fs sub'd at same pathname
    struct mount_entry changed = { .host_path = directory };
    changed.info.object_path = "/org/freedesktop/UDisks2/block_devices/sdx1";
    changed.host_mounted_by_bridge = true;
    uint64_t id;
    assert(read_host_mount_id(directory, &id) == 0);
    changed.host_mount_id = id + 1;
    prior_unmount_calls = host_unmount_calls;
    assert(cleanup_host_mount(NULL, &changed) == 0);
    assert(host_unmount_calls == prior_unmount_calls && !changed.host_mounted_by_bridge);

    // failed host unmount is error, not to be silently treated as clean
    changed.host_mounted_by_bridge = true;
    changed.host_mount_id = id;
    unmount_error = 1;
    assert(cleanup_host_mount(NULL, &changed) < 0);
    assert(changed.host_mounted_by_bridge);
    unmount_error = 0;
    assert(cleanup_host_mount(NULL, &changed) >= 0);
    assert(!changed.host_mounted_by_bridge);

    // successful host mount + failed container inj --> host still owned & to be cleaned up
    struct mount_entry *failed_injection = calloc(1, sizeof(*failed_injection));
    assert(failed_injection);
    failed_injection->info.object_path = strdup("/org/freedesktop/UDisks2/block_devices/sdx3");
    failed_injection->info.mount_path = strdup("/tmp/fng-failed-injection");
    failed_injection->info.drive_path = strdup("/org/freedesktop/UDisks2/drives/Test3");
    failed_injection->info.label = strdup("");
    failed_injection->info.uuid = strdup("");
    failed_injection->info.fstype = strdup("vfat");

    int exited_pipe[2];
    assert(pipe(exited_pipe) == 0);
    assert(write(exited_pipe[1], "x", 1) == 1);
    container_pidfd = exited_pipe[0];
    existing_host_mount = NULL;
    mount_method_path = directory;
    assert(mount_entry(NULL, failed_injection) < 0);
    assert(!failed_injection->info.mounted && failed_injection->host_mounted_by_bridge);
    assert(failed_injection->host_path && !strcmp(failed_injection->host_path, directory));
    existing_host_mount = directory;
    mounts = failed_injection;
    prior_unmount_calls = host_unmount_calls;
    assert(cleanup_all(NULL) == 0);
    assert(host_unmount_calls == prior_unmount_calls + 1 && !mounts);
    container_pidfd = -1;
    close(exited_pipe[0]);
    close(exited_pipe[1]);
    mount_method_path = NULL;

    // explicit Unmount to honour same ownership policy as shutdown. sim container mount & sep-mounted host fs
    int manual_pid[2];
    assert(pipe(manual_pid) == 0);
    assert(write(manual_pid[1], "x", 1) == 1);
    container_pidfd = manual_pid[0]; // the container has exited: mock detach
    existing_host_mount = directory;
    struct mount_entry manual_host = {
        .info = {
                    .object_path = "/org/freedesktop/UDisks2/block_devices/sdy1",
                    .mount_path = "/tmp/fng-manual-host",
                    .mounted = true
                },

        .host_path = strdup(directory)
    };

    assert(manual_host.host_path);
    prior_unmount_calls = host_unmount_calls;
    assert(unmount_entry(NULL, &manual_host) == 0);
    assert(!manual_host.info.mounted && !manual_host.host_path);
    assert(host_unmount_calls == prior_unmount_calls);

    // original host mount avail for remount
    created = true;
    result = get_host_mount_path(NULL, manual_host.info.object_path, &created);
    assert(result && !created && !strcmp(result, directory));
    free(result);

    // must clean as per shutdown
    struct mount_entry manual_owned = {
        .info = {
                    .object_path = "/org/freedesktop/UDisks2/block_devices/sdy2",
                    .mount_path = "/tmp/fng-manual-owned",
                    .mounted = true
                },

        .host_path = strdup(directory)
    };

    assert(manual_owned.host_path);
    update_host_mount_ownership(&manual_owned, directory, true);
    assert(manual_owned.host_mounted_by_bridge);
    prior_unmount_calls = host_unmount_calls;
    assert(unmount_entry(NULL, &manual_owned) == 0);
    assert(!manual_owned.info.mounted && !manual_owned.host_path && !manual_owned.host_mounted_by_bridge);
    assert(host_unmount_calls == prior_unmount_calls + 1);

    // failed host query/unmount post-detach retains ownership + orig path so a req can retry
    struct mount_entry retry = {
        .info = {
                    .object_path = "/org/freedesktop/UDisks2/block_devices/sdy3",
                    .mount_path = "/tmp/fng-manual-retry",
                    .mounted = true
                },

        .host_path = strdup(directory)
    };

    assert(retry.host_path);
    update_host_mount_ownership(&retry, directory, true);
    property_error = 1;
    assert(unmount_entry(NULL, &retry) < 0);
    assert(!retry.info.mounted && retry.host_mounted_by_bridge && retry.host_path);
    property_error = 0;
    unmount_error = 1;
    assert(unmount_entry(NULL, &retry) < 0);
    assert(retry.host_mounted_by_bridge && retry.host_path);
    unmount_error = 0;
    prior_unmount_calls = host_unmount_calls;
    assert(unmount_entry(NULL, &retry) == 0);
    assert(host_unmount_calls == prior_unmount_calls + 1);
    assert(!retry.host_mounted_by_bridge && !retry.host_path);
    container_pidfd = -1;
    close(manual_pid[0]);
    close(manual_pid[1]);

    assert(rmdir(directory) == 0);
    existing_host_mount = NULL;

    close(fds[0]);close(fds[1]);
    puts("bridge tests passed");
    return 0;
}
