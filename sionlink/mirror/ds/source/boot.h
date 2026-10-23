#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>

// Checks that a received packed image (common/slboot.h) can be booted.
// Returns NULL if so, or a short reason for the screen.
const char *BootCheck(const uint8_t *file, uint32_t size);

// Boots it. Wireless must already be off. Never returns.
void BootNds(const uint8_t *file, uint32_t size) __attribute__((noreturn));

#endif
