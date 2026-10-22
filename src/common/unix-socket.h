/*
* Helpers for SOCK_SEQPACKET
*
* send/recv preserves msg boundaries.
* This layer rejects embedded NULs + oversized packets (avoids command/snapshot truncation).
* Errors as negative errno vals (consistent with sd-bus).
* connect/listen returns caller-owned fd; send/recv never close.
*/

#pragma once

#include <stddef.h>
#include <sys/types.h>

// returns fd >= 0 or -errno; path must fit sockaddr_un.sun_path
int fng_seqpacket_connect(const char *path);

// bind 600 fs socket; won't (attempt to) replace live listener/non-socket/socket owned by other user
int fng_seqpacket_listen(const char *path);

// send one text packet w/o NULs. 0 (success), failure (-errno).
int fng_seqpacket_send(int fd, const char *text);

/*
* Receive <= size-1 payload bytes & append NUL.
*
* Returns:
*   - payload length (0 --> clean EOF)
*   - -EMSGSIZE (truncation)
*   - -EBADMSG (embedded NUL)
*
* Buffer unusable on err.
*/
ssize_t fng_seqpacket_recv(int fd, char *buf, size_t size);
