/*
* (Deliberately) Private wire contract between bridge (host) and fng-disks (container)
*
* SOCK_SEQPACKET:
*   - Message contains one payload
*   - Commands are ASCII prefixes followed by UDisks obj path
*       - No delim other than leading /
*   - SUBSCRIBE --> no suffix; receives full state snapshots
*   - Bridge returns OK or ERR to an action on connection
*
* Socket lives in user runtime dir.
*/

#pragma once

#include <stdbool.h>

#define REMOVABLE_CMD_SUBSCRIBE  "SUBSCRIBE"
#define REMOVABLE_CMD_MOUNT      "MOUNT"
#define REMOVABLE_CMD_UNMOUNT    "UNMOUNT"
#define REMOVABLE_CMD_EJECT      "EJECT"
#define REMOVABLE_CMD_POWER_OFF  "POWER_OFF"

// max in-mem snapshot packet (incl receiver NUL)
// too large state rejected (not partially applied)
// applies to full device list, not one fs
#define REMOVABLE_STATE_BUFSIZE  65536

// one fs/partition rather than drive
struct removable_info {
    char *object_path;
    char *drive_path;
    char *mount_path;
    char *label;
    char *uuid;
    char *fstype;
    // refers to injected mount
    bool mounted;
    bool can_power_off;
    bool media_removable;
    bool ejectable;
};
