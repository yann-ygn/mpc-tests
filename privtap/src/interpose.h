#ifndef PRIVTAP_INTERPOSE_H
#define PRIVTAP_INTERPOSE_H

#define API __attribute__((visibility("default")))
#define RESOLVE(variable, name) (variable || (variable = privtap_next_symbol(name)))

/* Private boundaries shared by the libc hooks and the raw-MIDI shim. */
void *privtap_next_symbol(const char *name);
int privtap_identity_fd(const char *path);

#endif
