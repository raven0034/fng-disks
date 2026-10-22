/* transport (listen, connect, send, recv) */

#include "unix-socket.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// platform-specific fixed path lim; fail rather than truncate socket name
static int make_addr(struct sockaddr_un *addr, const char *path) {
    if (strlen(path) >= sizeof(addr->sun_path))
        return -ENAMETOOLONG;

    *addr = (struct sockaddr_un){ .sun_family = AF_UNIX };
    strcpy(addr->sun_path, path);
    return 0;
}

int fng_seqpacket_connect(const char *path) {
    struct sockaddr_un addr;
    int r = make_addr(&addr, path);
    if (r < 0)
        return r;

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        r = -errno;
        close(fd);
        return r;
    }

    return fd;
}

/*
* Stale socket recovery conservative (intentional).
* bind() detects existing pathname.
* ECONNREFUSED on socket belonging to our UID --> qualifies for unlink/rebind
*   - only this case
*/
int fng_seqpacket_listen(const char *path) {
    struct sockaddr_un addr;
    int r = make_addr(&addr, path);
    if (r < 0)
        return r;

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        r = -errno;
        if (r != -EADDRINUSE)
            goto fail;

        // don't take pathname of live bridge
        // only recover stale socket owned by curr user in their runtime dir
        struct stat st;
        if (lstat(path, &st) < 0 || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid())
            goto fail;

        int probe = fng_seqpacket_connect(path);

        if (probe >= 0) {
            close(probe);
            goto fail;
        }

        if (probe != -ECONNREFUSED || unlink(path) < 0)
            goto fail;

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            r = -errno;
            goto fail;
        }
    }

    if (chmod(path, 0600) < 0 || listen(fd, 8) < 0) {
        r = -errno;
        unlink(path);
        goto fail;
    }

    return fd;

fail:
    close(fd);
    return r;
}

// SOCK_SEQPACKET normally send entire record atomically
// short send --> error (multi-call --> multi-record)
// MSG_NOSIGNAL --> stop disconnected client killing proc
int fng_seqpacket_send(int fd, const char *text) {
    size_t len = strlen(text);
    ssize_t n;

    do {
        n = send(fd, text, len, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    if (n < 0)
        return -errno;

    return (size_t)n == len ? 0 : -EIO;
}

ssize_t fng_seqpacket_recv(int fd, char *buf, size_t size) {
    if (!buf || !size)
        return -EINVAL;

    ssize_t n;

    do {
        // MSG_TRUNC reports the packet length rather than silently accepting a prefix.
        n = recv(fd, buf, size - 1, MSG_TRUNC);
    } while (n < 0 && errno == EINTR);

    if (n < 0)
        return -errno;

    if ((size_t)n >= size)
        return -EMSGSIZE;

    if (memchr(buf, '\0', (size_t)n))
        return -EBADMSG;

    buf[n] = '\0';
    return n;
}
