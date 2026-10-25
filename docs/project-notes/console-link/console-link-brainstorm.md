# Console Link on the Sion: brainstorm

Brainstorm notes, 2026-10-12. Nothing here is built. Claims about specific homebrew projects are from memory and should be checked before we pick one.

## The big catch first

No Xbox game knows about any of this. GameCube games had GBA code inside them; ours don't. So "Console Link" on the Sion has to come from our side, in three possible forms:

1. **Dashboard sends a program.** A "Link" area in the dash detects a handheld and pushes a standalone app or minigame to it (the Download Play / GBA multiboot model). This needs no game support at all.
2. **Companion per game.** xbcompat already owns the guest's RAM and input. A small per-title companion (keyed by title ID, like RetroAchievements memory maps or Dolphin's Wind Waker Tingle Tuner) reads game state and streams it to the handheld: a map, inventory, ammo, a second screen. Input from the handheld goes back through xbcompat's pad layer. This is the closest thing to "connect while the game is running".
3. **Handheld as a controller.** Like the GBA as a GameCube pad. The handheld shows up as one of xbcompat's four pad slots. Cheap to do once a link exists, and it works with every game.

Native "compatible game" linking (a retail PSP or DS game that expects to talk to a console game) is mostly out, because the console half of that conversation was never written for Xbox. The exceptions are handheld games that speak a documented protocol we could fake.

## Console side hardware

- **Sion:** x86 Linux, Intel 7265 Wi-Fi (2.4 and 5 GHz), no Ethernet in use, USB ports. The 7265 under iwlwifi can run as a 2.4 GHz access point with hostapd and does monitor mode; raw frame injection on it is hit or miss.
- **Pi 3:** same story plus GPIO pins, which matter for cables. Its built-in Broadcom Wi-Fi does monitor mode and injection only with the nexmon firmware patch.
- **Cable links** go through a small USB microcontroller (a Raspberry Pi Pico, about $4) that speaks the handheld's wire protocol and shows up on the Sion as a USB serial device. That keeps the Sion and Pi builds identical.

## Per handheld

### GBA (recommended first)
- **Why it works:** the GBA BIOS has multiboot built in. Any GBA with no cartridge (or holding Start+Select at boot) will accept a program of up to 256 KB over the link cable and run it from RAM. No flashcart, no custom firmware. This is exactly how the GameCube did it.
- **Cable:** sokky's GBA-to-GameCube cable speaks JoyBus, the GameCube controller-port protocol. The GC plug goes into a Pico (or a GameCube controller-port socket wired to one) running a JoyBus host. JoyBus is a single 3.3 V data line at about 250 kbit/s, which the Pico's PIO handles well. Several open projects already do JoyBus on a Pico for GC controllers; the GBA side of JoyBus is documented in GBATEK.
- **Alternative cable:** a plain GBA link cable cut open and wired to Pi 3 GPIO (or the Pico) in "normal" SPI mode. Pi GBA-multiboot scripts have existed for years. Cheaper, but sokky already has the JoyBus cable, and JoyBus is the authentic GC path.
- **GBA side software:** devkitARM + libtonc or Butano. afska's gba-link-connection library covers multiboot and link modes on the GBA end.
- **Speed:** a 256 KB upload takes a few seconds over JoyBus. After that, a few KB/s of live data is plenty for a map or status screen.
- **DS Lite / original DS** can also run GBA mode, but has no link port, so it doesn't help here.

### 3DS (best wireless option, with custom firmware)
- Most 3DS owners who'd use this already run Luma3DS + the Homebrew Launcher. 3dslink sends a .3dsx from a PC to the 3DS over normal Wi-Fi (TCP), and it's a tiny protocol we could serve from the Sion.
- The 3DS does WPA2, so it can join the Sion's own access point or the home network.
- Real 3DS Download Play (UDS) is reverse engineered on 3dbrew, but the child app has to be signed, so stock consoles can't take our programs. Not worth chasing.
- Bonus: a 3DS companion app can show the bottom-screen map/inventory idea nicely.

### Phones (widest reach, easiest)
- The Sion hosts a small web page (or the dash shows a QR code). Any phone on the same Wi-Fi gets a second screen or a touch controller in the browser. No install, works with every phone.
- Same companion data feed as the 3DS, so building one gets the other mostly free.
- Least "retro", most likely to actually get used.

### PSP
- PS2-to-PSP linking used USB, but on the PSP side that needs a homebrew app, which needs custom firmware (easy on every PSP model today).
- With CFW, a PSP app can talk over USB or over infrastructure Wi-Fi (802.11b, WEP/WPA-PSK depending on firmware). Wi-Fi to a Sion access point is simpler than USB; PSPLink-style USB host tools exist if we want the cable.
- PSP "Game Sharing" (the XMB's own send-a-demo-over-ad-hoc feature) is the real Download Play equivalent, but sending our own programs that way would need signed EBOOTs. Out.

### DS
- **Download Play (NiFi):** the DS firmware accepts a program over Nintendo's raw 802.11 frames, but the stock firmware checks an RSA signature, so unsigned homebrew only boots on a DS with FlashMe installed. The PC side needs a Wi-Fi card that can inject raw frames (old WMB host tools used Prism2/Atheros cards). The Intel 7265 is a gamble here; a cheap USB Atheros AR9271 dongle is the safe bet.
- **Easier DS route:** a flashcart app using dswifi joins the Sion over normal Wi-Fi. Catch: the DS only does open or WEP networks, so the Sion would need a separate open/WEP access point just for it.
- Verdict: fun, but the hardest path with the smallest audience. Last.

### Other things worth a mention
- **Dreamcast VMU:** VMU minigames were sent from the console over the Maple bus. Pico Maple bus projects exist. A Sion that sends VMU minigames would be a delightful party trick, and it's the same Pico-on-USB design as the GBA.
- **PlayStation Vita / PSTV:** with HENkaku it behaves like the 3DS case (homebrew over Wi-Fi). It also has native remote play heritage, so a "stream the Xbox game to the Vita" mode is plausible.
- **Wii U GamePad:** the libdrc / drc-sim projects got it working on PC with specific 5 GHz Wi-Fi cards. Very cool, very fiddly.
- **Pokémon-style link cable games (GB/GBC):** a Pico can also speak the Game Boy link port, so a "trade with the Sion" style box (storage, distribution events) is possible later.

## How it would plug in

- **linkd**, a small Sion service that owns every link transport: the Pico (USB serial) for GBA/VMU, a TCP/WebSocket server for 3DS, phones, PSP and Vita, and later raw NiFi. It knows what's connected and what it can do (receive a program, show a screen, act as a pad).
- **Dashboard:** a "Link" area that lists connected handhelds and a library of sendable programs ("Send to GBA", "Pair phone" with a QR code). It can also show a small handheld icon when one is linked.
- **xbcompat:**
  - Companion hooks: per title ID, a small script or module that names guest RAM addresses and what they mean, then sends updates to linkd at a few Hz.
  - Pad bridge: linkd exposes a linked handheld as a virtual pad that xbcompat already knows how to read.
  - Optional new export for homebrew Xbox titles built against our headers (an "XLink" API), so new Xbox homebrew can link natively, GameCube style.
- **Programs to send:** a GBA "Sion Link" multiboot app that, once loaded, shows the companion screen or acts as a pad. Later, small minigames.

## Suggested order

1. **GBA over the JoyBus cable:** Pico JoyBus host, multiboot upload of a test ROM from the Sion, then the GBA as a pad. Proves the whole idea with hardware sokky owns.
2. **One companion for one game** (something with a map or inventory worth a second screen), shown on the GBA.
3. **Wi-Fi side:** linkd's network server, a phone web page, then a 3DS app sharing the same feed.
4. **Stretch:** VMU minigames, PSP/Vita apps, DS Download Play with an injection-capable dongle.

## Things to confirm before building

- Which game first gets a companion.
- Whether sokky has a Pico (or will buy one) and a GameCube controller socket or extension cable to cut.
- Whether the Intel 7265 can run an access point while staying joined to the home network at the same time (otherwise the Sion's AP becomes the Sion's only network while linking).

## Tangent: Splinter Cell's GameCube GBA link on the Xbox version

sokky's idea (2026-10-12): take the GBA link from Splinter Cell GameCube (same Unreal Engine 2 base, ships a GBA multiboot program plus in-game handling) and add it to the Xbox version on the Sion as a title-update-style patch.

What the GC version does (from memory, worth confirming in Dolphin): the GBA shows a live overhead map with Sam and nearby enemies, and can trigger some gadgets such as remote sticky bombs.

### What carries over and what doesn't
- **The GBA program** is just a GBA binary on the GC disc. It runs as-is; we only have to send it over JoyBus and answer it the way the GC game did.
- **Game logic that lives in UnrealScript** (.u packages) is bytecode, not CPU code, so it is not tied to PowerPC. If the GC build's link logic is mostly script, it could in principle be moved into the Xbox packages. The risk is that the Xbox and GC builds are different engine revisions, so package versions and class layouts may not line up.
- **Native C++ code** (the JoyBus driver and anything else compiled) is PowerPC and can't be copied. That part becomes a new native function that xbcompat provides.

### Two ways to build it
1. **Script port.** Decompile the GC link classes (UE Explorer has handled Splinter Cell packages before), port them into the Xbox packages, and back their native calls with an xbcompat export that talks to linkd. Closest to "the GC feature, on Xbox".
2. **Host-side companion, no game patch.** xbcompat reads Sam's and the enemies' positions straight out of Xbox SC's memory (UE2's global object list is findable) and feeds the GBA program in the same message format the GC game used. GBA actions such as detonating come back as injected inputs or a hooked call. Less faithful, but independent of package compatibility.

Step zero for either: run the GC version in Dolphin with its built-in GBA (mGBA) link, log the JoyBus traffic, and look at whether the GC packages hold the link logic as script. That decides route 1 vs 2.

### The title update wrapper
Real Xbox title updates live in `TDATA\<TitleID>\$u\` (for SC, 5553000C): a replacement default.xbe plus any changed data files, and the dash launches that instead of the disc XBE. xbcompat can honor the same folder as an overlay on the disc: XBE, patched .u packages, the GBA ROM and a link manifest. The dash can show it like a normal TU.

Shipping: the package has to be a patch applied to sokky's own discs, not Ubisoft's files. That means it needs a GC disc image as input to pull the GBA program and the original GC packages.

## DS NiFi: checked against sources (2026-10-12)

- GBATEK, "DS Wifi Nintendo Beacons" / "Multiplay Master" / "Multiplay Slave" / "Nintendo DS Download Play" (https://problemkaputt.de/gbatek.htm):
  - Host beacons at 2 Mbit/s with no public SSID, carrying a Nintendo element (tag DDh, OUI 00:09:BF:00) with game ID, stream code, beacon type (multiboot = 0Bh) and Download Play icon/title snippets. Channels 1, 7 or 13.
  - Clients join with standard 802.11 auth + association; the association SSID is built from the game ID + stream code.
  - Host sends CMD frames (DA 03:09:BF:00:00:00); each client's hardware sends its queued REPLY (DA ...:10) in its slot; the host's hardware then sends a CMD ACK (DA ...:03) automatically after the reply window of 16 + (10 + W_CMD_REPLYTIME) * clients microseconds. Non-DS hosts must reproduce that ACK themselves, with SEQNO one higher than the CMD.
  - Download Play needs an RSA signature (private key unknown). GBATEK notes the RSA frame itself (entry points, load addresses) is not covered by the signature. Unexplored.
- CORRECTION: BlockSDS DSWifi has a local multiplayer mode (host = access point with beacons, up to 15 clients, CMD/REPLY multiplayer transfers, no encryption): https://blocksds.skylyrac.net/dswifi/md_documentation_2local__multiplayer__mode.html . It also has raw TX/RX (Wifi_RawTxFrame, Wifi_RawSetPacketHandler).
- CORRECTION: the tight timing is on the host side too (the automatic CMD ACK), so an ESP32 host has to send that ACK within microseconds of the replies. How strictly a DS enforces it is unknown.
- ESP32 esp_wifi_80211_tx sends beacons, probe req/resp, action and non-QoS data frames only (no auth/assoc frames): https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32/api-guides/wifi-driver/wifi-vendor-features.html . So an ESP32 host would run its own AP for auth/assoc and add the Nintendo element, sending CMD frames raw.
- hostapd vendor_elements adds custom elements to beacons/probe responses, but the driver/firmware must support it (it didn't on Pi 3 brcmfmac in 2016): https://lists.infradead.org/pipermail/hostap/2016-June/035846.html . Untested on the Intel 7265.

## Hardware direction (2026-10-12)
- Sion front: Xbox-style controller ports (plain USB electrically). sokky likes their look and strength.
- Other systems through a USB multitap box rather than swappable port modules: a Pico with GameCube sockets (GC pads + GBA link on any port), with an Xbox plug on its cable. The first single-socket GBA dongle is version 1 of that box.
- The official Wii U GameCube adapter can't do GBA link: its firmware only polls controllers and reports inputs (USB commands: start polling, rumble). Electrically its ports would be fine.

## DS to Sion over Wi-Fi: parked plan (2026-10-15)

sokky has a DS flashcart and chose "not now", so nothing is built.
- Route 1 (recommended): the Sion's Intel 7265 hosts a separate open (or WEP) 2.4 GHz 802.11b-compatible network for the DS. hostapd is not in sion_bootani_defconfig yet; WEP needs hostapd built with CONFIG_WEP. Unchecked: whether the 7265 can run that AP while staying on the home Wi-Fi.
- DS side: BlockSDS/dswifi homebrew on the flashcart. Uses, in suggested order: (1) dslink-style program sending from the Sion, (2) DS as pad + map/status screen for xbcompat titles (~2 Mbps link, so only small low-rate frames), (3) local Wi-Fi Connection server (Wiimmfi/AltWFC-style) for retail DS online menus (inferred, untested).
- Route 2 (later, if ever): NiFi Download Play needs an AR9271 dongle; stock DSes only boot signed programs (e.g. kiosk demos broadcast unchanged), our own code needs FlashMe.

## GBA Wireless Adapter: research and Sion design (2026-10-16, plan only)

What's documented:
- Link-port protocol and commands: afska gba-link-connection docs/wireless_adapter.md
  (https://github.com/afska/gba-link-connection/blob/master/docs/wireless_adapter.md),
  https://blog.kuiper.dev/gba-wireless-adapter, GBATEK (login, commands, chip list),
  pret pokefirered decomp (Nintendo librfu). Wireless multiboot: game ID bit 15,
  client "RFU-MB-DL", ROM in 84-byte chunks.
- Emulation: gpSP libretro (davidgf, https://www.davidgf.net/2024/01/13/gba-wireless-adapter/). mGBA: roadmap only.
- Radio: Freescale MC13190 + CT3000/CT3001 MCU; FCC: ~4 Mbit/s ASK, TDMA. Proprietary, not 802.11.
  No air-protocol decode, no MCU firmware dump, no reflash known. rfu_firm.mb
  (douevenknow.us) is the GBA-side client the adapter multiboots, not adapter firmware.

Design: "Sion Link for GBA" fake adapter
- Pico W / Pico 2 W per GBA on the link port (3.3 V, no level shifter; cut link cable or breakout).
- Link side: emulate the adapter command set, so stock games and afska-based homebrew work.
- Air side: Pico W joins the Sion's sionds AP as a normal station (it hears Intel unicast,
  unlike the DS) and speaks sionlink common/slproto.h over UDP to sionlinkd (needs a UDP transport).
- Download Play: our own multiboot client served from the Pico to a cartless GBA; Sion sends ROMs.
- GBA-to-GBA: Sion relays between Pico Ws (4-player games).
- Test plan: run the adapter logic behind gpSP's RFU emulation in the cloud before hardware.
- Existing boards (2026-10-16): GBA-Global-Link (github zaksabeast, MIT, Pico wireless-adapter emulator over USB+web app, WIP) = recommended base; GB-Link (GPL-3.0, Pico link-cable-to-USB, no wireless-adapter mode; Crowd Supply board); GB-Link Switch LDN (AGPL, ESP32); GBLink_ESP32; Celio-Link online service.

## RG DS as a Sion Link client (2026-10-16, plan only)
- Hardware: RK3568 (4x A55, Mali-G52), 3 GB, two 4" 640x480 MIPI-DSI touch panels, RTL8821CS Wi-Fi/BT. Mainline Linux supports it (Armbian board merged 2026-06-11, kernel 7.0/7.1; mainline U-Boot 2026.01; out-of-tree aw87391 speaker driver + goodix fix). Boots from microSD; pulling the card returns to Android.
- OS: own Buildroot microSD image, KMS only (no X/Wayland), boots straight into the Sion Link client; one screen status, one the app.
- Link: joins the Sion's open sionds AP as a normal station; IPv6 link-local + multicast discovery; slproto over UDP. No router, DHCP or web page. Sion side = new UDP back end in sionlinkd (new files).
- Client: one native C/SDL2 client (Link Test, FFCC via the 3DS port's software GBA PPU). Optional interim: same client as a native sideloaded Android APK, never web.
- sokky chose plan only; nothing built.
- Graphics (2026-10-16): atomic KMS with VOP2 hardware planes; client draws GBA at 240x160 and the plane scales to 640x480 (which RK3568 plane types scale under mainline is unverified); vsync page flips; each screen its own CRTC. Panfrost GBM/EGL only for 3D/shader apps. No fbdev, X11 or Wayland.
- SDL3 (2026-10-16): suggested for the client: KMSDRM back end via GBM/EGL on Panfrost, one fullscreen window per screen (untested on these panels), evdev input/touch, ALSA; same code builds the Android APK. Costs a bit more power than planes; fallback = SDL3 without video + own planes path. (xbcompat itself stays SDL2.)
- Plane scaling (2026-10-16): SDL2 and SDL3 KMSDRM both use only the primary plane through the GPU. Pick: own small atomic-KMS layer for video + SDL (no video) for input/audio; libliftoff if many layers; 3D apps render low-res GLES into a GBM bo on a scaling plane; SDL kmsdrm patch possible; skip Weston/gamescope (Wayland). Check which VOP2 plane types scale on first boot.
