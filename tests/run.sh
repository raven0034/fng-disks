#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc=${CC:-cc}
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function)

if [[ "${SANITISE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}"
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
fi

for file in src/{bridge,proxy,mountinject,common}/*.c; do
    "$cc" "${flags[@]}" -fsyntax-only -Itests/support "$file"
done

# compile bridge & proxy as separate TUs as well as tests below
# latter includes .c files in one test TU to inspect private state (o/w would miss dupe global defs)
# reloc link catches these with needing running sys bus or real libsystemd when testing
for app in bridge proxy; do
    if [[ $app == bridge ]]; then
        sources=(src/bridge/removable-bridge.c src/bridge/bridge-mounts.c src/bridge/bridge-discovery.c src/bridge/bridge-control.c)
    else
        sources=(src/proxy/fng-disks.c src/proxy/disks-state.c src/proxy/disks-export.c)
    fi

    objects=()
    for source in "${sources[@]}"; do
        object="$build/$(basename "${source%.c}").o"
        "$cc" "${flags[@]}" -Itests/support -c "$source" -o "$object"
        objects+=("$object")
    done
    ld -r "${objects[@]}" -o "$build/${app}-combined.o"
done

for t in snapshot bridge socket mountinject; do
    files=("tests/${t}_test.c")
    if [[ $t == socket ]]; then files+=(src/common/unix-socket.c); fi

    "$cc" "${flags[@]}" -ffunction-sections -fdata-sections -Itests/support "${files[@]}" \
        -Wl,--gc-sections -o "$build/$t"
    "$build/$t"
done
