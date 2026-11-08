savedcmd_trigger6.mod := printf '%s\n'   trigger6.o | awk '!x[$$0]++ { print("./"$$0) }' > trigger6.mod
