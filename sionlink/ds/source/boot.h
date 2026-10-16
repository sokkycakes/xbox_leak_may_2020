#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>

// Checks that a received .nds can be booted from where it sits in RAM.
// Returns NULL if so, or a short reason for the screen.
const char *BootCheck(const uint8_t *file, uint32_t size);

// Boots it (see common/slboot.h). Wireless must already be off. Never returns.
void BootNds(const uint8_t *file, uint32_t size) __attribute__((noreturn));

#endif
