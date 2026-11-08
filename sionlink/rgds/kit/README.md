The RG DS kit's arm64 builds (the rest of the kit is in the repo already): copy
`ports/Sion Link.sh` and `ports/Sion DS Mirror.sh` into `roms/ports` on the card, and these,
with `../../mirror/kit/sion-link-live.nds` and `../slclient`, into `roms/ports/sionlink`.
`sionlink-app`: `make -C ../linkapp` for arm64; `mirrorrun`: `mirror/harness/build.sh` with
VIEW=1 for arm64 (see ../README.md), stripped.
