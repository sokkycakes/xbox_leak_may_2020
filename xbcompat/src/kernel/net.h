/* Host side of the emulated Ethernet controller (nic.c, net.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NET_MAX_FRAME 1536

/* Backend spec: "nat" (the default when networking is on), or
   "tunnel:HOST:PORT[:LOCALPORT]" (raw frames in UDP to another xbcompat). */
bool net_init(const char *spec, const uint8_t mac[6]);
/* A frame the title transmitted (Ethernet header included, no FCS). */
void net_send(const uint8_t *frame, unsigned len);

/* A frame for the title; dropped when its address filter rejects it. */
void nic_receive(const uint8_t *frame, unsigned len);
void nic_poll(void);
void nic_init(const char *spec, const uint8_t mac[6]);
bool nic_trap_fault(void *addr, void *ucontext);
bool nic_trap_step(void *ucontext);
