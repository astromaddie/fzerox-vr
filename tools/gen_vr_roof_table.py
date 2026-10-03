#!/usr/bin/env python3
"""Generate port/vr/gdx_vr_roof_table.h: how high each stock machine's roof is, for the VR cockpit view.

The cockpit eye sits on the machine's roof. Roof heights differ by a factor of six across the roster
(Twin Noritta's centreline is ~3.5 world units up, Black Bull's ~21), so a single offset either buries
the eye inside the tall bodies or floats it over the low ones. This measures each machine's LOD1 race
mesh and records, for a row of eye positions along the machine's forward axis, the highest point of
the body the eye has to see over.

SOURCES (both user-supplied or in-repo; nothing extracted is written out, only the derived heights):
  - decomp/src/overlays/ovl_i9/machine_draw.c: Machine_Draw<Name>Lod1 for each machine, in the order of
    sMachineDrawFuncs (decomp/src/game/racer.c): 30 pilots, then the super machines. Each function is
    a literal list of gSPVertex / gSP1Triangle / gSP2Triangles, so the triangles are rebuilt by
    replaying the vertex cache.
  - fzerox.o2r (the archive gdx-extract builds from the user's ROM): the Vtx resources those calls
    name. Resource layout: 0x40-byte header, u32 count at 0x40, then 16-byte little-endian Vtx with
    s16 x, y, z first.

MODEL SPACE: the race model matrix (Matrix_ScaleFrom3DMatrix, racer.c Racer_Draw) scales by 0.1 and
maps model z to the travel basis x (forward) and model y to its y (up). Model y = 0 is the underside.

MEASUREMENT: for an eye at model (0, ?, zEye), the roof height is the highest triangle surface over
the strip |x| <= HALF_WIDTH, z >= zEye - BEHIND, sampled on a SAMPLE_STEP grid. Everything ahead of
the eye is included so the view clears a canopy or fin in front of it. The table holds that height in
world units (model units * 0.1) at each forward offset in FORWARD_SAMPLES.

Usage: tools/gen_vr_roof_table.py --o2r path/to/fzerox.o2r [--out port/vr/gdx_vr_roof_table.h]
"""
import argparse
import re
import struct
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MODEL_SCALE = 0.1
FORWARD_SAMPLES = [f * 2.0 for f in range(-6, 7)]  # world units, -12 .. +12
HALF_WIDTH = 40   # model units either side of the centreline
BEHIND = 20       # model units behind the eye still counted as under it
SAMPLE_STEP = 10  # model units

VERTEX_RE = re.compile(r"gSPVertex\(gfx\+\+, ([^,]+), (\d+), (\d+)\)")
TRI2_RE = re.compile(r"gSP2Triangles\(gfx\+\+, (\d+), (\d+), (\d+), \d+, (\d+), (\d+), (\d+), \d+\)")
TRI1_RE = re.compile(r"gSP1Triangle\(gfx\+\+, (\d+), (\d+), (\d+), \d+\)")
CMD_RE = re.compile("|".join(r.pattern for r in (VERTEX_RE, TRI2_RE, TRI1_RE)))


def machine_order(racer_c):
    table = racer_c[racer_c.index("sMachineDrawFuncs[][5]"):]
    table = table[:table.index("};")]
    return re.findall(r"\{ Machine_Draw(\w+?)Lod1,", table)


def load_vertices(archive, names, symbol):
    data = archive.read(names[symbol])
    if data[4:8] != b"XTVO":
        raise ValueError(f"{symbol} is not a Vtx resource")
    count = struct.unpack_from("<I", data, 0x40)[0]
    return [struct.unpack_from("<hhh", data, 0x44 + 16 * i) for i in range(count)]


def triangles(body, vertex_data):
    cache = [None] * 64
    out = []
    for m in CMD_RE.finditer(body):
        g = m.groups()
        if g[0]:
            ref = re.match(r"&?(D_\w+)(?:\[(\w+)\]| \+ (\w+))?$", g[0].strip())
            if not ref:
                raise ValueError(f"unparsed vertex reference {g[0]!r}")
            offset = int(ref.group(2) or ref.group(3) or "0", 0)
            verts = vertex_data(ref.group(1))
            count, first = int(g[1]), int(g[2])
            cache[first:first + count] = verts[offset:offset + count]
        elif g[3]:
            out.append([cache[int(i)] for i in g[3:6]])
            out.append([cache[int(i)] for i in g[6:9]])
        else:
            out.append([cache[int(i)] for i in g[9:12]])
    return out


def surface_height(tris, x, z):
    best = None
    for a, b, c in tris:
        det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if det == 0:
            continue
        l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det
        l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det
        l3 = 1.0 - l1 - l2
        if min(l1, l2, l3) < -1e-6:
            continue
        y = l1 * a[1] + l2 * b[1] + l3 * c[1]
        best = y if best is None else max(best, y)
    return best


def roof_height(tris, forward):
    z_eye = forward / MODEL_SCALE
    z_max = max(v[2] for t in tris for v in t)
    best = 0.0
    z = z_eye - BEHIND
    while z <= z_max:
        for x in range(-HALF_WIDTH, HALF_WIDTH + 1, SAMPLE_STEP):
            y = surface_height(tris, x, z)
            if y is not None:
                best = max(best, y)
        z += SAMPLE_STEP
    return best * MODEL_SCALE


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--o2r", required=True, type=Path)
    parser.add_argument("--out", type=Path, default=REPO / "port/vr/gdx_vr_roof_table.h")
    args = parser.parse_args()

    draw_c = (REPO / "decomp/src/overlays/ovl_i9/machine_draw.c").read_text()
    order = machine_order((REPO / "decomp/src/game/racer.c").read_text())
    archive = zipfile.ZipFile(args.o2r)
    names = {n.rsplit("/", 1)[-1]: n for n in archive.namelist()}
    vertex_cache = {}

    def vertex_data(symbol):
        if symbol not in vertex_cache:
            vertex_cache[symbol] = load_vertices(archive, names, symbol)
        return vertex_cache[symbol]

    rows = []
    for name in order:
        start = draw_c.index(f"Gfx* Machine_Draw{name}Lod1(Gfx* gfx) {{")
        body = draw_c[start:draw_c.index("\n}\n", start)]
        tris = triangles(body, vertex_data)
        rows.append((name, [roof_height(tris, f) for f in FORWARD_SAMPLES]))

    lines = [
        "// Generated by tools/gen_vr_roof_table.py from the stock LOD1 machine meshes. Do not edit.",
        "// Roof height (world units above the machine's underside) the VR cockpit eye must clear, per",
        "// machine and per eye position along the machine's forward axis (kRoofForward).",
        "#pragma once",
        "",
        "namespace gdx_vr {",
        "",
        f"constexpr int kRoofForwardCount = {len(FORWARD_SAMPLES)};",
        "constexpr float kRoofForward[kRoofForwardCount] = { "
        + ", ".join(f"{f:.1f}f" for f in FORWARD_SAMPLES) + " };",
        "",
        f"constexpr int kRoofMachineCount = {len(rows)}; // sMachineDrawFuncs order: 30 pilots, then super machines",
        "constexpr float kRoofHeight[kRoofMachineCount][kRoofForwardCount] = {",
    ]
    for name, heights in rows:
        lines.append("    { " + ", ".join(f"{h:.1f}f" for h in heights) + f" }}, // {name}")
    lines += ["};", "", "} // namespace gdx_vr", ""]
    args.out.write_text("\n".join(lines))
    print(f"wrote {args.out} ({len(rows)} machines)")


if __name__ == "__main__":
    main()
