#!/usr/bin/env bash

# requires user & mount namespace support. run deliberately rather than w/ unpriv suite

set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=gnu11 -O2 -Wall -Wextra -Werror src/mountinject/mountinject.c -o "$build/mountinject"
export FNG_TEST_TMP="$build"
cat > "$build/inner.sh" <<'INNER'
#!/bin/bash
set -euo pipefail
mount -t tmpfs tmpfs "$FNG_TEST_TMP/target"
touch "$FNG_TEST_TMP/target/CHILD"
touch "$FNG_TEST_TMP/inner-ready"
while [ ! -e "$FNG_TEST_TMP/inner-done" ]; do sleep .1; done
umount "$FNG_TEST_TMP/target"
INNER
unshare -Ur -m --propagation private bash -euo pipefail -c '
    mkdir -p "$FNG_TEST_TMP/source" "$FNG_TEST_TMP/target" "$FNG_TEST_TMP/outside"
    mount -t tmpfs tmpfs "$FNG_TEST_TMP/source"
    printf "from-source" > "$FNG_TEST_TMP/source/marker"

    # symlinked parents rejected, outside dir untouched
    ln -s "$FNG_TEST_TMP/outside" "$FNG_TEST_TMP/target/link"
    if "$FNG_TEST_TMP/mountinject" add "$FNG_TEST_TMP/source" "$$" "$FNG_TEST_TMP/target/link/escape"; then exit 1; fi
    test ! -e "$FNG_TEST_TMP/outside/escape"

    # reject path traversal before mkdirat creates dirs
    if "$FNG_TEST_TMP/mountinject" add "$FNG_TEST_TMP/source" "$$" "$FNG_TEST_TMP/target/not-created/../escape"; then exit 1; fi
    test ! -e "$FNG_TEST_TMP/target/not-created"

    # reject symlink as final dst comp
    ln -s "$FNG_TEST_TMP/outside" "$FNG_TEST_TMP/target/final-link"
    if "$FNG_TEST_TMP/mountinject" add "$FNG_TEST_TMP/source" "$$" "$FNG_TEST_TMP/target/final-link"; then exit 1; fi
    test -L "$FNG_TEST_TMP/target/final-link"

    "$FNG_TEST_TMP/mountinject" add "$FNG_TEST_TMP/source" "$$" "$FNG_TEST_TMP/target/USB"
    test "$(cat "$FNG_TEST_TMP/target/USB/marker")" = from-source

    # refuse stacked mounts, original injected mount to remain intact
    if "$FNG_TEST_TMP/mountinject" add "$FNG_TEST_TMP/source" "$$" "$FNG_TEST_TMP/target/USB"; then exit 1; fi
    test "$(cat "$FNG_TEST_TMP/target/USB/marker")" = from-source

    "$FNG_TEST_TMP/mountinject" remove "$$" "$FNG_TEST_TMP/target/USB"
    test ! -e "$FNG_TEST_TMP/target/USB"
    "$FNG_TEST_TMP/mountinject" remove "$$" "$FNG_TEST_TMP/target/USB" # idempotent

    # file still open by app should not leave mount attached. also verify repeat attach/detach
    for i in 1 2 3; do
        "$FNG_TEST_TMP/mountinject" add "$FNG_TEST_TMP/source" "$$" "$FNG_TEST_TMP/target/USB"
        exec {held}< "$FNG_TEST_TMP/target/USB/marker"
        "$FNG_TEST_TMP/mountinject" remove "$$" "$FNG_TEST_TMP/target/USB"
        test ! -e "$FNG_TEST_TMP/target/USB"
        test "$(cat <&$held)" = from-source
        exec {held}<&-
    done

    # OPEN_TREE_CLONE indep of src mnt after attach. unmount of host path should not break injected mnt
    mkdir -p "$FNG_TEST_TMP/independent"
    mount -t tmpfs tmpfs "$FNG_TEST_TMP/independent"
    printf "still-readable" > "$FNG_TEST_TMP/independent/marker"
    "$FNG_TEST_TMP/mountinject" add "$FNG_TEST_TMP/independent" "$$" "$FNG_TEST_TMP/target/CLONE"
    umount "$FNG_TEST_TMP/independent"
    test "$(cat "$FNG_TEST_TMP/target/CLONE/marker")" = still-readable
    "$FNG_TEST_TMP/mountinject" remove "$$" "$FNG_TEST_TMP/target/CLONE"
    test ! -e "$FNG_TEST_TMP/target/CLONE"

    # inner ns overmounts dst parent. verify attach goes to inner tree not outer ns dir
    unshare -m bash "$FNG_TEST_TMP/inner.sh" & inner_pid=$!
    for i in {1..100}; do
        test -e "$FNG_TEST_TMP/inner-ready" && break
        sleep .1
    done
    test -e "$FNG_TEST_TMP/inner-ready"
    "$FNG_TEST_TMP/mountinject" add "$FNG_TEST_TMP/source" "$inner_pid" "$FNG_TEST_TMP/target/INNER"
    test "$(cat "/proc/$inner_pid/root$FNG_TEST_TMP/target/INNER/marker")" = from-source
    test ! -e "$FNG_TEST_TMP/target/INNER" # not injected in the outer namespace
    "$FNG_TEST_TMP/mountinject" remove "$inner_pid" "$FNG_TEST_TMP/target/INNER"
    test ! -e "/proc/$inner_pid/root$FNG_TEST_TMP/target/INNER"
    touch "$FNG_TEST_TMP/inner-done"
    wait "$inner_pid"

    umount "$FNG_TEST_TMP/source"
    echo "mountinject real namespace tests passed"
'
