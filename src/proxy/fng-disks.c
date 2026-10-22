
/*
* My word, is that a fake UDisks2? Uh... maybe...
*
* KDE Solid talks to our proc via the container system bus like it would the real UDisks2.
*
* Hardware discovery & mount authority remains with host bridge (the source of truth):
*   - This proc only presents snapshots & forwards impl D-Bus actions
*   - Lost subscription --> clears stale export objects --> new SUBSCRIBE receives complete state again
*   - IT IS NOT A COMPLETE UDISKS2 IMPLEMENTATION FOR OUR USE CASE (I only have so much time in this world)
*/

#define _GNU_SOURCE
#include "disks-internal.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
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

// proc-wide state
const char *socket_path;
struct device *devices;
sd_bus *bus;

/*
* Register exported obj tree before owning bus name.
* Then poll both D-Bus and host sub.
* Post-disconnect: clear objects & reconnect w/ full snapshot.
* 1s poll timeout while disconnected as simple retry policy.
*/
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s REMOVABLE_SOCKET\n", argv[0]);
        return 2;
    }

    socket_path = argv[1];
    int subscription_fd = -1;

    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "fng-disks: cannot open system bus: %s\n", strerror(-r));
        goto out;
    }

    if ((r = register_udisks_objects(bus)) < 0) {
        fprintf(stderr, "fng-disks: object registration failed: %s\n", strerror(-r));
        goto out;
    }

    r = sd_bus_request_name(bus, UDISKS_NAME, 0);
    if (r < 0) {
        fprintf(stderr, "fng-disks: cannot own %s: %s\n", UDISKS_NAME, strerror(-r));
        goto out;
    }

    fprintf(stderr, "fng-disks: owns %s\nfng-disks: bridge=%s\n", UDISKS_NAME, socket_path);
    for (;;) {
        while ((r = sd_bus_process(bus, NULL)) > 0)
            ;

        if (r < 0) {
            fprintf(stderr, "fng-disks: sd_bus_process: %s\n", strerror(-r));
            goto out;
        }

        if (subscription_fd < 0) {
            subscription_fd = fng_seqpacket_connect(socket_path);
            if (subscription_fd >= 0 && fng_seqpacket_send(subscription_fd, REMOVABLE_CMD_SUBSCRIBE) < 0)
                fng_close_fd(&subscription_fd);

            if (subscription_fd >= 0)
                fprintf(stderr, "fng-disks: subscribed to removable bridge\n");
        }

        struct pollfd fds[] = {
            {
                .fd = sd_bus_get_fd(bus),
                .events = sd_bus_get_events(bus)
            },
            {
                .fd = subscription_fd,
                .events = POLLIN | POLLHUP | POLLERR
            }
        };

        int pr = poll(fds, 2, subscription_fd >= 0 ? -1 : 1000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;

            fprintf(stderr, "fng-disks: poll: %s\n", strerror(errno));
            goto out;
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "fng-disks: system bus poll error (revents=0x%x)\n", fds[0].revents);
            goto out;
        }

        if (subscription_fd >= 0 && (fds[1].revents & POLLIN)) {
            char state[REMOVABLE_STATE_BUFSIZE];

            ssize_t n = fng_seqpacket_recv(subscription_fd, state, sizeof(state));

            if (n <= 0) {
                fng_close_fd(&subscription_fd);
                char empty[] = "\n";
                apply_state(empty);
                continue;
            }

            apply_state(state);
        }

        if (subscription_fd >= 0 && (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            fng_close_fd(&subscription_fd);
            char empty[] = "\n";
            apply_state(empty);
        }
    }

out:
    fng_close_fd(&subscription_fd);
    free_devices(devices);
    sd_bus_unref(bus);
    return 1;
}
