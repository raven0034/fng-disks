#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sd_bus sd_bus;

typedef struct sd_bus_message sd_bus_message;

typedef struct sd_bus_slot sd_bus_slot;

typedef struct sd_bus_error { 
    const char *name;
    const char *message;
    int _need_free;
} sd_bus_error;

typedef struct sd_bus_vtable {
    int t;
} sd_bus_vtable;

#define SD_BUS_ERROR_NULL ((sd_bus_error){0})
#define SD_BUS_ERROR_INVALID_ARGS "org.freedesktop.DBus.Error.InvalidArgs"
#define SD_BUS_ERROR_FAILED "org.freedesktop.DBus.Error.Failed"
#define SD_BUS_VTABLE_START(x) {0}
#define SD_BUS_VTABLE_END {0}
#define SD_BUS_PROPERTY(...) {0}
#define SD_BUS_METHOD_WITH_OFFSET(...) {0}
#define SD_BUS_VTABLE_PROPERTY_CONST 1
#define SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE 2
#define SD_BUS_VTABLE_UNPRIVILEGED 4

int sd_bus_object_path_is_valid(const char *);

int sd_bus_error_setf(sd_bus_error *, const char *, const char *, ...);
void sd_bus_error_free(sd_bus_error *);

const char *sd_bus_message_get_member(sd_bus_message *);
const char *sd_bus_message_get_path(sd_bus_message *);

int sd_bus_message_append(sd_bus_message *, const char * , ...);
int sd_bus_message_append_array(sd_bus_message *, char, const void *, size_t);

int sd_bus_message_open_container(sd_bus_message *, char, const char *);
int sd_bus_message_close_container(sd_bus_message *);
int sd_bus_message_enter_container(sd_bus_message *, char, const char *);

int sd_bus_message_read(sd_bus_message *, const char *, ...);
int sd_bus_message_read_array(sd_bus_message *, char, const void **, size_t *);

int sd_bus_reply_method_return(sd_bus_message *, const char *, ...);

int sd_bus_add_object_vtable(sd_bus *, sd_bus_slot **, const char *, const char *, const sd_bus_vtable *, void *);
int sd_bus_add_object_manager(sd_bus *, sd_bus_slot **, const char *);
int sd_bus_add_fallback_vtable(sd_bus *, sd_bus_slot **, const char *, const char *, const sd_bus_vtable *, int (*)(sd_bus *, const char *, const char *, void *, void **, sd_bus_error *), void *);
int sd_bus_add_node_enumerator(sd_bus *, sd_bus_slot **, const char *, int (*)(sd_bus *, const char *, void *, char ***, sd_bus_error *), void *);

int sd_bus_emit_object_added(sd_bus *, const char *);
int sd_bus_emit_object_removed(sd_bus *, const char *);
int sd_bus_emit_properties_changed(sd_bus *, const char *, const char *, const char *, ...);

int sd_bus_open_system(sd_bus **);
int sd_bus_process(sd_bus *, sd_bus_message **);

int sd_bus_get_fd(sd_bus *);
int sd_bus_get_events(sd_bus *);

int sd_bus_request_name(sd_bus *, const char *, uint64_t);
sd_bus *sd_bus_unref(sd_bus *);

int sd_bus_get_property(sd_bus *, const char *, const char *, const char *, const char *, sd_bus_error *, sd_bus_message **, const char *);
int sd_bus_get_property_trivial(sd_bus *, const char *, const char *, const char *, const char *, sd_bus_error *, char,void *);
int sd_bus_get_property_string(sd_bus *, const char *, const char *, const char *, const char *, sd_bus_error *, char **);

int sd_bus_call_method(sd_bus *, const char *, const char *, const char *, const char *, sd_bus_error *, sd_bus_message **, const char *, ...);

sd_bus_message *sd_bus_message_unref(sd_bus_message *);

int sd_bus_match_signal(sd_bus *, sd_bus_slot **, const char *, const char *, const char *, const char *, int (*)(sd_bus_message *, void *, sd_bus_error *), void *);
int sd_bus_add_match(sd_bus *, sd_bus_slot **, const char *, int (*)(sd_bus_message *, void *, sd_bus_error *), void *);

sd_bus_slot *sd_bus_slot_unref(sd_bus_slot *);
