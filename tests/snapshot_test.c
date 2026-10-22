/*
* Proxy-state transaction tests w/ mocked D-Bus.
* Malformed records/escapes/dupes must not replaced exported state.
* Valid state changes must product coherent notifs.
*/

#define main fng_disks_main
#include "../src/proxy/fng-disks.c"
#include "../src/proxy/disks-state.c"
#include "../src/proxy/disks-export.c"
#undef main
#include <assert.h>
#include <stdarg.h>

static int added;
static int removed;
static int block_changed;
static int mounts_changed;
static int drive_changed;
static int exported_boolean;

int sd_bus_message_append(sd_bus_message *m, const char *signature, ...) {
    (void)m;

    assert(!strcmp(signature, "b") || !strcmp(signature, "s"));

    va_list args;
    va_start(args, signature);

    if (!strcmp(signature, "b"))
        exported_boolean = va_arg(args, int);
    else
        (void)va_arg(args, const char *);

    va_end(args);
    return 1;
}

int sd_bus_message_append_array(sd_bus_message *m, char type, const void *data, size_t size) {
    (void)m;
    (void)type;
    (void)data;
    (void)size;

    return 1;
}

int sd_bus_object_path_is_valid(const char *p) {
    return p && *p == '/' && !strchr(p, ' ') ? 1 : 0;
}

int sd_bus_emit_object_added(sd_bus *b, const char *p) {
    (void)b;
    (void)p;
    added++;

    return 0;
}

int sd_bus_emit_object_removed(sd_bus *b, const char *p) {
    (void)b;
    (void)p;
    removed++;

    return 0;
}

int sd_bus_emit_properties_changed(sd_bus *b, const char *path, const char *iface, const char *property, ...) {
    (void)b;
    (void)path;
    (void)property;

    if (!strcmp(iface, IFACE_BLOCK))
        block_changed++;

    else if (!strcmp(iface,IFACE_FILESYSTEM))
        mounts_changed++;

    else if (!strcmp(iface,IFACE_DRIVE))
        drive_changed++;

    return 0;
}

static int apply(const char *s) {
    char *dup = strdup(s);
    assert(dup);
    int r = apply_state(dup);
    free(dup);

    return r;
}

int main(void) {
    const char *A = "/org/freedesktop/UDisks2/block_devices/sdb1";
    const char *D = "/org/freedesktop/UDisks2/drives/drive1";
    const char *P = "/mnt/USB";

    char state[4096];
    snprintf(state, sizeof(state), "%s\t%s\t%s\tFIRST\tuuid\tvfat\t1\t1\t0\t0\n", A, D, P);

    assert(apply(state) == 0 && added == 2 && removed == 0 && devices->info.mounted);
    // host able to power USB drive off, proxy clients shouldn't see that capability
    assert(devices->info.can_power_off);
    exported_boolean = -1;
    assert(export_property(NULL, D, IFACE_DRIVE, "CanPowerOff", NULL, devices, NULL) >= 0);
    assert(exported_boolean == 0);

    assert(export_property(NULL, D, IFACE_DRIVE, "MediaRemovable", NULL, devices, NULL) >= 0);
    assert(exported_boolean == 0);
    assert(apply(state) == 0 && added == 2 && !block_changed && !mounts_changed);

    snprintf(state, sizeof(state), "%s\t%s\t%s\tNEW\\tLABEL\\nLINE\tuuid2\texfat\t0\t0\t1\t1\n", A, D, P);
    assert(apply(state) == 0 && added == 2 && removed == 0 && block_changed == 1 && mounts_changed == 1 && drive_changed == 1);
    assert(!strcmp(devices->info.label,"NEW\tLABEL\nLINE") && !devices->info.mounted && !devices->info.can_power_off);
    assert(export_property(NULL, D, IFACE_DRIVE, "MediaRemovable", NULL, devices, NULL) >= 0);
    assert(exported_boolean == 1); // reader eject behaviour remains intact
    assert(export_property(NULL, D, IFACE_DRIVE, "CanPowerOff", NULL, devices, NULL) >= 0);
    assert(exported_boolean == 0);

    assert(apply("malformed\n") < 0 && devices && block_changed == 1 && removed == 0);
    assert(apply("/dev/x\t/drive/d\t/mnt/a\tlabel\tuuid\text4\t1\n") < 0 && devices);

    snprintf(state, sizeof(state), "%s\t%s\t%s\tFIRST\tuuid\tvfat\t1\t1\t0\t0\n%s\t%s\t%s\tFIRST\tuuid\tvfat\t1\t1\t0\t0\n", A, D, P, A, D, P);
    assert(apply(state) < 0 && devices && removed == 0);

    // invalid escaping/bool fields must reject full update atomically
    snprintf(state, sizeof(state), "%s\t%s\t%s\tBAD\\q\tuuid\tvfat\t1\t1\t0\t0\n", A, D, P);
    assert(apply(state) < 0 && devices && added == 2 && removed == 0);
    snprintf(state, sizeof(state), "%s\t%s\t%s\tBAD\tuuid\tvfat\t2\t1\t0\t0\n", A, D, P);
    assert(apply(state) < 0 && devices && added == 2 && removed == 0);

    char both[4096];
    snprintf(both, sizeof(both), "%s\t%s\t%s\tNEW\tuuid2\texfat\t0\t0\t1\t1\n/org/freedesktop/UDisks2/block_devices/sdb2\t%s\t/mnt/SECOND\tSECOND\tu2\text4\t1\t0\t0\t0\n", A, D, P, D);
    assert(apply(both) == 0 && added == 3 && removed == 0);

    snprintf(state, sizeof(state), "%s\t%s\t%s\tNEW\tuuid2\texfat\t0\t0\t1\t1\n", A, D, P);
    assert(apply(state) == 0 && added == 3 && removed == 1); // keep drive
    assert(apply("\n") == 0 && devices == NULL && removed == 3); // block + drive
    puts("snapshot tests passed");
    return 0;
}
