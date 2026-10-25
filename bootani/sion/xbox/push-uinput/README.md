# push-uinput

`uinput.ko` for the Sion's 6.18.7 kernel (CONFIG_INPUT_UINPUT=m, same tree and
fragments as push-fuse). Gives /dev/uinput, used by the Windows-cart host to feed
Wine a virtual gamepad. Check `uinput.undef` against the Sion's exports as in
push-erofs/README.md, then `insmod uinput.ko`; for boot, copy it to
/data/overlay/lib/modules/6.18.7/extra/ (S06fuse loads it when present).
