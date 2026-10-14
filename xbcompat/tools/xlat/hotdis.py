#!/usr/bin/env python3
"""Disassembles an XLAT_COUNT hot.txt: each block's x86 code and the ARM code
xlat made for it.  Needs objdump for i386 and arm-linux-gnueabihf-objdump.
usage: hotdis.py hot.txt [blocks]"""
import os, subprocess, sys, tempfile

def dis(data, args):
    with tempfile.NamedTemporaryFile(suffix='.bin') as f:
        f.write(data)
        f.flush()
        out = subprocess.run(args + [f.name], capture_output=True, text=True).stdout
    lines = out.split('<.data>:\n', 1)[-1].splitlines()
    return '\n'.join(l for l in lines if l.strip())

def main():
    path, limit = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 10
    armdump = os.environ.get('ARMDUMP', 'arm-linux-gnueabihf-objdump')
    blocks, cur = [], None
    for line in open(path):
        if line.startswith('x86_'):
            cur = {'head': line.strip(), 'addr': int(line[4:12], 16)}
            blocks.append(cur)
        elif line.startswith('x86:') and cur:
            cur['x86'] = bytes(int(b, 16) for b in line.split()[1:])
        elif line.startswith('arm:') and cur:
            cur['arm'] = b''.join(int(w, 16).to_bytes(4, 'little') for w in line.split()[1:])
    for b in blocks[:limit]:
        print('==', b['head'])
        print(dis(b.get('x86', b''), ['objdump', '-D', '-b', 'binary', '-mi386', '--adjust-vma=%#x' % b['addr']]))
        print('--')
        print('\n'.join(l.split('\t', 2)[-1] for l in dis(b.get('arm', b''), [armdump, '-D', '-b', 'binary', '-marm']).splitlines()))

main()
