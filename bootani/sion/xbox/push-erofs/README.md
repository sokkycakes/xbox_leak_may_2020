# push-erofs

`erofs.ko` for the Sion's 6.18.7 kernel, so Kazeta `.kzr` runtimes (EROFS images)
can be mounted. Built from vanilla 6.18.7 with the Sion's kernel fragments plus
`CONFIG_EROFS_FS=m` (lz4, zstd and deflate; no lzma, because the Sion's kernel has
no MicroLZMA decoder). vermagic `6.18.7 SMP preempt mod_unload`.

`erofs.undef` lists every kernel symbol the module needs. Before loading it,
check them against the Sion's exports (read-only):

    ssh sion "grep ' __ksymtab_' /proc/kallsyms" | sed 's/.* __ksymtab_//' | sort -u > sion.exports
    comm -23 erofs.undef sion.exports

An empty result means it will load. Load with `insmod erofs.ko`.
