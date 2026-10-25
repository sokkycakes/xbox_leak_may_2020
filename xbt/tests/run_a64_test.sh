#!/bin/sh
# Compare the encoder's output against aarch64 objdump.
set -e
D=$(mktemp -d)
cc -O1 -o "$D/a64_test" "$(dirname "$0")/a64_test.c"
"$D/a64_test" "$D/code.bin" "$D/expect.txt"
aarch64-linux-gnu-objdump -D -b binary -m aarch64 "$D/code.bin" | \
  grep -E '^ +[0-9a-f]+:' | sed -E 's/^ +[0-9a-f]+:\t[0-9a-f]+ \t//; s/\t/ /g; s/ +\/\/.*$//; s/ +$//' > "$D/got.txt"
python3 - "$D/expect.txt" "$D/got.txt" <<'PY'
import sys
e=open(sys.argv[1]).read().split('\n')
g=open(sys.argv[2]).read().split('\n')
bad=0
for i,(a,b) in enumerate(zip(e,g)):
    if a.strip()!=b.strip():
        bad+=1; print("MISMATCH %3d: expected %-40s got %s"%(i,a,b))
print("%d instructions, %d mismatches"%(len([x for x in e if x]),bad))
sys.exit(1 if bad else 0)
PY
