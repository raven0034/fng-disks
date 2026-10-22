/*
* Host-side owner of removable-media lifecycle
*
* Host UDisks2 is still responsible for fs mount/unmount
*
* Bridge filters discoverable devices, injecting host mounts (excl. system) into
*   single container's mount namespace via mountinject
*
* State is mirrored over socket
*
* Important not to conflate:
*   - block device/fs in host UDisks
*   - its fs mounted on the host
*   - injected mount attached in the container
*   - bridge cleans up host mount *if* it was responsible for creating it
*/

#define _GNU_SOURCE
#include "bridge-internal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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


// session-wide state
struct mount_entry *mounts;
int subscriber_fd = -1;
static volatile sig_atomic_t stop_requested;
const char *container_pid_text;
int container_pidfd = -1;
const char *container_root;
const char *mountinject_path;
bool automount = true;
const char *control_socket_path;
int control_fd = -1;

// Only sets sig_atomic_t flag, other cleanup elsewhere
static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

/*
* Lifecycle:
*   - validate target PID
*   - acquire pidfd
*   - connect to host system bus
*   - register UDisks subs
*   - coldplug
*   - poll bus + ctrl listener + container pidfd
*   - release injected mounts on exit before closing resources
*
* Bus disconnection currently fatal.
*/
int main(int argc, char **argv) {
    if (argc != 5 && !(argc == 6 && !strcmp(argv[5], "--no-automount"))) {
        fprintf(stderr, "usage: %s CONTAINER_PID CONTAINER_MOUNT_ROOT MOUNTINJECT CONTROL_SOCKET [--no-automount]\n", argv[0]);
        return 2;
    }

    char *end;
    errno = 0;
    long pid = strtol(argv[1], &end, 10);
    if (errno || !*argv[1] || *end || pid <= 0 || (long)(pid_t)pid != pid) {
        fprintf(stderr, "removable-bridge: invalid container PID: %s\n", argv[1]);
        return 2;
    }

    container_pid_text = argv[1];
    container_root = argv[2];
    mountinject_path = argv[3];
    control_socket_path = argv[4];
    automount = argc == 5;

    // open pidfd in way safe for target Rocky 9 as well
    container_pidfd = syscall(SYS_pidfd_open, (pid_t)pid, 0);
    if (container_pidfd < 0) {
        fprintf(stderr, "removable-bridge: pidfd_open(%ld): %s\n", pid, strerror(errno));
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    sd_bus *bus = NULL;
    sd_bus_slot *change_slot = NULL, *property_slot = NULL;
    int exit_code = 1;

    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "removable-bridge: can't open system bus: %s\n", strerror(-r));
        goto out;
    }

    control_fd = fng_seqpacket_listen(control_socket_path);
    if (control_fd < 0) {
        fprintf(stderr, "removable-bridge: control socket: %s\n", strerror(-control_fd));
        goto out;
    }

    fprintf(stderr, "removable-bridge: control socket %s\n", control_socket_path);

    r = sd_bus_match_signal(bus, &change_slot, UDISKS_NAME, UDISKS_ROOT, IFACE_OBJECT_MANAGER, NULL, interfaces_changed, bus);
    if (r < 0) {
        fprintf(stderr, "removable-bridge: ObjectManager match failed: %s\n", strerror(-r));
        goto out;
    }

    r = sd_bus_add_match(bus, &property_slot,
                         "type='signal',sender='org.freedesktop.UDisks2',"
                         "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
                         "path_namespace='/org/freedesktop/UDisks2'",
                         properties_changed, bus);

    if (r < 0) {
        fprintf(stderr, "removable-bridge: PropertiesChanged match failed: %s\n", strerror(-r));
        goto out;
    }

    enumerate_existing(bus);
    fprintf(stderr, "removable-bridge: watching UDisks2 for container pid %s\n", container_pid_text);

    while (!stop_requested) {
        while ((r = sd_bus_process(bus, NULL)) > 0)
            ;

        if (r < 0) {
            fprintf(stderr, "removable-bridge: sd_bus_process: %s (%d)\n", strerror(-r), r);
            goto out;
        }

        struct pollfd fds[] = {
            { .fd = sd_bus_get_fd(bus), .events = sd_bus_get_events(bus) },
            { .fd = control_fd, .events = POLLIN },
            { .fd = container_pidfd, .events = POLLIN}
        };

        int pr = poll(fds, 3, have_pending_work() ? 2000 : -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;

            fprintf(stderr, "removable-bridge: poll: %s\n", strerror(errno));
            goto out;
        }

        if (fds[2].revents & POLLNVAL) {
            fprintf(stderr, "removable-bridge: invalid container pidfd\n");
            goto out;
        }
        if (fds[2].revents & (POLLIN | POLLHUP))
            break;

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "removable-bridge: system bus poll error (revents=0x%x)\n", fds[0].revents);
            goto out;
        }

        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "removable-bridge: control socket poll error (revents=0x%x)\n", fds[1].revents);
            goto out;
        }

        if (fds[1].revents & POLLIN)
            handle_control_connection(bus);

        if (have_pending_work())
            retry_pending_work(bus);
    }

    exit_code = 0;

out:
    if (cleanup_all(bus))
        exit_code = 1;

    fng_close_fd(&subscriber_fd);

    if (control_fd >= 0) {
        fng_close_fd(&control_fd);
        unlink(control_socket_path);
    }

    fng_close_fd(&container_pidfd);

    sd_bus_slot_unref(property_slot);
    sd_bus_slot_unref(change_slot);
    sd_bus_unref(bus);
    return exit_code;
}
