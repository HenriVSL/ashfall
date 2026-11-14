#!/usr/bin/env python3
"""summarize.py - turn probe_w.log / probe_z.log (probe.exe "probe: ..." lines) into the Markdown tables of
RESULTS.md. Usage: python3 probe/summarize.py [probe/probe_w.log probe/probe_z.log] > /tmp/tables.md (T02)."""
import os, re, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))


def parse(path):
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path, encoding="latin-1"):
        m = re.search(r"probe: (s\d) (\S+)(.*)$", line.strip())
        if not m:
            continue
        kind = m.group(2) if "=" not in m.group(2) else ""
        kv = dict(re.findall(r"(\w+)=(\S+)", m.group(2) + m.group(3)))
        kv["_scene"], kv["_kind"] = m.group(1), kind
        rows.append(kv)
    return rows


def main():
    logs = sys.argv[1:] or [os.path.join(HERE, "probe_w.log"), os.path.join(HERE, "probe_z.log")]
    rows = []
    for p in logs:
        rows += parse(p)
    out = []
    # scene 1
    out.append("### Scene 1 coastline (err = pixels differing from the painter's reference; flips = error pixels "
               "that change between two frames 1/30 s apart at 250 m/s)\n")
    out.append("| depth | far | near | 1 km | 3 km | 6 km | 12 km | 17 km |")
    out.append("|---|---|---|---|---|---|---|---|")
    g = defaultdict(dict)
    for r in rows:
        if r["_scene"] == "s1" and "dist" in r:
            key = (r["mode"], r["far"], r["near"])
            g[key][r["dist"]] = "%s/%s (f %s)" % (r["err_a"], r["err_b"], r["flips"])
    for key in sorted(g):
        out.append("| %s | %s | %s | %s |" % (key[0], key[1], key[2], " | ".join(g[key].get(d, "-") for d in
                                                                         ["1000", "3000", "6000", "12000", "17000"])))
    # scene 2
    out.append("\n### Scene 2 parallel quads (leak % of the near quad's pixels overwritten by the far quad)\n")
    out.append("| depth | far | near | orient | sep | 100 m | 1 km | 5 km | 10 km | 17 km |")
    out.append("|---|---|---|---|---|---|---|---|---|---|")
    g = defaultdict(dict)
    for r in rows:
        if r["_scene"] == "s2" and "sep" in r:
            g[(r["mode"], r["far"], r["near"], r["orient"], r["sep"])][r["dist"]] = r["leak_pct"]
    for key in sorted(g):
        out.append("| %s | %s | %s | %s | %s | %s |" % (key + (" | ".join(g[key].get(d, "-") for d in
                                                                      ["100", "1000", "5000", "10000", "17000"]),)))
    # scene 3
    out.append("\n### Scene 3 runway decal 0.2 m over flat ground (err % of runway pixels)\n")
    out.append("| depth | far | near | view | zBias | 0.5 km | 2 km | 4 km |")
    out.append("|---|---|---|---|---|---|---|---|")
    g = defaultdict(dict)
    for r in rows:
        if r["_scene"] == "s3" and "zbias" in r:
            g[(r["mode"], r["far"], r["near"], r["view"], r["zbias"])][r["dist"]] = r["err_pct"]
    for key in sorted(g):
        out.append("| %s | %s | %s | %s | %s | %s |" % (key + (" | ".join(g[key].get(d, "-") for d in
                                                                      ["500", "2000", "4000"]),)))
    # scene 4
    out.append("\n### Scene 4a fog ramp (black billboards at view depth W; fog fraction expected vs measured)\n")
    out.append("| depth | preset | W | expected | measured |")
    out.append("|---|---|---|---|---|")
    for r in rows:
        if r["_scene"] == "s4" and r["_kind"] == "fogramp":
            out.append("| %s | %s | %s | %s | %s |" % (r["mode"], r["preset"], r["w"], r["expected"], r["measured"]))
    out.append("\n### Scene 4b sea vs sky panorama at the far edge\n")
    out.append("| depth | preset | alt | pitch | edge row | max row step | max fog err (W > 0.75 far) | sea edge rgb | beyond rgb |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        if r["_scene"] == "s4" and r["_kind"] == "seasky":
            out.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (r["mode"], r["preset"], r["alt"], r["pitch"],
                       r["edge_row"], r["max_row_step"], r["max_fog_err_near_far"], r["sea_edge_rgb"], r["beyond_rgb"]))
    # scene 5
    out.append("\n### Scene 5 UV magnitude (integer uv offset M vs M = 0)\n")
    out.append("| depth | offset | max uv | px diff > 8 | px diff > 24 | mean diff |")
    out.append("|---|---|---|---|---|---|")
    for r in rows:
        if r["_scene"] == "s5":
            out.append("| %s | %s | %s | %s | %s | %s |" % (r["mode"], r["offset"], r["max_uv"], r["diff_px_gt8"],
                                                          r["diff_px_gt24"], r["mean_maxch_diff"]))
    # scene 6
    out.append("\n### Scene 6 jet (A1 LOD2) pixels vs background (diff > 10 / > 40), fog 3.6-18 km, haze behind\n")
    out.append("| depth | aspect | 1 km | 2.5 km | 4 km | 6 km |")
    out.append("|---|---|---|---|---|---|")
    g = defaultdict(dict)
    for r in rows:
        if r["_scene"] == "s6" and r["_kind"] == "jet" and "aspect" in r:
            g[(r["mode"], r["aspect"])][r["dist"]] = "%s / %s" % (r["px_diff_gt10"], r["px_diff_gt40"])
    for key in sorted(g):
        out.append("| %s | %s | %s |" % (key + (" | ".join(g[key].get(d, "-") for d in ["1000", "2500", "4000", "6000"]),)))
    # scene 7
    out.append("\n### Scene 7 blend check\n")
    out.append("| depth | case | expected | measured | ok |")
    out.append("|---|---|---|---|---|")
    for r in rows:
        if r["_scene"] == "s7" and r["_kind"] == "blendcheck":
            out.append("| %s | %s | %s | %s | %s |" % (r["mode"], r["case"], r["expected"], r["measured"], r["ok"]))
    out.append("\n### Scene 7 HUD line core colour (2 px green / yellow) per background\n")
    out.append("| blend | band | green rgb | green chroma | yellow rgb | luma contrast (green) |")
    out.append("|---|---|---|---|---|---|")
    g = {}
    for r in rows:
        if r["_scene"] == "s7" and r["_kind"] == "line" and r["mode"] == "w":
            g.setdefault((r["blend"], r["band"]), {})[r["line"]] = r
    for (bl, band), d in g.items():
        gr, ye = d.get("g2px"), d.get("y2px")
        out.append("| %s | %s | %s | %s | %s | %s |" % (bl, band, gr["rgb"], gr["chroma"], ye["rgb"], gr["luma_contrast"]))
    # scene 8
    out.append("\n### Scene 8 costs\n")
    out.append("| depth | case | frame ms (GPU incl.) | cpu ms |")
    out.append("|---|---|---|---|")
    for r in rows:
        if r["_scene"] == "s8" and r["_kind"] == "sync":
            out.append("| %s | %s | %s | %s |" % (r["mode"], r["case"], r["frame_ms"], r["cpu_part_ms"]))
        if r["_scene"] == "s8" and r["_kind"] == "bench":
            out.append("| %s | %s | (cpu-side) %s | %s |" % (r["mode"], r["case"], r["mean_ms"], r["cpu_part_ms"]))
    print("\n".join(out))


if __name__ == "__main__":
    main()
