#!/usr/bin/env python3
"""Generate port/vr/gdx_vr_cabin_table.h: the VR cabin cockpit for each stock machine.

The cabin view seats the viewer inside their own machine. For every stock machine this measures the
LOD1 race mesh and emits:
  - the cabin volume (model units) whose hull triangles are not drawn in cockpit eyes;
  - the seat (eye position, model units);
  - the cabin geometry: floor, side walls, body-coloured sills, rear bulkhead, headrest, dash and
    gauges, each triangle tagged with a part (its colour role);
  - glass: the machine's own canopy triangles, drawn as tinted glass, where it has a canopy (27 of
    33). Twin Noritta has a cockpit in each hull: the cabin goes under one (TWIN_SIDE) and the other is
    left as drawn. Machines with no glass get a generic canopy frame and glass.

SOURCES: decomp/src/overlays/ovl_i9/machine_draw.c (Machine_Draw<Name>Lod1, replayed with the
material state: texture tile and primitive / environment colours), and the Vtx resources in the
user's fzerox.o2r. Only derived geometry is written out.

MODEL SPACE: model z is the machine's forward, y up, y = 0 the underside; the race model matrix
scales by 0.1 into world units. The canopy glass is the tile-0 group with a light-blue primitive and a
blue-dominant environment colour (two palettes across the roster).

Usage: tools/gen_vr_cabin_table.py --o2r path/to/fzerox.o2r [--out port/vr/gdx_vr_cabin_table.h]
"""
import argparse
import math
import re
import struct
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Parts: the C side maps each to a colour (BODY uses the racer's body colour).
PARTS = ["FLOOR", "WALL", "BODY", "BULKHEAD", "HEADREST", "DASH", "GAUGE_CYAN", "GAUGE_AMBER", "GAUGE_RED",
         "FRAME", "GLASS_OWN", "GLASS_GENERIC", "LAMP_ENERGY", "LAMP_BEHIND", "LAMP_BOOST"]
P = {name: i for i, name in enumerate(PARTS)}

CMD = re.compile(
    r"gSPVertex\(gfx\+\+, ([^,]+), (\d+), (\d+)\)"
    r"|gSP2Triangles\(gfx\+\+, (\d+), (\d+), (\d+), \d+, (\d+), (\d+), (\d+), \d+\)"
    r"|gSP1Triangle\(gfx\+\+, (\d+), (\d+), (\d+), \d+\)"
    r"|gSPTexture\(gfx\+\+, [^,]+, [^,]+, \d+, (\d+), G_ON\)"
    r"|gDPSetPrimColor\(gfx\+\+, \d+, \d+, (\d+), (\d+), (\d+), \d+\)"
    r"|gDPSetEnvColor\(gfx\+\+, ([^,]+), ([^,]+), ([^,]+), \d+\)"
)


class Meshes:
    def __init__(self, o2r):
        self.src = (REPO / "decomp/src/overlays/ovl_i9/machine_draw.c").read_text()
        racer = (REPO / "decomp/src/game/racer.c").read_text()
        t = racer[racer.index("sMachineDrawFuncs[][5]"):]
        self.order = re.findall(r"\{ Machine_Draw(\w+?)Lod1,", t[: t.index("};")])
        self.archive = zipfile.ZipFile(o2r)
        self.names = {n.rsplit("/", 1)[-1]: n for n in self.archive.namelist()}
        self.cache = {}

    def vtx(self, sym):
        if sym not in self.cache:
            d = self.archive.read(self.names[sym])
            if d[4:8] != b"XTVO":
                raise ValueError(f"{sym} is not a Vtx resource")
            n = struct.unpack_from("<I", d, 0x40)[0]
            self.cache[sym] = [struct.unpack_from("<hhh", d, 0x44 + 16 * i) for i in range(n)]
        return self.cache[sym]

    def triangles(self, name):
        start = self.src.index(f"Gfx* Machine_Draw{name}Lod1(Gfx* gfx) {{")
        text = self.src[start: self.src.index("\n}\n", start)]
        cache = [None] * 64
        tile, prim, env = 0, (255, 255, 255), None
        out = []
        for m in CMD.finditer(text):
            g = m.groups()
            if g[0]:
                ref = re.match(r"&?(D_\w+)(?:\[(\w+)\]| \+ (\w+))?$", g[0].strip())
                if not ref:
                    raise ValueError(f"unparsed vertex reference {g[0]!r}")
                off = int(ref.group(2) or ref.group(3) or "0", 0)
                n, first = int(g[1]), int(g[2])
                cache[first: first + n] = self.vtx(ref.group(1))[off: off + n]
            elif g[3] or g[9]:
                for tri in ([g[3:6], g[6:9]] if g[3] else [g[9:12]]):
                    out.append({"v": [cache[int(i)] for i in tri], "tile": tile, "prim": prim, "env": env})
            elif g[12]:
                tile = int(g[12])
            elif g[13]:
                prim = tuple(int(x) for x in g[13:16])
            elif g[16]:
                try:
                    env = tuple(int(x) for x in g[16:19])
                except ValueError:
                    env = None  # the racer's body colour
        return out


def is_glass(t):
    return t["tile"] == 0 and t["env"] is not None and t["prim"][2] > 200 and t["env"][2] > t["env"][0]


def surface_height(tris, x, z):
    best = None
    for a, b, c in (t["v"] for t in tris):
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


# --- geometry helpers ---------------------------------------------------------------------------
def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def unit(v):
    n = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
    return (v[0] / n, v[1] / n, v[2] / n) if n > 1e-9 else (0.0, 1.0, 0.0)


def quad(out, part, a, b, c, d):
    """One quad as two triangles (the C side draws every triangle double-sided)."""
    out.append((part, (a, b, c)))
    out.append((part, (a, c, d)))


# Dash shape, model units: the face runs from DASH_BOTTOM to DASH_BOTTOM + DASH_RISE about the sill,
# leaning back DASH_LEAN_DEG from vertical; a DASH_SHELF-deep shelf tops it. (First look: the earlier
# near-horizontal dash should face the player.)
DASH_BOTTOM, DASH_RISE, DASH_LEAN_DEG, DASH_SHELF = -12.0, 18.0, 25.0, 10.0


# The annunciator pod: a block rising from the middle of the dash shelf with two lamps on its face,
# "CHECK ENGINE" (red, low energy) on the viewer's left and "BOOST READY" (green) on the right. The
# face is turned to the seat so both read at a glance without turning the head (wear feedback
# 2026-10-05: the sill lamps were too far out to read at speed). The lamp faces, lettering included,
# are the VR host's quads at the corners emitted here (lampTile): Vtx whole units are too coarse
# for them, and lettering as geometry needed a sub-unit scale matrix the gfx bridge dropped.
POD_WIDTH, POD_HEIGHT, POD_DEPTH = 0.66, 4.2, 5.0  # width in cabin half-widths; model units
POD_LAMP_LIFT = 0.3


def facing_up(c, eye, x):
    """Unit up axis of a panel at c with text axis x, its face turned toward the eye."""
    n = unit(sub(eye, c))
    y = unit(cross(n, x))
    return y if y[1] >= 0 else tuple(-v for v in y)


def pod_frame(cab, seat):
    """Centre of the pod face, its text axis (the viewer's right), up axis and normal (toward the
    seat). Centred on the cabin axis: cabin_geometry's output is shifted by cx afterwards."""
    hw, sill, z0, z1 = cab["hw"], cab["sill"], cab["z0"], cab["z1"]
    zd = z1 - 0.35 * (z1 - z0)
    yt = sill + DASH_BOTTOM + DASH_RISE
    zt = zd + DASH_RISE * math.tan(math.radians(DASH_LEAN_DEG))
    eye = (seat[0] - cab["cx"], seat[1], seat[2])
    x = (-1.0, 0.0, 0.0)
    back = (0.0, yt + POD_HEIGHT / 2, zt + 2.5 + POD_DEPTH)  # the pod's back edge, on the shelf
    n = unit(sub(eye, back))
    y = facing_up(back, eye, x)
    face = tuple(back[i] + n[i] * POD_DEPTH for i in range(3))
    return face, x, y, n


def pod_lamp(cab, seat, k):
    """Lamp k's centre (0: left, 1: right), width and height on the pod face."""
    face, x, y, n = pod_frame(cab, seat)
    pw = POD_WIDTH * cab["hw"]
    # Just proud of the face (stored in whole units), so the quad sits on it with no visible gap.
    c = tuple(face[i] + x[i] * (k - 0.5) * 0.5 * pw + n[i] * POD_LAMP_LIFT for i in range(3))
    return c, 0.47 * pw, 0.72 * POD_HEIGHT


def pod_lamp_corners(cab, seat, k):
    """Lamp k's corners (top-left, top-right, bottom-right, bottom-left as the viewer sees it)."""
    _, x, y, _ = pod_frame(cab, seat)
    c, w, h = pod_lamp(cab, seat, k)
    return [tuple(c[i] + x[i] * du + y[i] * dv for i in range(3))
            for du, dv in ((-w / 2, h / 2), (w / 2, h / 2), (w / 2, -h / 2), (-w / 2, -h / 2))]


def cabin_geometry(cab, generic, seat):
    hw, fl, sill, top, z0, z1 = cab["hw"], cab["floor"], cab["sill"], cab["top"], cab["z0"], cab["z1"]
    L = z1 - z0
    out = []
    quad(out, P["FLOOR"], (-hw, fl, z0), (hw, fl, z0), (hw, fl, z1), (-hw, fl, z1))
    for s in (-1, 1):
        x = s * hw
        quad(out, P["WALL"], (x, fl, z0), (x, fl, z1), (x, sill, z1), (x, sill, z0))
        xi = s * (hw - 8)
        quad(out, P["BODY"], (x, sill, z0), (x, sill, z1), (xi, sill, z1), (xi, sill, z0))         # sill top
        quad(out, P["BODY"], (xi, sill, z0), (xi, sill, z1), (xi, sill - 6, z1), (xi, sill - 6, z0))  # sill lip
    quad(out, P["BULKHEAD"], (-hw, fl, z0), (hw, fl, z0), (hw, top, z0), (-hw, top, z0))
    zr = z0 + 0.12 * L
    hr = 0.35 * hw
    quad(out, P["HEADREST"], (-hr, sill - 6, zr + 8), (hr, sill - 6, zr + 8), (hr, top - 12, zr + 8), (-hr, top - 12, zr + 8))
    quad(out, P["HEADREST"], (-hr, top - 12, zr), (hr, top - 12, zr), (hr, top - 12, zr + 8), (-hr, top - 12, zr + 8))
    # The dash: a face towards the viewer, leaning back DASH_LEAN_DEG from vertical (DASH_RISE tall),
    # topped by a shelf. The game's own energy meter, timer, minimap and speed are shown on the face
    # (the VR host places crops of the race HUD there; the C++ side's kDashFaceDeg must match).
    zd = z1 - 0.35 * L
    yb, yt = sill + DASH_BOTTOM, sill + DASH_BOTTOM + DASH_RISE
    zt = zd + DASH_RISE * math.tan(math.radians(DASH_LEAN_DEG))
    quad(out, P["DASH"], (-hw, yb, zd), (hw, yb, zd), (hw, yt, zt), (-hw, yt, zt))
    quad(out, P["DASH"], (-hw, yt, zt), (hw, yt, zt), (hw, yt, zt + DASH_SHELF), (-hw, yt, zt + DASH_SHELF))
    quad(out, P["WALL"], (-hw, fl, zd), (hw, fl, zd), (hw, yb, zd), (-hw, yb, zd))  # below the face
    # The annunciator pod on the shelf (pod_frame): its face, top and sides, and the two lamps.
    face, x, y, n = pod_frame(cab, seat)
    pw = POD_WIDTH * hw
    def corner(c, du, dv, dn=0.0, axis_v=y):
        return tuple(c[i] + x[i] * du + axis_v[i] * dv + n[i] * dn for i in range(3))
    h2 = POD_HEIGHT / 2
    quad(out, P["DASH"], corner(face, -pw / 2, h2), corner(face, pw / 2, h2), corner(face, pw / 2, -h2),
         corner(face, -pw / 2, -h2))
    quad(out, P["BULKHEAD"], corner(face, -pw / 2, h2, -POD_DEPTH), corner(face, pw / 2, h2, -POD_DEPTH),
         corner(face, pw / 2, h2), corner(face, -pw / 2, h2))
    for sgn in (-1, 1):
        quad(out, P["DASH"], corner(face, sgn * pw / 2, h2), corner(face, sgn * pw / 2, h2, -POD_DEPTH),
             corner(face, sgn * pw / 2, -h2, -POD_DEPTH), corner(face, sgn * pw / 2, -h2))
    # The lamp faces are the VR host's quads (lampTile), not triangles here.
    # LAMP_BEHIND has no geometry any more: the racer-behind warning is the mirror's own frame,
    # drawn by the VR host, so it can't drift out of line with the mirror.
    if generic:
        zf = z1 - 4
        zn = z1 + 0.25 * L
        for s in (-1, 1):
            x = s * (hw - 2)
            quad(out, P["FRAME"], (x - 2, top, zr), (x + 2, top, zr), (x + 2, top, zf), (x - 2, top, zf))
            quad(out, P["FRAME"], (x, top - 3, zr), (x, top, zr), (x, top, zf), (x, top - 3, zf))
            quad(out, P["FRAME"], (x - 2, top, zf), (x + 2, top, zf), (s * hw + 2, sill + 3, zn), (s * hw - 2, sill + 3, zn))
            quad(out, P["FRAME"], (x, sill + 2, zr), (x, top, zr), (x, top, zr + 4), (x, sill + 2, zr + 4))
        quad(out, P["FRAME"], (-hw + 2, top - 3, zr), (hw - 2, top - 3, zr), (hw - 2, top, zr), (-hw + 2, top, zr))
        quad(out, P["GLASS_GENERIC"], (-hw, top, zr), (hw, top, zr), (hw, top, zf), (-hw, top, zf))
        quad(out, P["GLASS_GENERIC"], (-hw, top, zf), (hw, top, zf), (hw, sill + 3, zn), (-hw, sill + 3, zn))
    return out


# Twin Noritta has a cockpit in each hull; the viewer takes this side (model +x, on the viewer's left
# when facing forward). The other cockpit is left as the game draws it.
TWIN_SIDE = 1


def place(tris):
    glass = [t for t in tris if is_glass(t)]
    crosses = any(min(p[0] for p in t["v"]) <= 0 <= max(p[0] for p in t["v"]) for t in glass)
    if glass and not crosses:
        # One canopy per hull: keep the chosen hull's, and centre the cabin on it.
        glass = [t for t in glass if sum(p[0] for p in t["v"]) * TWIN_SIDE > 0]
    single = bool(glass)
    cx = 0.0
    if single:
        pts = [p for t in glass for p in t["v"]]
        lo = [min(p[i] for p in pts) for i in range(3)]
        hi = [max(p[i] for p in pts) for i in range(3)]
        cx = 0.0 if crosses else 0.5 * (lo[0] + hi[0])
        hw = min(max(hi[0] - cx, cx - lo[0]) * 0.92, 75.0)
        z0, z1 = lo[2] - 20, lo[2] + 0.55 * (hi[2] - lo[2])
        top = hi[1]
        sill = lo[1] + 0.25 * (hi[1] - lo[1])
    else:  # no glass: sink the cabin into the roof just ahead of the machine's origin
        zc, hw = 20.0, 45.0
        top = max((surface_height(tris, x, z) or 0.0) for x in range(-40, 41, 10) for z in range(-40, 81, 10)) + 4
        z0, z1 = zc - 60, zc + 60
        sill = top - 35
    # The seat sits a third of the way forward from the rear bulkhead, behind the dash (which starts
    # 65% of the way forward), with the eye a little over halfway from sill to canopy top.
    seat = (cx, sill + 0.55 * (top - sill), z0 + 0.32 * (z1 - z0))
    cab = {"cx": cx, "hw": hw, "floor": sill - 40, "sill": sill, "top": top, "z0": z0, "z1": z1}
    return cab, single, glass, seat


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--o2r", required=True, type=Path)
    ap.add_argument("--out", type=Path, default=REPO / "port/vr/gdx_vr_cabin_table.h")
    args = ap.parse_args()
    m = Meshes(args.o2r)

    machines, tris_out, cut_glass = [], [], []
    for name in m.order:
        tris = m.triangles(name)
        cab, single, glass, seat = place(tris)
        geom = [(part, tuple((v[0] + cab["cx"], v[1], v[2]) for v in tri))
                for part, tri in cabin_geometry(cab, not single, seat)]
        if single:
            glass_rgb = tuple(int(0.65 * e + 0.35 * p) for e, p in zip(glass[0]["env"], glass[0]["prim"]))
            for t in glass:
                geom.append((P["GLASS_OWN"], tuple(tuple(float(c) for c in v) for v in t["v"])))
        else:
            glass_rgb = (150, 200, 235)
        tri_start, glass_start = len(tris_out), len(cut_glass)
        for part, (a, b, c) in geom:
            n = unit(cross(sub(b, a), sub(c, a)))
            tris_out.append((part, a, b, c, n))
        for t in glass:  # every glass triangle is replaced: by GLASS_OWN, or by the generic frame
            cut_glass.append(tuple(round(sum(v[i] for v in t["v"]) / 3) for i in range(3)))
        tiles = []
        for k in range(2):
            tiles.append([(v[0] + cab["cx"], v[1], v[2]) for v in pod_lamp_corners(cab, seat, k)])
        machines.append({"name": name, "kind": 1 if single else 2, "cab": cab, "seat": seat, "tiles": tiles,
                         "tri": (tri_start, len(tris_out) - tri_start),
                         "glass": (glass_start, len(cut_glass) - glass_start), "rgb": glass_rgb})

    r = lambda v: int(round(v))
    lines = [
        "// Generated by tools/gen_vr_cabin_table.py from the stock LOD1 machine meshes. Do not edit.",
        "// The VR cabin cockpit per machine, in model units (the race model matrix scales by 0.1): the",
        "// volume whose hull triangles are not drawn in cockpit eyes, the seat, the cabin geometry and the",
        "// centres of the canopy glass triangles it replaces. C, included by port/decomp_port.c only.",
        "#ifndef GDX_VR_CABIN_TABLE_H",
        "#define GDX_VR_CABIN_TABLE_H",
        "",
        "enum {",
    ] + [f"    GDX_CABIN_PART_{p} = {i}," for i, p in enumerate(PARTS)] + [
        f"    GDX_CABIN_PART_COUNT = {len(PARTS)}",
        "};",
        "",
        "typedef struct GdxCabinTri {",
        "    short v[3][3];",
        "    signed char n[3]; /* unit normal * 127, from the winding (the C side draws both sides) */",
        "    unsigned char part;",
        "} GdxCabinTri;",
        "",
        "typedef struct GdxCabinMachine {",
        "    unsigned char kind; /* 1: own canopy as glass, 2: generic frame and glass */",
        "    short box[6];       /* cut volume: x0, x1, y0, y1, z0, z1 */",
        "    short seat[3];      /* eye */",
        "    unsigned short triStart, triCount;",
        "    unsigned short glassStart, glassCount; /* into kGdxCabinGlass: replaced canopy triangles */",
        "    unsigned char glassRgb[3];",
        "    short dash[3];      /* centre of the dash face, which leans back 25 degrees from vertical */",
        "    short dashHalfWidth;",
        "    /* the pod lamps (0: CHECK ENGINE, 1: BOOST READY), drawn by the VR host as quads: corners,",
        "       top-left first, clockwise as the viewer sees them */",
        "    float lampTile[2][4][3];",
        "} GdxCabinMachine;",
        "",
        f"#define GDX_CABIN_MACHINE_COUNT {len(machines)} /* sMachineDrawFuncs order: 30 pilots, then super machines */",
        "static const GdxCabinMachine kGdxCabinMachines[GDX_CABIN_MACHINE_COUNT] = {",
    ]
    for mc in machines:
        c = mc["cab"]
        box = (r(c["cx"] - c["hw"]), r(c["cx"] + c["hw"]), r(c["floor"]), r(c["top"]) + 30, r(c["z0"]), r(c["z1"]))
        lines.append(f"    {{ {mc['kind']}, {{ {', '.join(map(str, box))} }}, {{ {', '.join(str(r(v)) for v in mc['seat'])} }}, "
                     f"{mc['tri'][0]}, {mc['tri'][1]}, {mc['glass'][0]}, {mc['glass'][1]}, "
                     f"{{ {', '.join(map(str, mc['rgb']))} }}, "
                     f"{{ {r(c['cx'])}, {r(c['sill'] + DASH_BOTTOM + DASH_RISE / 2)}, "
                     f"{r(c['z1'] - 0.35 * (c['z1'] - c['z0']) + DASH_RISE / 2 * math.tan(math.radians(DASH_LEAN_DEG)))} }}, "
                     f"{r(c['hw'])}, {{ "
                     + ", ".join("{ " + ", ".join("{ " + ", ".join(f"{v:.3f}f" for v in vec) + " }" for vec in t) + " }"
                                 for t in mc["tiles"]) + f" }} }}, /* {mc['name']} */")
    lines += ["};", "", f"static const GdxCabinTri kGdxCabinTris[{len(tris_out)}] = {{"]
    for part, a, b, c, n in tris_out:
        vs = ", ".join("{ " + ", ".join(str(r(x)) for x in v) + " }" for v in (a, b, c))
        lines.append(f"    {{ {{ {vs} }}, {{ {', '.join(str(r(x * 127)) for x in n)} }}, {part} }},")
    lines += ["};", "", f"static const short kGdxCabinGlass[{max(1, len(cut_glass))}][3] = {{"]
    lines += [f"    {{ {', '.join(map(str, g))} }}," for g in cut_glass] or ["    { 0, 0, 0 },"]
    lines += ["};", "", "#endif", ""]
    args.out.write_text("\n".join(lines))
    singles = sum(1 for mc in machines if mc["kind"] == 1)
    print(f"wrote {args.out}: {len(machines)} machines ({singles} own canopy), {len(tris_out)} triangles, {len(cut_glass)} glass")


if __name__ == "__main__":
    main()
