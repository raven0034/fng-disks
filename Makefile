CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS ?=
WARNINGS = -std=gnu11 -Wall -Wextra -Werror
SYSTEMD_CFLAGS := $(shell $(PKG_CONFIG) --cflags libsystemd 2>/dev/null)
SYSTEMD_LIBS := $(shell $(PKG_CONFIG) --libs libsystemd 2>/dev/null)
BUILD_DIR ?= bin

BRIDGE_DIR = src/bridge
PROXY_DIR = src/proxy
COMMON_DIR = src/common
MOUNT_DIR = src/mountinject

BRIDGE_SRCS = $(addprefix $(BRIDGE_DIR)/,removable-bridge.c bridge-mounts.c bridge-discovery.c bridge-control.c)
PROXY_SRCS = $(addprefix $(PROXY_DIR)/,fng-disks.c disks-state.c disks-export.c)
SOCKET_SRC = $(COMMON_DIR)/unix-socket.c
COMMON_HDRS = $(addprefix $(COMMON_DIR)/,unix-socket.h removable-protocol.h fng-common.h)

.PHONY: all clean test test-sanitise test-namespace check-scripts
all: $(BUILD_DIR)/removable-bridge $(BUILD_DIR)/fng-disks $(BUILD_DIR)/mountinject test test-namespace

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/removable-bridge: $(BRIDGE_SRCS) $(SOCKET_SRC) $(BRIDGE_DIR)/bridge-internal.h $(COMMON_HDRS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(SYSTEMD_CFLAGS) $(CFLAGS) $(WARNINGS) $(BRIDGE_SRCS) $(SOCKET_SRC) $(LDFLAGS) $(SYSTEMD_LIBS) $(LDLIBS) -o $@
	chmod 0755 $@

$(BUILD_DIR)/fng-disks: $(PROXY_SRCS) $(SOCKET_SRC) $(PROXY_DIR)/disks-internal.h $(COMMON_HDRS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(SYSTEMD_CFLAGS) $(CFLAGS) $(WARNINGS) $(PROXY_SRCS) $(SOCKET_SRC) $(LDFLAGS) $(SYSTEMD_LIBS) $(LDLIBS) -o $@
	chmod 0755 $@

$(BUILD_DIR)/mountinject: $(MOUNT_DIR)/mountinject.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< $(LDFLAGS) $(LDLIBS) -o $@
	chmod 0755 $@

test:
	CC=$(CC) ./tests/run.sh

test-sanitise:
	CC=$(CC) sanitise=1 ./tests/run.sh

test-namespace:
	./tests/mountinject_integration.sh

check-scripts:
	bash -n tests/run.sh tests/mountinject_integration.sh

clean:
	rm -rf $(BUILD_DIR)
