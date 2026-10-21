# push-erofs

`erofs.ko` for the Sion's 6.18.7 kernel, so Kazeta `.kzr` runtimes (EROFS images)
can be mounted. Built from vanilla 6.18.7 with the Sion's kernel fragments plus
`CONFIG_EROFS_FS=m` (lz4, zstd, deflate and lzma). The Sion's kernel has no
MicroLZMA decoder, so a private copy of lib/xz's decoder is linked into erofs.ko
under renamed symbols (see `microlzma-bundle.txt`); `nm -u erofs.ko | grep xz_`
is empty. vermagic `6.18.7 SMP preempt mod_unload`.

`erofs.undef` lists every kernel symbol the module needs. Before loading it,
check them against the Sion's exports (read-only):

    ssh sion "grep ' __ksymtab_' /proc/kallsyms" | sed 's/.* __ksymtab_//' | sort -u > sion.exports
    comm -23 erofs.undef sion.exports

An empty result means it will load. Load with `insmod erofs.ko`.

To keep it across reboots: copy erofs.ko to /data/overlay/lib/modules/6.18.7/extra/;
push-fuse's S06fuse loads it at boot when present.
