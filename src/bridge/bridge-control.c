/* local control protocol w/ full-state snapshot sending */

#define _GNU_SOURCE
#include "bridge-internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

// Escape packet delims in fields so fields with "\t" and "\n" don't cause issues
static int append_state_field(char *state, size_t *used, const char *field) {
    for (const unsigned char *p = (const unsigned char *)field; *p; p++) {
        char escaped = *p == '\\' ? '\\' : *p == '\t' ? 't' : *p == '\n' ? 'n' : *p == '\r' ? 'r' : 0;

        size_t count = escaped ? 2 : 1;
        if (REMOVABLE_STATE_BUFSIZE - *used <= count)
            return -ENOSPC; // no space left

        if (escaped)
            state[(*used)++] = '\\';
        state[(*used)++] = escaped ? escaped : *p;
    }
    return 0;
}

/*
* Send REPLACEMENT snapshot (not incremental ev stream).
* Subscriber can reconnect & converge w/o past updates.
* "gone" objs discarded here.
* Non-blocking send; slow/dead sub dropped w/ proxy auto-reconnect.
*/
void notify_subscriber(void) {
    if (subscriber_fd < 0)
        return;

    char state[REMOVABLE_STATE_BUFSIZE];
    size_t used = 0;
    int r = 0;

    for (struct mount_entry *e = mounts; e; e = e->next) {
        if (e->gone)
            continue;

        const char *fields[] = {
                                e->info.object_path,
                                e->info.drive_path,
                                e->info.mount_path,
                                e->info.label,
                                e->info.uuid,
                                e->info.fstype
                                };

        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            r = append_state_field(state, &used, fields[i]);
            if (r < 0)
                goto fail;

            if (used >= sizeof(state)) {
                r = -ENOSPC;
                goto fail;
            }

            state[used++] = '\t';
        }

        int n = snprintf(state + used, sizeof(state) - used, "%d\t%d\t%d\t%d\n",
                            e->info.mounted, e->info.can_power_off, e->info.media_removable, e->info.ejectable);

        if (n < 0 || (size_t)n >= sizeof(state) - used) {
            r = -ENOSPC;
            goto fail;
        }

        used += n;
    }

    if (!used)
        state[used++] = '\n';

    ssize_t n;
    do {
        n = send(subscriber_fd, state, used, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);

    if (n >= 0 && (size_t)n == used)
        return;

    r = n < 0 ? -errno : -EIO;

fail:
    if (r != -EAGAIN && r != -EWOULDBLOCK)
        fprintf(stderr, "removable-bridge: dropping subscriber: %s\n", strerror(-r));
    fng_close_fd(&subscriber_fd);
}

// Validate min cmd format - known cmd prefix + absolute obj path.
// Actual object must be tracked before action.
static bool request_target(const char *request, const char *command, const char **target) {
    size_t n = strlen(command);
    if (strncmp(request, command, n) || request[n] != '/' || !request[n + 1])
        return false;

    *target = request + n;
    return true;
}

/*
* One accepted conn = persistent SUBSCRIBE stream/one-shot action or reply.
* Receive timeout to stop clients hogging thread.
* No arbitrary host paths read from reqs; only tracked D-Bus objs.
*/
void handle_control_connection(sd_bus *bus) {
    int fd = accept4(control_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno != EINTR)
            fprintf(stderr, "removable-bridge: accept: %s\n", strerror(errno));
        return;
    }

    // connected idle client shouldn't hold up UDisks
    struct timeval timeout = { .tv_sec = 2 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        goto out;

    char request[4096];
    if (fng_seqpacket_recv(fd, request, sizeof(request)) <= 0)
        goto out;

    if (!strcmp(request, REMOVABLE_CMD_SUBSCRIBE)) {
        fng_close_fd(&subscriber_fd);
        subscriber_fd = fd;
        fprintf(stderr, "removable-bridge: state subscriber connected\n");
        notify_subscriber();
        return;
    }

    fprintf(stderr, "removable-bridge: control request: %s\n", request);

    int r;
    const char *target;
    if (request_target(request, REMOVABLE_CMD_MOUNT, &target))
        r = request_mount(bus, target);

    else if (request_target(request, REMOVABLE_CMD_UNMOUNT, &target))
        r = request_unmount(bus, target);

    else if (request_target(request, REMOVABLE_CMD_EJECT, &target))
        r = request_drive_action(bus, target, true);

    else if (request_target(request, REMOVABLE_CMD_POWER_OFF, &target))
        r = request_drive_action(bus, target, false);

    else
        r = -EINVAL;

    fng_seqpacket_send(fd, r < 0 ? "ERR" : "OK");

out:
    fng_close_fd(&fd);
}
