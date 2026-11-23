"""Draws Quark's logo with Depict: docs/assets/logo.svg (the mark), logoTile.svg (the mark on its tile) and
docs/assets/src/logo.depict (the project, its guides kept as construction lines).

    python docs/assets/src/logo.py <path to depict.exe>

The mark is a Q drawn as one pen stroke along a chain of guides: circle arcs, each leaving the last one
tangentially, and a line for the tail. They were fitted to the hand-drawn reference, then the ring was
opened, the knot rounded and the stroke lightened. The pen presses as hard as WIDTH says along the way,
WEIGHT times over on the tile.
The tile is the family's (bestyle): a 512 square, inset 15.36, corners 112.64, #141417."""
import json, math, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.dirname(HERE)
SIZE = 512
TILE_INSET, TILE_RADIUS, TILE = 15.36, 112.64, "#141417"
TEAL = [[0, "#5EEAD4"], [0.5, "#14B8A6"], [1, "#0F766E"]]   # Quark's teal, light at the top left
SIDE = 372                                                  # the mark's longer side on the tile: round, as Cascade's C
WEIGHT = 1.5                                                # the drawing's light stroke, made to read as an icon down to 16 px

# The chain: where the pen starts, its heading (degrees, y down), then each guide: a circle's radius
# (negative: it turns left) and how far it turns, or a line's length.
START, HEADING = (393.003, 758.284), 203.797
CHAIN = [
    ("A, ring", 300.05, 263.588),
    ("B, into the knot", 115.62, 38.695),
    ("K, knot", 69.43, 69.392),
    ("C, crest", -658.67, -6.539),
    ("D, crest tightening", -123.41, -35.142),
    ("L, loop", -47.33, -117.267),
    ("E, ease", -918.42, -15.447),
    ("T, tail", None, 63.058),
]
# The stroke's width at even steps from start to end.
WIDTH = [3.3, 4.9, 7.0, 9.2, 11.3, 13.6, 15.6, 17.2, 18.6, 19.8, 20.9, 22.0, 23.0, 23.8, 24.6, 25.2, 26.0, 26.8, 27.5,
         28.1, 28.7, 29.2, 29.6, 29.9, 30.3, 30.7, 31.1, 31.4, 31.6, 31.8, 32.0, 32.0, 32.1, 32.1, 32.3, 32.3, 32.4,
         32.4, 32.4, 32.5, 32.6, 32.6, 32.7, 32.8, 32.8, 32.8, 32.8, 32.7, 32.7, 32.8, 32.8, 32.7, 33.0, 33.0, 32.6,
         32.4, 32.3, 32.2, 32.1, 32.0, 31.9, 31.8, 31.6, 31.4, 31.3, 31.3, 31.2, 31.1, 30.9, 30.8, 30.8, 30.8, 30.6,
         30.5, 30.6, 30.6, 30.6, 30.6, 30.6, 30.4, 30.3, 30.2, 30.1, 30.0, 30.0, 29.9, 29.9, 29.9, 29.8, 29.7, 29.6,
         29.5, 29.5, 29.5, 29.5, 29.5, 29.5, 29.5, 29.6, 29.6, 29.6, 29.5, 29.5, 29.5, 29.4, 29.2, 29.1, 29.1, 29.1,
         29.0, 29.0, 29.0, 29.0, 29.0, 29.0, 28.9, 28.7, 28.7, 28.6, 28.5, 28.3, 28.1, 27.9, 27.8, 27.8, 27.7, 27.6,
         27.4, 27.3, 27.1, 27.0, 26.9, 26.9, 26.8, 26.7, 26.7, 26.5, 26.3, 26.2, 26.0, 25.9, 25.9, 25.9, 25.7, 25.7,
         25.7, 25.6, 25.6, 25.9, 25.8, 25.4, 25.4, 25.4, 25.3, 25.4, 25.4, 25.4, 25.4, 25.3, 25.2, 25.2, 25.1, 24.9,
         24.8, 24.6, 24.5, 24.3, 23.9, 23.3, 22.5, 21.5, 20.6, 20.0, 19.9, 20.0, 20.2, 20.3, 20.5, 20.8, 21.3, 21.9,
         22.7, 23.4, 24.1, 24.8, 25.4, 26.0, 26.4, 26.7, 26.9, 27.1, 27.6, 28.0, 28.3, 28.7, 29.1, 29.4, 29.6, 29.5,
         28.9, 27.9, 26.8, 26.3, 26.2, 26.1, 25.8, 25.4, 25.0, 24.6, 24.2, 23.8, 23.5, 23.1, 22.8, 22.3, 21.9, 21.4,
         20.8, 20.4, 19.9, 19.4, 18.8, 18.1, 17.5, 16.7, 15.9, 15.0, 14.1, 13.2, 12.2, 11.3, 10.4, 9.4, 8.4, 7.6, 6.8,
         6.0, 5.1, 4.1, 3.1, 2.4]


def walk(step=1.5):
    """The pen's way along the chain, and each guide placed: (name, circle center and radius | line ends)."""
    x, y = START
    th = math.radians(HEADING)
    points, placed = [], []
    for name, radius, amount in CHAIN:
        if radius is None:
            n = max(2, int(amount / step))
            for i in range(1, n + 1):
                points.append((x + math.cos(th) * amount * i / n, y + math.sin(th) * amount * i / n))
            placed.append((name, "line", (x, y), (x + math.cos(th) * amount, y + math.sin(th) * amount)))
            x, y = points[-1]
            continue
        k = 1 / radius
        turn = math.radians(amount)
        cx, cy = x - math.sin(th) / k, y + math.cos(th) / k
        placed.append((name, "circle", (cx, cy), abs(radius), (x, y)))
        n = max(2, int(abs(turn / k) / step))
        for i in range(0 if not points else 1, n + 1):
            a = th + turn * i / n
            points.append((x + (math.sin(a) - math.sin(th)) / k, y + (-math.cos(a) + math.cos(th)) / k))
        x, y = points[-1]
        th += turn
    return points, placed


def width_at(share):
    at = share * (len(WIDTH) - 1)
    i = min(int(at), len(WIDTH) - 2)
    return WIDTH[i] + (WIDTH[i + 1] - WIDTH[i]) * (at - i)


def main():
    exe = sys.argv[1]
    points, placed = walk()
    # Onto the tile: its longer side SIDE, its box centered.
    reach = [WEIGHT * width_at(i / (len(points) - 1)) / 2 for i in range(len(points))]
    x0 = min(p[0] - r for p, r in zip(points, reach)); x1 = max(p[0] + r for p, r in zip(points, reach))
    y0 = min(p[1] - r for p, r in zip(points, reach)); y1 = max(p[1] + r for p, r in zip(points, reach))
    scale = SIDE / max(x1 - x0, y1 - y0)
    ox, oy = SIZE / 2 - (x0 + x1) / 2 * scale, SIZE / 2 - (y0 + y1) / 2 * scale
    def board(p): return (round(p[0] * scale + ox, 2), round(p[1] * scale + oy, 2))
    widths = [WEIGHT * width_at(i / (len(points) - 1)) * scale for i in range(len(points))]
    pen = math.ceil(max(widths))
    stroke = [[*board(p), round(max(0.04, w / pen), 3)] for p, w in zip(points, widths)]

    guide = "#E0486A"
    shapes = []
    for item in placed:
        if item[1] == "circle":
            name, _, c, r, at = item
            c, r = board(c), r * scale
            if r < 200:
                shapes.append({"kind": "ellipse", "name": name, "box": [round(c[0] - r, 2), round(c[1] - r, 2), round(2 * r, 2), round(2 * r, 2)]})
                shapes.append({"kind": "path", "name": name + ", radius", "path": [{"closed": False, "nodes": [list(c), list(board(at))]}]})
            else:  # too big to show whole: the stretch the stroke uses
                a0 = math.atan2(board(at)[1] - c[1], board(at)[0] - c[0])
                sweep = math.radians(dict((n, s) for n, _, s in CHAIN)[name]) * (1 if r > 0 else -1)
                more = 50 / r
                a1 = a0 + sweep + math.copysign(more, sweep); a0 -= math.copysign(more, sweep)
                nodes = [[round(c[0] + r * math.cos(a0 + (a1 - a0) * i / 24), 2), round(c[1] + r * math.sin(a0 + (a1 - a0) * i / 24), 2)] for i in range(25)]
                shapes.append({"kind": "path", "name": name, "path": [{"closed": False, "nodes": nodes}]})
        else:
            name, _, a, b = item
            shapes.append({"kind": "path", "name": name, "path": [{"closed": False, "nodes": [list(board(a)), list(board(b))]}]})
    for s in shapes:
        s.update({"guide": True, "fill": "none", "stroke": {"paint": guide, "width": 1}})

    side = SIZE - 2 * TILE_INSET
    ops = [{"op": "document", "name": "Quark", "width": SIZE, "height": SIZE, "transparent": True},
           {"op": "add", "object": {"kind": "rectangle", "name": "Tile", "box": [TILE_INSET, TILE_INSET, side, side],
                                    "radius": TILE_RADIUS, "fill": TILE, "stroke": "none"}}]
    ops += [{"op": "add", "object": s} for s in shapes]
    ops.append({"op": "brush", "points": stroke, "size": pen, "smoothing": 1.5, "tolerance": 0.2,
                "object": {"name": "Mark", "fill": {"type": "linear", "stops": TEAL, "from": [0, 0], "to": [1, 1]}}})
    for vertical, at in ((False, round(y0 * scale + oy, 2)), (False, round(y1 * scale + oy, 2)), (True, round(x0 * scale + ox, 2)), (True, round(x1 * scale + ox, 2))):
        ops.append({"op": "guide", "vertical": vertical, "position": round(at, 2)})

    project = os.path.join(HERE, "logo.depict")
    with tempfile.TemporaryDirectory() as scratch:
        def make(file, listing):
            path = os.path.join(scratch, "ops.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump(listing, f)
            subprocess.run([exe, "new", "--out", file, "--width", str(SIZE), "--height", str(SIZE), "--transparent"], check=True, capture_output=True)
            subprocess.run([exe, "apply", file, path], check=True, capture_output=True)
        make(project, ops)
        # The mark alone, on the same square: the family's -mark.svg.
        bare = os.path.join(scratch, "mark.depict")
        make(bare, [o for o in ops if o.get("object", {}).get("name") != "Tile"])
        subprocess.run([exe, "export", project, "--out", os.path.join(ASSETS, "logoTile.svg")], check=True, capture_output=True)
        subprocess.run([exe, "export", bare, "--out", os.path.join(ASSETS, "logo.svg")], check=True, capture_output=True)

if __name__ == "__main__":
    main()
