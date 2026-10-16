// sionsniff: records what is on the air on a monitor interface, as a pcap
// file (radiotap), to see what a 3DS or DS and the Sion send each other.
//
//   sionsniff IFACE OUT.pcap SECONDS
//
// Keeps probe requests, beacons with a Nintendo element, and every frame
// to or from a Nintendo MAC (00:09:BF, 40:D2:8A and the like) or the Sion
// (02: and 06: addresses on its card); other networks' beacons are left out. Data frames are cut to their first 64 bytes: a 3DS's are
// encrypted anyway, and the headers are what matter here.

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void Stop(int sig)
{
    (void)sig;
    stop = 1;
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: sionsniff IFACE OUT.pcap SECONDS\n");
        return 2;
    }
    int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (s < 0)
    {
        perror("socket");
        return 1;
    }
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL),
                               .sll_ifindex = if_nametoindex(argv[1]) };
    if (!sll.sll_ifindex || bind(s, (struct sockaddr *)&sll, sizeof(sll)) < 0)
    {
        perror(argv[1]);
        return 1;
    }
    struct packet_mreq mr = { .mr_ifindex = sll.sll_ifindex, .mr_type = PACKET_MR_PROMISC };
    setsockopt(s, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr));
    struct ifreq ifr = { 0 };
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", argv[1]);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0 && !(ifr.ifr_flags & IFF_UP))
        fprintf(stderr, "sionsniff: %s is down\n", argv[1]);
    FILE *f = fopen(argv[2], "wb");
    if (!f)
    {
        perror(argv[2]);
        return 1;
    }
    // pcap header: LINKTYPE_IEEE802_11_RADIOTAP (127)
    uint32_t gh[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 127 };
    fwrite(gh, sizeof(gh), 1, f);
    time_t end = time(NULL) + atoi(argv[3]);
    struct timeval tv = { 0, 200000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    long kept = 0, seen = 0, beacons = 0, probes = 0;
    time_t next_report = time(NULL) + 5;
    uint8_t buf[4096];
    struct sigaction sa = { .sa_handler = Stop };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    while (time(NULL) < end && !stop)
    {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            perror("sionsniff: recv");
            break;
        }
        if (time(NULL) >= next_report)
        {
            fprintf(stderr, "sionsniff: %ld frames seen, %ld kept so far\n", seen, kept);
            next_report = time(NULL) + 5;
        }
        if (n < 8)
            continue;
        seen++;
        int rt = buf[2] | buf[3] << 8; // radiotap length
        if (rt + 24 > n)
            continue;
        const uint8_t *h = buf + rt;
        int type = (h[0] >> 2) & 3, sub = h[0] >> 4;
        int keep = 0;
        int cut = n;
        if (type == 0 && sub == 4)
            keep = 1; // probe requests: what a scanning 3DS or DS asks for
        else if (type == 0 && sub == 8)
        {
            // beacons carrying a Nintendo element (the Sion's, a 3DS's or a DS's)
            for (int i = rt + 36; i + 5 <= n && !keep; i++)
                keep = buf[i] == 0xDD && ((buf[i + 2] == 0x00 && buf[i + 3] == 0x1F && buf[i + 4] == 0x32) ||
                                          (buf[i + 2] == 0x00 && buf[i + 3] == 0x09 && buf[i + 4] == 0xBF));
        }
        if (!keep)
        {
            static const uint8_t ouis[][3] = { { 0x00, 0x09, 0xBF }, { 0x00, 0x16, 0x56 }, { 0x40, 0xD2, 0x8A },
                                               { 0x02, 0x1D, 0x96 }, { 0x06, 0x1D, 0x96 }, { 0x00, 0x1F, 0x32 },
                                               { 0x2C, 0x10, 0xC1 }, { 0x98, 0xB6, 0xE9 }, { 0x9C, 0xE6, 0x35 } };
            for (unsigned i = 0; i < sizeof(ouis) / 3 && !keep; i++)
                for (int a = 4; a <= 16 && !keep; a += 6)
                    keep = !memcmp(h + a, ouis[i], 3);
            if (type != 0)
                cut = rt + 64 < n ? rt + 64 : n;
        }
        if (!keep)
            continue;
        if (type == 0 && sub == 8)
            beacons++;
        if (type == 0 && sub == 4)
            probes++;
        struct timeval now;
        gettimeofday(&now, NULL);
        uint32_t ph[4] = { (uint32_t)now.tv_sec, (uint32_t)now.tv_usec, (uint32_t)cut, (uint32_t)n };
        fwrite(ph, sizeof(ph), 1, f);
        fwrite(buf, cut, 1, f);
        fflush(f);
        kept++;
    }
    fclose(f);
    printf("sionsniff: %ld frames seen, %ld kept (%ld beacons, %ld probe requests)\n", seen, kept, beacons, probes);
    return 0;
}
