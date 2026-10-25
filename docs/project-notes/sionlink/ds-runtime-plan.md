# Sion Link: one DS app, a DS runtime, and Frolick hooks

sokky's direction (2026-10-23): one DS client app for linking and mirroring; Sion Link
functions get UI in Frolick (placement is sokky's call); DS games run as a *runtime* with a
wrapper frontend, like Virtual Console, the Game Boy Player, or Kazeta's runtimes.

## What exists today

| Piece | Where | State |
|---|---|---|
| Sion Link launcher (DS) | `sionlink/ds` on branch rb4kah | Wi-Fi link to the Sion, Download Play, Sion games with DS players |
| Mirror client (DS) | `sionlink/mirror/ds` on lkot3d (`sion-link-live.nds`) | mirror only, over DSpico USB |
| mirrorrun (Sion) | `sionlink/mirror/harness` | melonDS + TV + sound + mirror; waits for the DS before the game starts; no saves; DS pad is the only input |
| tv.sh (Sion) | `sionlink/mirror/kit/sion` | stops the dashboard, runs mirrorrun from /tmp, brings the dashboard back |
| Windows carts | xbcompat `wincart.c` + `wincart-run` + `kazeta-guide` (combined build, gudfcx) | the model to copy: a Kazeta cart shows up as a title in both dashboards, saves land in UDATA |

## 1. The DS runtime (Sion side)

A DS game is a Kazeta cart on the game card, the same way Windows games are:

```
Mario Kart DS/
  mkds.kzi      Name=Mario Kart DS  Id=mario-kart-ds  Runtime=nds  Exec=mkds.nds  Icon=icon.png
  mkds.nds
  icon.png
```

- **Shows up in both dashboards.** xbcompat's cart shim already turns `Runtime=windows*` carts
  into titles. It would take `Runtime=nds*` too and launch `dscart-run` instead of
  `wincart-run`. That's a small change to `wincart.c` on the combined build; I'd send it to the
  thread that installs xbcompat rather than install it myself.
- **`dscart-run`** takes the TV from xbox-av, runs the game, and gives the TV back when it ends
  (what tv.sh does now, minus stopping the dashboard: the dashboard launches it like any title).
- **The game starts right away.** A DS can plug in or drop out at any time. With no DS, Xbox
  pads play it: the right stick moves a stylus on the bottom screen and a click touches.
- **Saves** go to UDATA under the cart's title ID, so the Memory screen lists them.

## 2. The wrapper (the "Game Boy Player" part)

Holding Select/Back for 2 seconds opens a menu over the game, like the Game Boy Player or
Virtual Console (sokky, 2026-10-23). The Guide button stays the Guide and chorded actions.
A short press of Select still reaches the game at once; after 2 s the game's Select is let go
and the menu opens, so nothing waits on the hold. The game pauses while the menu is open. The
DS's own Select opens it too, and the DS client's L+R+Select status page moves into it.

- Screen layout: stacked, side by side, one big screen with the other small, swap screens.
- Border art behind the screens (per cart, or a default).
- DS link: connected or not, 30 or 60 fps on the DS.
- Sound in slow scenes: stretch or short gaps (the open card).
- Reset, quick save, quick load, and back to the Dashboard.

## 3. One DS app

`Sion Link` on the DSpico's SD card replaces both DS apps:

- **Home screen:** shows the Sion it's linked to, over USB when the DSpico cable is in,
  otherwise over Wi-Fi.
- **Mirror mode:** when the Sion starts a DS cart, it switches to the mirror by itself. When
  the game ends, it goes back home.
- **Kept from the Wi-Fi launcher:** Download Play and Sion games with DS players.
- **Limit:** the mirror needs USB. Wi-Fi from the Sion is broadcast only, too slow and lossy
  for both screens.

## 4. Frolick hooks (UI placement is sokky's)

A small Sion Link service on the Sion that any UI can ask for state and send commands to:

- linked DSes (USB / Wi-Fi), the running DS game, link quality;
- runtime defaults (layout, border, 30/60 on the DS, sound mode);
- send a program over Download Play.

Frolick only draws it. Nothing in Frolick changes until sokky says where it goes.

## Order

1. mirrorrun as a runtime: start without a DS, hot-plug the DS, pads with a stylus cursor, saves.
2. `dscart-run` plus the cart shim change; a DS cart launches from the dashboard.
3. The wrapper menu (hold Select/Back for 2 s).
4. The one DS app (merge launcher + mirror client).
5. The Sion Link service for Frolick.
6. Try melonDS's OpenGL renderer on the HD 610 (EGL/GBM): it frees the core the software 3D
   takes in heavy scenes. The link stays the same; captured pixels the mirror sends need
   copying back from the GPU. Measured on the Sion against run 13/14 before switching.

Steps 1, 3 and 4 are cloud work on lkot3d. Step 2 touches the combined xbcompat build, so it
goes to the thread that installs xbcompat. Nothing reaches the Sion until a kit is ready.
