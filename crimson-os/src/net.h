/* Small Winsock helpers shared by the host and the agent. */
#ifndef CRIMSON_NET_H
#define CRIMSON_NET_H
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"

/* hvsocket.h is not in MinGW, so the few pieces we need are defined here. */
#ifndef AF_HYPERV
#define AF_HYPERV 34
#endif
#define HV_PROTOCOL_RAW 1
typedef struct {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} CR_SOCKADDR_HV;
/* a42e7cda-d03f-480c-9cc2-a4de20abb878: the parent partition, seen from a guest */
static const GUID CR_HV_GUID_PARENT =
    {0xa42e7cda, 0xd03f, 0x480c, {0x9c, 0xc2, 0xa4, 0xde, 0x20, 0xab, 0xb8, 0x78}};
static const GUID CR_HV_GUID_WILDCARD = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

static int cr_parse_guid(const char *s, GUID *g)
{
    unsigned a, b, c, d[8];
    if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c,
               &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11)
        return 0;
    g->Data1 = a; g->Data2 = (USHORT)b; g->Data3 = (USHORT)c;
    for (int i = 0; i < 8; i++) g->Data4[i] = (BYTE)d[i];
    return 1;
}

static int cr_send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return 0;
        p += n; len -= n;
    }
    return 1;
}

static int cr_recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return 0;
        p += n; len -= n;
    }
    return 1;
}

/* Sends header + body (+ optional payload) as one message. */
static int cr_send_msg(SOCKET s, uint16_t type, const void *body, uint32_t body_len,
                       const void *extra, uint32_t extra_len)
{
    cr_hdr h = { CR_MAGIC, type, CR_VERSION, body_len + extra_len };
    if (!cr_send_all(s, &h, sizeof h)) return 0;
    if (body_len && !cr_send_all(s, body, (int)body_len)) return 0;
    if (extra_len && !cr_send_all(s, extra, (int)extra_len)) return 0;
    return 1;
}

static void cr_tune_socket(SOCKET s)
{
    int one = 1, buf = 8 << 20;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one); /* ignored on hvsocket */
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&buf, sizeof buf);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&buf, sizeof buf);
}
#endif
