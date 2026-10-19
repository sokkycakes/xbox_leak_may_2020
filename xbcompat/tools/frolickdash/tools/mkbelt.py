#!/usr/bin/env python3
"""The GameTap game belt, packed for the Belt node.

    mkbelt.py beta2scene-belt.json out/belt.bin

beta2scene-belt.json is the belt taken out of GameTap's beta2scene.gr2
(art/gametap/game-belt-render.md): the 13 cover nodes gsb_0shape ..
gsb_12shape, each with its world matrix sampled at frames 1000..1020 of
the Belt_Right clip, and the 13 cover meshes gsb_NshapeShape bound to
them.  The JSON's mesh positions are in bind space, so a cover's vertex in
GameTap's world is worldMatrix * inverseBindMatrix * position; the file
carries each mesh in its bone's space (inverseBindMatrix applied) so the
node draws it with the sampled world matrix alone.

belt.bin, little-endian:
  "BLT3", int32 nodes (13), frames (21), verts per cover (42), indices
  per cover (180)
  float32 circle x, z, radius   the circle the twelve regular covers sit on
                                at frame 1000 (least squares in the xz
                                plane; the focused cover is pulled in)
  float32 cover height          the regular covers' centre y
  per cover:
    verts * (x y z nx ny nz u v) float32   the mesh in the bone's space
    indices * uint16                       60 triangles
    frames * 16 float32                    world matrices, column-major as
                                           the JSON (translation in 12..14)
"""
import json, math, struct, sys


def mat(m):
    def apply(p, w=1.0):
        x, y, z = p
        return (m[0] * x + m[4] * y + m[8] * z + m[12] * w,
                m[1] * x + m[5] * y + m[9] * z + m[13] * w,
                m[2] * x + m[6] * y + m[10] * z + m[14] * w)
    return apply


def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def length(a): return math.sqrt(sum(c * c for c in a))
def norm(a): l = length(a); return tuple(c / l for c in a) if l > 0 else a


def solve(N):
    """Gauss-Jordan on an augmented 3x4 system."""
    for i in range(3):
        piv = max(range(i, 3), key=lambda r: abs(N[r][i]))
        N[i], N[piv] = N[piv], N[i]
        for r in range(3):
            if r != i:
                f = N[r][i] / N[i][i]
                N[r] = [a - f * c for a, c in zip(N[r], N[i])]
    return [N[i][3] / N[i][i] for i in range(3)]


def main(src, out):
    d = json.load(open(src))
    nodes = sorted(d["nodes"], key=lambda n: n["index"])
    meshes = {m["boneBindings"][0]: m for m in d["meshes"]}
    nframes = len(nodes[0]["samples"])
    nverts = len(meshes[nodes[0]["name"]]["positions"])
    nidx = len(meshes[nodes[0]["name"]]["indices"])

    covers = []
    centres = []
    for i, n in enumerate(nodes):
        mesh = meshes[n["name"]]
        inv = mat(n["inverseBindMatrix"])
        pos = [inv(p) for p in mesh["positions"]]
        nrm = [norm(inv(v, 0.0)) for v in mesh["normals"]]
        assert len(pos) == nverts and len(mesh["indices"]) == nidx and len(n["samples"]) == nframes
        frames = [s["local"]["worldMatrix"] for s in n["samples"]]
        covers.append((pos, nrm, mesh["uvs"], mesh["indices"], frames))
        w = mat(frames[0])
        P = [w(p) for p in pos]
        c = tuple(sum(p[k] for p in P) / nverts for k in range(3))
        if i != 6:
            centres.append(c)
        if i == 6:
            near = c, length(sub(P[36], P[0])), length(sub(P[5], P[0]))

    # The ring the regular covers sit on: a least-squares circle through
    # their centres in the xz plane (x^2 + z^2 + a x + b z + c = 0), and the
    # height they sit at.
    N = [[0.0] * 4 for _ in range(3)]
    for x, y, z in centres:
        row, rhs = (x, z, 1.0), -(x * x + z * z)
        for i in range(3):
            for j in range(3):
                N[i][j] += row[i] * row[j]
            N[i][3] += row[i] * rhs
    a, b, c = solve(N)
    cx, cz = -a / 2, -b / 2
    radius = math.sqrt(cx * cx + cz * cz - c)
    cover_y = sum(p[1] for p in centres) / len(centres)

    blob = b"BLT3" + struct.pack("<4i", len(nodes), nframes, nverts, nidx)
    blob += struct.pack("<4f", cx, cz, radius, cover_y)
    for pos, nrm, uvs, idx, frames in covers:
        for i in range(nverts):
            blob += struct.pack("<8f", pos[i][0], pos[i][1], pos[i][2],
                                nrm[i][0], nrm[i][1], nrm[i][2], uvs[i][0], uvs[i][1])
        blob += struct.pack("<%dH" % nidx, *idx)
        for f in frames:
            blob += struct.pack("<16f", *f)
    open(out, "wb").write(blob)
    print("belt: %d covers, %d frames, circle (%.1f, %.1f) r %.1f at y %.1f, focused cover at (%.0f, %.0f, %.0f) %.0f x %.0f, %d bytes" % (
        len(nodes), nframes, cx, cz, radius, cover_y, near[0][0], near[0][1], near[0][2], near[1], near[2], len(blob)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
