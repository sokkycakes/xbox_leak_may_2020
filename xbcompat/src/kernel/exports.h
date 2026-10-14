#ifndef XBCOMPAT_EXPORTS_H
#define XBCOMPAT_EXPORTS_H

struct kexport {
    unsigned ordinal;
    const char *name;
    void *address;      /* NULL when the export is not implemented */
    int is_data;
};

extern const struct kexport kernel_exports[];

#endif
