/* local socket transport tests, incl msg framing & stale listener safety. no requirements of actual UDisks service or USB device */

#define _GNU_SOURCE
#include "../src/common/unix-socket.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    char directory[] = "/tmp/fng-socket-test-XXXXXX";
    assert(mkdtemp(directory));

    char path[256];
    assert(snprintf(path, sizeof(path), "%s/control.sock", directory) < (int)sizeof(path));

    // invalid buffers & pathname lengths to fail before any socket activity
    assert(fng_seqpacket_recv(-1, NULL, 4) == -EINVAL);

    char empty[1];
    assert(fng_seqpacket_recv(-1, empty, 0) == -EINVAL);

    char too_long[300];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    assert(fng_seqpacket_connect(too_long) == -ENAMETOOLONG);
    assert(fng_seqpacket_listen(too_long) == -ENAMETOOLONG);

    // stale socket recovery must not touch regular files
    char other_path[256];
    assert(snprintf(other_path, sizeof(other_path), "%s/not-a-socket", directory) < (int)sizeof(other_path));

    FILE *regular = fopen(other_path, "w");
    assert(regular);
    assert(fputs("keep", regular) >= 0);
    assert(fclose(regular) == 0);
    assert(fng_seqpacket_listen(other_path) == -EADDRINUSE);
    assert(access(other_path, F_OK) == 0);
    assert(unlink(other_path) == 0);

    int server = fng_seqpacket_listen(path);
    assert(server >= 0);

    // second instance shouldn't unlink live listener pathname
    assert(fng_seqpacket_listen(path) == -EADDRINUSE);

    // dupe-instance probe --> brief connect --> close
    int probe = accept(server, NULL, NULL);
    assert(probe >= 0);
    close(probe);

    int client = fng_seqpacket_connect(path);
    assert(client >= 0);

    int accepted = accept(server, NULL, NULL);
    assert(accepted >= 0);

    assert(fng_seqpacket_send(client, "hello") == 0);
    char buf[8];
    assert(fng_seqpacket_recv(accepted, buf, sizeof(buf)) == 5);
    assert(!strcmp(buf, "hello"));

    char embedded_nul[] = { 'A', '\0', 'B' };
    assert(send(client, embedded_nul, sizeof(embedded_nul), 0) == (ssize_t)sizeof(embedded_nul));
    assert(fng_seqpacket_recv(accepted, buf, sizeof(buf)) == -EBADMSG);

    assert(send(client, "0123456789", 10, 0) == 10);
    assert(fng_seqpacket_recv(accepted, buf, sizeof(buf)) == -EMSGSIZE);

    close(client);
    close(accepted);
    close(server);

    // crashed server may leave socket, recover only this stale case
    server = fng_seqpacket_listen(path);
    assert(server >= 0);
    close(server);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
    puts("socket tests passed");
    return 0;
}
