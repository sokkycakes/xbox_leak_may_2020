#ifndef XBCOMPAT_HLE_H
#define XBCOMPAT_HLE_H

struct hle_func {
    const char *name;   /* decorated name as in the library, e.g. _D3DDevice_Clear@24 */
    void *impl;
};

/* Each replaced library provides a NULL-terminated table. */
extern const struct hle_func d3d8_funcs[];
extern const struct hle_func xinput_funcs[];
extern const struct hle_func dsound_funcs[];

const struct hle_func *hle_find(const char *name);
ULONG hle_lookup(const char *name);
ULONG hle_lookup_prefix(const char *prefix);
void d3d_bind_globals(void);

#endif
