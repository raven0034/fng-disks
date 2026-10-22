# fng-disks

A narrow host UDisks2 bridge --> minimal substitute UDisks2 service for removable media injection across mount namespace boundaries in rootless Podman container.

*Intended use case is in [UWCS' FNG container](https://github.com/uwcs/fng) running KDE (or derivative e.g. SonicDE) Plasma, running in unprivileged Rocky 9/10 environment on DCS Lab PCs/similar. May have usage value beyond this, does not come with any guarantees of this. Not a full UDisks replacement.*

## Features
- Mounting
- Unmounting (container & host, w/ device poweroff possible)
- Remounting
- Optional automounting (bridge-side)
- Device discovery (coldplug & hotplug)
- Device clean-up on shutdown

## Purpose
The FNG container is a rootless Podman container run through our shared DCS account. Block-device discovery and filesystem mounting are the responsibility of the host, and as such, make it a... *royal nightmare* for someone to rock up to FNG with a USB and pull up a pre-downloaded game ~~or OpenPlanet configs & mods for Trackmania they don't want to redownload every week~~.

Whilst there's limited scope in which `udisksctl` could be used to mount a USB *prior* to container startup, there is no way of safely unmounting within the container, nor can the USB be remounted trivially within the mount namespace of the container once removed (even with `/run/media` bound, the container may see the *directory* for the/a mount being created, but won't receive the mount itself). Even then, we're not giving FNG-goers access to a virtual console on our shared account to do this.

The ideal scenario is that (within reason ~~and my sanity~~), FNG-goers can show up, plug their removable storage in, and have a "close enough" experience to that of Plasma natively (showing up correctly in the Disks & Devices UI, being able to mount & unmount safely within the container, and not need to do anything "involved" to get their USB working).

However, we also don't want to blindly mount *everything* from the host into the container, else it removes many of the isolation boundaries achieved by stopping usage of Distrobox (where, incidentally, the same & more limitations (aimed to be) mitigated by this project also apply).

## Requirements

- Host UDisks2 must remain the authority for mounting, unmounting and other operations.
- The container should not have unrestricted access to the system bus socket or otherwise given direct ability to invoke host mount (etc) operations - instead this should be narrowly scoped through a bridge on the host.
- Making mounts visible to the container should be local to its mount namespace, not host-visible remounting of arbitrary paths.
- Configurable bridge-side automounting (as opposed to e.g. delegating this to KDE's device automounter in the container). If automounting is disabled, report (applicable) unmounted filesystems such that they can be requested to be mounted on-demand (e.g. via the Disks & Devices UI).
- Preservation of device discovery & property changes over... *deep breath*... hotplug, coldplug, multi-partitioned drives, filesystem meta changes, and transient failures.
- Distinguish between the following:
    - filesystem being present
    - being host-mounted
    - being container-mounted (injected)
    - being container-unmounted (ie can still be host-mounted)
    - powered-off (distinct from *unmounted*)/ejected device
- Preserve host-mount prior to container start (edge case), clean-up host mounts created by the bridge after unmounting from container
    - Have this be consistent between user-interaction-driven unmount vs container shutdown (or crash etc, where possible)
- Only report successful unmounts on actual unmount (ie don't blindly remove from container view if not actually safe to remove)
- Don't overmount

## Architecture

```text
                        host (system bus / UDisks2)
                                    |
                        removable media + filesystem --> org.freedesktop.UDisks2
                                    |
                            enumerate/get properties
                        listen (InterfacesAdded,InterfacesRemoved, PropertiesChanged)
                                    |
                            removable-bridge (host)
                            |           |
        action requests + snapshots     |--> podman unshare mountinject
                            |                   |--> open_tree clone (host)
           AF_UNIX SOCK_SEQPACKET               |--> setns (target mnt ns)
                  /run/user/.../fng/...         |--> move_mount/umount2
                            |                             |
  ------------------ container namespace boundary --------|
                            |                             v
                        fng-disks (process)        /run/media/fng/LABEL
                            |
          owns org.freedesktop.UDisks2 (container system bus)
          exports Block/Filesystem/Drive/Manager objects
                            |
                   KDE Solid --> Plasma
```

`removable-bridge`:
    - host-side process
    - reads actual UDisks2 properties
    - discovers devices
    - requests host mounting where applicable
    - maintains device table
    - maintains control socket
    - sends state snapshots across proxy
    - track host mount ownership & container lifetime

`fng-disks`:
    - container-side process
    - owns `org.freedesktop.UDisks2` on container system bus
    - receives snapshots from the bridge
    - publishes subset of UDisks2 D-Bus object model
        - only forwards implemented actions to host bridge

`mountinject`:
    - short-lived host-side helper invoked via `podman unshare`
    - has `add` and `remove` subcommands
    - separation of concerns from bridge (ongoing process)
    - given host source (add), container PID target, and destination under the root of the mount namespace (target to inject/detach mount from)
    - not to be exposed to the container

`unix-socket.c`, `removable-protocol.h` and `fng-common.h` provide shared `AF_UNIX`/`SOCK_SEQPACKET` transport & shared helpers, alongside common UDisks constants.

## Authority boundaries
- Host UDisks2 is the ultimate authority on which block devices, filesystems etc exist, and performs the corresponding operations.
- The bridge only accepts action requests for objects corresponding to those in its tracked set (ie the socket cannot be used to mount arbitrary host directories).
- The substitute UDisks' properties were selected to work with Solid but avoid unintentional power-off requests during unmounts (allowing remounting within the container without physical unplug & re-plug).
- `mountinject` only changes target mount namespace - it is beholden to host UDisks policies and similar.
- Rootless container & user namespace isolation is a boundary largely at the Podman (& kernel level) ie only expose host functionality on an "as-needed" basis.

## Build
GCC or Clang, GNU C11, `pkg-config` and `libsystemd` development package, with the `systemd-devel` package additionally required (Rocky/RHEL specific). For `test-sanitise`, `dnf install libasan libubsan` if not already installed beforehand. `test-namespace` requires mount namespace ability on build machine.

## Deployment with FNG container
*NOTE: To hopefully happen in near future*

During testing, suitable additions to the `.xsession` (or `session.sh` if using my Wayland branch) was:

```bash
podman run ... --name arch ... --volume "$(pwd)/disk-stuff/bin/fng-disks:/usr/bin/fng-disks" ... &
podman_pid=$!

while [ "$(podman inspect -f '{{.State.Running}}' arch 2>/dev/null)" != "true" ]; do
    if ! kill -0 "$podman_pid" 2>/dev/null; then
        wait "$podman_pid"
        exit $?
    fi
    sleep 0.5
done

CONTAINER_PID="$(podman inspect -f '{{.State.Pid}}' arch)" || exit 1
if [ "$CONTAINER_PID" -le 0 ]; then
    echo "container arch is not running" >&2
    exit 1
fi

{
    echo
    echo "===== removable-bridge start $(date --iso-8601=seconds) ====="
} >>"./disk-stuff/removable-bridge.log"

./disk-stuff/bin/removable-bridge "$CONTAINER_PID" /run/media/fng "$(pwd)/disk-stuff/bin/mountinject" "$PROXY_DIR/removable.sock" >>"./disk-stuff/removable-bridge.log" --no-automount 2>&1 &
removable_bridge_pid=$!

wait "$podman_pid"
rc=$?
kill "$removable_bridge_pid" 2>/dev/null || true
wait "$removable_bridge_pid" 2>/dev/null || true
exit "$rc"
```

Similarly, modifications to the `Containerfile` were necessary:

```
systemctl mask \
    ...
    ... udisks2.service

...
COPY --chown=root:root --chmod=0644 disk-stuff/org.fng.UDisks2.conf /etc/dbus-1/system.d/org.fng.UDisks2.conf
COPY --chown=root:root --chmod=0644 disk-stuff/fng-disks.service /etc/systemd/system/fng-disks.service
RUN systemctl enable fng-disks.service
...
```

*These are example, not finalised implementation decisions.*

## Mounting behaviour
- By default, eligible filesystems are automatically injected at the point of discovery.
- Passing `--no-automount` publishes eligible filesystems without injecting.
- Plasma's own automounting configuration is separate (and disabled by default currently in the FNG container).
- A single private socket & tracked container PID are used - this is not intended to be multi-tenant.

## Documentation TODOs
*Basically to cover the rest of the more deep-dive bits scattered throughout my notes when I'm not sleep-deprived lmao*

- Host enumeration & filtering
- UDisks2 substitute object & property handling
- D-Bus lifetime
- Solid limitations
- Socket protocol
- Mount injection/detachment & pathname hardening
- Mount ownership & specifics on clean-ups
- Device lifecycle + manual mounts
- Retries & failure (/reconciliation) handling
- Limitations/security considerations & encountered regressions ~~if I come back across any relevant ones that weren't just sleep-deprived stupidity~~
- Debugging & testing
- Useful resources ~~yippee time for one of these again~~
