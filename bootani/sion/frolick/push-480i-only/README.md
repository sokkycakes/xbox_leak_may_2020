# push-480i-only (Frolick as a game, E:\Games\Frolick)

Frolick built from claude/frolick-xap-dash-4fvafa 930632bf + the change in
frolick-480i-source.patch (c3025a60): Settings > Display > Resolution offers
only 480i (others grayed out), a kept mode also sets sion-video for the boot,
and xbox-av prefers the EDID's exact 2880x480i over the kernel's CVT one.

Files are the dist/game kit's changed ones; the sounds stay as installed.
On the Sion: copy everything to /tmp, then `sh /tmp/install-game.sh`.
Expected before (930632bf kit): Games/Frolick/default.xbe sha256 036bd0f1...
