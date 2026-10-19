#!/usr/bin/env python3
"""The GameTap game belt, packed for the Belt node.

    mkbelt.py beta2scene-belt.json out/belt.bin

beta2scene-belt.json is the belt decoded from GameTap's beta2scene.gr2
(see art/gametap/game-belt-render.md): 13 cover nodes gsb_0shape ..
gsb_12shape, each with its world matrix sampled at frames 1000..1020 of
the Belt_Right clip, plus the cover meshes.  Every mesh is the same 7x6
vertex grid in its bone's space (mesh position times the bone's inverse
bind matrix), so the file carries it once.

belt.bin, little-endian:
  "BLT1", int32 nodes (13), frames (21), verts (42), indices (180),
  grid columns (7), grid rows (6)
  float32 ring centre x y z     where the covers face: GameTap's camera
  float32 near width, height    the focused cover (gsb_6shape) at frame 1000
  verts * (x y z u v) float32   the cover grid: vertex col*6+row, row 0 at
                                the bottom (v 0.83), row 5 at the top (v 0)
  indices * uint16              60 triangles
  nodes * frames * 16 float32   world matrices, column-major as the JSON
                                (translation in elements 12..14)
"""
import json, math, struct, sys


def mat(m):
    def apply(p):
        x, y, z = p
        return (m[0] * x + m[4] * y + m[8] * z + m[12],
                m[1] * x + m[5] * y + m[9] * z + m[13],
                m[2] * x + m[6] * y + m[10] * z + m[14])
    return apply


def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def length(a): return math.sqrt(sum(c * c for c in a))
def norm(a): l = length(a); return tuple(c / l for c in a)


def main(src, out):
    d = json.load(open(src))
    nodes = sorted(d["nodes"], key=lambda n: n["index"])
    meshes = {m["boneBindings"][0]: m for m in d["meshes"]}
    near = nodes[6]
    mesh = meshes[near["name"]]
    inv = mat(near["inverseBindMatrix"])
    locals_ = [inv(p) for p in mesh["positions"]]
    uvs = mesh["uvs"]
    indices = mesh["indices"]
    nverts = len(locals_)
    frames = [s["local"]["worldMatrix"] for s in near["samples"]]
    nframes = len(frames)

    # Where the covers face: the point nearest every cover's normal line at
    # frame 1000.  That is GameTap's camera, the centre of the ring.
    A = [[0.0] * 3 for _ in range(3)]
    b = [0.0] * 3
    for n in nodes:
        w = mat(n["samples"][0]["local"]["worldMatrix"])
        P = [w(l) for l in locals_]
        c = tuple(sum(p[i] for p in P) / nverts for i in range(3))
        nrm = norm(cross(sub(P[36], P[0]), sub(P[5], P[0])))
        for i in range(3):
            for j in range(3):
                M = (1 if i == j else 0) - nrm[i] * nrm[j]
                A[i][j] += M
                b[i] += M * c[j]
    M = [A[i] + [b[i]] for i in range(3)]
    for i in range(3):
        piv = max(range(i, 3), key=lambda r: abs(M[r][i]))
        M[i], M[piv] = M[piv], M[i]
        for r in range(3):
            if r != i:
                f = M[r][i] / M[i][i]
                M[r] = [a - f * c for a, c in zip(M[r], M[i])]
    ring = [M[i][3] / M[i][i] for i in range(3)]

    w = mat(frames[0])
    P = [w(l) for l in locals_]
    near_w, near_h = length(sub(P[36], P[0])), length(sub(P[5], P[0]))

    blob = b"BLT1" + struct.pack("<6i", len(nodes), nframes, nverts, len(indices), 7, 6)
    blob += struct.pack("<5f", ring[0], ring[1], ring[2], near_w, near_h)
    for i, (x, y, z) in enumerate(locals_):
        blob += struct.pack("<5f", x, y, z, uvs[i][0], uvs[i][1])
    blob += struct.pack("<%dH" % len(indices), *indices)
    for n in nodes:
        assert len(n["samples"]) == nframes
        for s in n["samples"]:
            blob += struct.pack("<16f", *s["local"]["worldMatrix"])
    open(out, "wb").write(blob)
    print("belt: %d covers, %d frames, ring centre (%.1f, %.1f, %.1f), focused cover %.0f x %.0f, %d bytes" % (
        len(nodes), nframes, ring[0], ring[1], ring[2], near_w, near_h, len(blob)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
