// Sion Link test bridge: puts sionlinkd's 3DS radio into an Azahar
// multiplayer room, so a 3DS program running in Azahar finds and joins the
// Sion like it would over the air (in the cloud, where there is no 3DS).
//
// Azahar carries local wireless between room members as "WiFi packets":
// beacon, authentication, association response and data frame bodies, with
// the sender's and receiver's MAC addresses beside them, unencrypted. The
// bridge joins the room with the Sion's MAC and passes each packet to and
// from sionlinkd (--air3ds PORT) as one UDP datagram:
//   [type] [channel] [from MAC (6)] [to MAC (6)] [body]
// with type as Azahar numbers it (0 beacon, 1 data, 2 auth, 3 assoc
// response, 4 deauth, 5 node map).
//
// usage: azbridge ROOM_HOST ROOM_PORT SIONLINKD_UDP_PORT [MAC]
// Builds against an Azahar build tree (build.sh).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "network/network.h"
#include "network/room_member.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: azbridge ROOM_HOST ROOM_PORT SIONLINKD_UDP_PORT [MAC]\n");
        return 2;
    }
    Network::MacAddress mac = {0x02, 0x51, 0x0E, 0x4E, 0x00, 0x01};
    if (argc > 4)
        std::sscanf(argv[4], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3],
                    &mac[4], &mac[5]);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me{}, peer{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    me.sin_port = 0;
    bind(sock, reinterpret_cast<sockaddr*>(&me), sizeof(me));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer.sin_port = htons(static_cast<u16>(std::atoi(argv[3])));

    Network::Init();
    auto member = Network::GetRoomMember().lock();
    std::atomic<bool> joined{false};
    member->BindOnStateChanged([&](const Network::RoomMember::State& s) {
        joined = s == Network::RoomMember::State::Joined;
        std::fprintf(stderr, "azbridge: room state %d\n", static_cast<int>(s));
    });
    member->BindOnWifiPacketReceived([&](const Network::WifiPacket& p) {
        u8 buf[2048];
        if (p.data.size() + 14 > sizeof(buf))
            return;
        buf[0] = static_cast<u8>(p.type);
        buf[1] = p.channel;
        std::memcpy(buf + 2, p.transmitter_address.data(), 6);
        std::memcpy(buf + 8, p.destination_address.data(), 6);
        std::memcpy(buf + 14, p.data.data(), p.data.size());
        sendto(sock, buf, 14 + p.data.size(), 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    });
    member->Join("Sion", "0000000000000000", argv[1], static_cast<u16>(std::atoi(argv[2])), 0, mac,
                 "");

    // sionlinkd -> room. The first datagram from sionlinkd also tells it
    // where we are (it answers to the sender).
    sendto(sock, "", 0, 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    for (;;) {
        u8 buf[2048];
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n < 14 || !joined)
            continue;
        Network::WifiPacket p;
        p.type = static_cast<Network::WifiPacket::PacketType>(buf[0]);
        p.channel = buf[1];
        std::memcpy(p.transmitter_address.data(), buf + 2, 6);
        std::memcpy(p.destination_address.data(), buf + 8, 6);
        p.data.assign(buf + 14, buf + n);
        member->SendWifiPacket(p);
    }
}
