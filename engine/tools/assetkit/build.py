"""Generator registry + runner (python3 tools/build_assets.py is a thin wrapper around main()).

Discovery (stable order): tools/art/gen_*.py, tools/sfx/gen_*.py, engine/tools/assetkit/samples/gen_*.py
(the kit's own sample; it writes into samples/out/, never into data/), then every tracks/<id>/track.txt via
tools/trackc/trackc.py (skipped with a note while trackc doesn't exist or lacks build_track()).

Generator protocol (module level):
    ASSETS = ["meshes/x.msh", "tex/x.bmp", {"file": "meshes/v10gt.msh", "class": "car"}, ...]
             # every output, relative to the data root; dict form declares the validator budget class
    ORDER = 50                    # optional; lower runs first (then by name)
    def build(out_dir, kit): ...  # write every ASSETS file under out_dir (the data root of this run)
trackc protocol: trackc.build_track(track_txt_path, out_dir, kit) -> list of written paths relative to
out_dir (e.g. ["tracks/testloop.trk", "tracks/testloop_preview.png", "tracks/testloop_report.txt"]).

Every run builds each generator into a fresh temp dir, validates the outputs, then either copies changed
files into data/ (normal) or compares md5 against data/ (--check, nothing is written).

Imports (1.2): only the selected generators are imported (--only), and an import-time error in one gen_*.py
(e.g. another task's half-saved file) fails only that generator ("FAILED (import error)"); the rest still run.
"""

import hashlib
import importlib.util
import os
import shutil
import sys
import tempfile
import time
import traceback

if __name__ == "__main__" and not __package__:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.path = [p for p in sys.path if os.path.abspath(p or ".") != os.path.dirname(os.path.abspath(__file__))]
    import assetkit.build as _b
    sys.exit(_b.main(sys.argv[1:]))

from . import KIT_DIR, project_root
from . import export, image, mesh, palette, preview, rng, validate


class Kit(object):
    """Handed to build(out_dir, kit). Attributes: name, root (project), out_dir (write here), data_dir (the
    real data root: read other generators' outputs through find()), preview_dir, previews (False in --check:
    skip expensive previews), check, tex_dirs ([out_dir/tex, data_dir/tex]) and the modules rng, palette,
    mesh, image, export, validate, preview."""

    rng_module = rng

    def __init__(self, name, root, out_dir, data_dir, preview_dir, check=False, previews=True, verbose=False):
        self.name = name
        self.root = root
        self.out_dir = out_dir
        self.data_dir = data_dir
        self.preview_dir = preview_dir
        self.check = check
        self.previews = previews
        self.verbose = verbose
        self.tex_dirs = [os.path.join(out_dir, "tex"), os.path.join(data_dir, "tex")]
        self.palette = palette
        self.mesh = mesh
        self.image = image
        self.export = export
        self.validate = validate
        self.preview = preview
        self.notes = []

    def rng(self, seed, *names):
        """assetkit.rng.Rng(seed, *names)."""
        return rng.Rng(seed, *names)

    def out(self, rel):
        """Absolute output path for a data-relative path; creates the folder."""
        p = os.path.join(self.out_dir, rel)
        d = os.path.dirname(p)
        if not os.path.isdir(d):
            os.makedirs(d)
        return p

    def find(self, rel):
        """Existing file: this run's output first, then data/. None if missing."""
        for base in (self.out_dir, self.data_dir):
            p = os.path.join(base, rel)
            if os.path.exists(p):
                return p
        return None

    def preview_path(self, name):
        if not os.path.isdir(self.preview_dir):
            os.makedirs(self.preview_dir)
        return os.path.join(self.preview_dir, name)

    def save_msh(self, rel, builder):
        """MeshBuilder -> out_dir/rel (.msh). Returns the MshFile."""
        return builder.save(self.out(rel))

    def save_bmp(self, rel, img, **kw):
        """image.save_bmp into out_dir/rel. Returns written paths."""
        return image.save_bmp(self.out(rel), img, **kw)

    def save_bmp8(self, rel, grey, **kw):
        """image.save_bmp8 (8-bit grey BMP, e.g. a hand-made <name>_a.bmp) into out_dir/rel. Returns the path."""
        return image.save_bmp8(self.out(rel), grey, **kw)

    def save_text(self, rel, text, encoding="ascii", crlf=False):
        """Text file into out_dir/rel, written as bytes (no newline translation: the bytes are exactly
        text.encode(encoding)). crlf=True turns every lone \\n into \\r\\n (files read by Win98 tools).
        Returns the path."""
        if crlf:
            text = text.replace("\r\n", "\n").replace("\n", "\r\n")
        p = self.out(rel)
        with open(p, "wb") as f:
            f.write(text.encode(encoding))
        return p

    def save_preview(self, name, img):
        """PNG into the preview dir (skipped when kit.previews is False). Returns the path or None."""
        if not self.previews:
            return None
        return preview.save(img, self.preview_path(name))

    def log(self, msg):
        self.notes.append(msg)
        print("  [%s] %s" % (self.name, msg))

    def validate_files(self, rels, classes=None):
        """Validate data-relative outputs of this run -> [validate.Problem]."""
        return validate.check_paths([os.path.join(self.out_dir, r) for r in rels], classes,
                                    [os.path.join(self.data_dir, "tex")])


class Gen(object):
    """One discovered generator. module is None until load(); load_error holds the traceback text when the
    import failed (that generator then fails on its own; the others still run)."""

    def __init__(self, name, kind, path, data_root, preview_dir, track_src=None):
        self.name = name
        self.kind = kind
        self.path = path
        self.data_root = data_root
        self.preview_dir = preview_dir
        self.track_src = track_src
        self.module = None
        self.load_error = None
        self.order = 50

    def load(self):
        """Import the generator module (once). Returns True on success; on any import-time error (syntax
        error, missing name, half-saved file) stores the traceback in load_error and returns False."""
        if self.kind == "track" or self.module is not None:
            return True
        if self.load_error is not None:
            return False
        modname = "assetkit_gen_" + self.kind + "_" + self.name
        try:
            self.module = _load_module(self.path, modname)
            self.order = getattr(self.module, "ORDER", 50)
        except (Exception, SystemExit):
            sys.modules.pop(modname, None)
            self.module = None
            self.load_error = traceback.format_exc()
        return self.load_error is None


def _load_module(path, modname):
    kit_parent = os.path.dirname(KIT_DIR)
    if kit_parent not in sys.path:
        sys.path.insert(0, kit_parent)
    spec = importlib.util.spec_from_file_location(modname, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[modname] = mod
    spec.loader.exec_module(mod)
    return mod


_KINDS = {"art": 0, "sfx": 1, "sample": 2, "track": 3}


def discover(root, only=None, load=True):
    """Generators in run order (art, sfx, sample sorted by (ORDER, name); then tracks).
    only = list of "name[,name]" strings (see select()): only those generators are imported. load=False
    imports nothing (order = file name order). A generator whose import fails is still returned, with
    g.load_error set; run_gen() reports it as that generator's failure."""
    gens = select(_find(root), only)
    if load:
        for g in gens:
            g.load()
    gens.sort(key=lambda g: (_KINDS[g.kind], g.order, g.name))
    return gens


def _find(root):
    """Every generator file and track, unimported, in discovery order."""
    data = os.path.join(root, "data")
    gens = []
    for sub, kind in (("tools/art", "art"), ("tools/sfx", "sfx")):
        d = os.path.join(root, sub)
        if os.path.isdir(d):
            for f in sorted(os.listdir(d)):
                if f.startswith("gen_") and f.endswith(".py"):
                    gens.append(Gen(f[4:-3], kind, os.path.join(d, f), data,
                                    os.path.join(root, "tools", "art", "preview")))
    sd = os.path.join(KIT_DIR, "samples")
    for f in sorted(os.listdir(sd)) if os.path.isdir(sd) else []:
        if f.startswith("gen_") and f.endswith(".py"):
            out = os.path.join(sd, "out")
            gens.append(Gen(f[4:-3], "sample", os.path.join(sd, f), out, os.path.join(out, "preview")))
    td = os.path.join(root, "tracks")
    if os.path.isdir(td):
        for t in sorted(os.listdir(td)):
            src = os.path.join(td, t, "track.txt")
            if os.path.exists(src):
                gens.append(Gen(t, "track", os.path.join(root, "tools", "trackc", "trackc.py"), data,
                                os.path.join(data, "tracks"), src))
    return gens


def _md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def _declared(mod):
    files = []
    classes = {}
    for a in getattr(mod, "ASSETS", []):
        if isinstance(a, dict):
            files.append(a["file"])
            if a.get("class"):
                classes[a["file"]] = a["class"]
        else:
            files.append(a)
    return files, classes


def _walk(base, skip_dir=None):
    out = []
    for r, dirs, files in os.walk(base):
        dirs.sort()
        if skip_dir and os.path.abspath(r).startswith(os.path.abspath(skip_dir)):
            continue
        for f in sorted(files):
            out.append(os.path.relpath(os.path.join(r, f), base).replace(os.sep, "/"))
    return out


def _mesh_tris(path):
    try:
        return export.read_msh(path).tri_counts()[0]
    except (ValueError, IndexError):
        return 0


class Result(object):
    def __init__(self, gen):
        self.gen = gen
        self.files = []
        self.tris = 0
        self.kb = 0.0
        self.errors = 0
        self.warnings = 0
        self.status = ""
        self.ok = True
        self.secs = 0.0
        self.skipped = False


def run_gen(gen, root, check=False, previews=True, verbose=False, keep_temp=False, trackc=None):
    res = Result(gen)
    t0 = time.time()
    tmp = tempfile.mkdtemp(prefix="assetkit_%s_" % gen.name)
    prev_dir = os.path.join(tmp, "_preview") if check else gen.preview_dir
    kit = Kit(gen.name, root, tmp, gen.data_root, prev_dir, check, previews and not check, verbose)
    try:
        if gen.kind != "track" and not gen.load():
            res.ok = False
            res.errors += 1
            res.status = "FAILED (import error)"
            print("  [%s] import of %s failed:" % (gen.name, os.path.relpath(gen.path, root)))
            sys.stdout.write("".join("  [%s] %s\n" % (gen.name, l) for l in gen.load_error.rstrip().split("\n")))
            return res
        if gen.kind == "track" and isinstance(trackc, ImportFailure):
            res.ok = False
            res.errors += 1
            res.status = "FAILED (trackc import error)"
            sys.stdout.write("".join("  [%s] %s\n" % (gen.name, l) for l in trackc.error.rstrip().split("\n")))
            return res
        if gen.kind == "track":
            if trackc is None:
                res.skipped = True
                res.status = "skipped: trackc not present yet"
                return res
            if not hasattr(trackc, "build_track"):
                res.skipped = True
                res.status = "skipped: trackc.build_track() missing"
                return res
            written = trackc.build_track(gen.track_src, tmp, kit) or []
            declared, classes = [w.replace(os.sep, "/") for w in written], {}
        else:
            declared, classes = _declared(gen.module)
            gen.module.build(tmp, kit)
        written = _walk(tmp, os.path.join(tmp, "_preview"))
        missing = [d for d in declared if d not in written]
        extra = [w for w in written if w not in declared]
        msgs = []
        for d in missing:
            msgs.append("declared but not written: %s" % d)
            res.errors += 1
        for w in extra:
            msgs.append("written but not in ASSETS (warning): %s" % w)
            res.warnings += 1
        res.files = written
        for w in written:
            p = os.path.join(tmp, w)
            res.kb += os.path.getsize(p) / 1024.0
            if w.endswith(".msh"):
                res.tris += _mesh_tris(p)
        probs = validate.check_paths([os.path.join(tmp, w) for w in written], classes,
                                     [os.path.join(gen.data_root, "tex")])
        for p in probs:
            rel = os.path.relpath(p.path, tmp)
            if p.level == "error":
                res.errors += 1
                msgs.append("%s: ERROR [%s] %s" % (rel, p.code, p.msg))
            else:
                res.warnings += 1
                if verbose:
                    msgs.append("%s: warn [%s] %s" % (rel, p.code, p.msg))
        if check:
            same = diff = new = 0
            for w in written:
                dst = os.path.join(gen.data_root, w)
                if not os.path.exists(dst):
                    new += 1
                    msgs.append("not in data: %s" % w)
                elif _md5(dst) == _md5(os.path.join(tmp, w)):
                    same += 1
                else:
                    diff += 1
                    msgs.append("md5 differs: %s" % w)
            res.status = "identical" if diff == 0 and new == 0 else "DIFF %d, missing %d" % (diff, new)
            if diff or new:
                res.ok = False
        else:
            changed = 0
            for w in written:
                src = os.path.join(tmp, w)
                dst = os.path.join(gen.data_root, w)
                if not os.path.exists(dst) or _md5(dst) != _md5(src):
                    d = os.path.dirname(dst)
                    if not os.path.isdir(d):
                        os.makedirs(d)
                    shutil.copyfile(src, dst)
                    changed += 1
            res.status = "%d written, %d unchanged" % (changed, len(written) - changed)
        if res.errors:
            res.ok = False
        for m in msgs:
            print("  [%s] %s" % (gen.name, m))
    except Exception:
        res.ok = False
        res.errors += 1
        res.status = "FAILED (exception)"
        traceback.print_exc()
    finally:
        res.secs = time.time() - t0
        if keep_temp:
            print("  [%s] temp output kept in %s" % (gen.name, tmp))
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return res


class ImportFailure(object):
    """Stands in for a module whose import raised (error = traceback text)."""

    def __init__(self, error):
        self.error = error


def select(gens, only):
    """Generators named in only (list of "a,b" strings: generator names, track ids, kinds art sfx sample, or
    tracks). Unknown names raise SystemExit. Doesn't import anything."""
    if not only:
        return gens
    want = set()
    for o in only:
        for x in o.split(","):
            if x.strip():
                want.add(x.strip())
    out = [g for g in gens if g.name in want or g.kind in want or (g.kind == "track" and "tracks" in want)]
    unknown = want - set(g.name for g in gens) - set(g.kind for g in gens) - set(["tracks"])
    if unknown:
        raise SystemExit("build: unknown generator(s): %s (known: %s)" % (", ".join(sorted(unknown)),
                                                                      ", ".join(g.name for g in gens)))
    return out


def main(argv=None, root=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    root = root or project_root()
    only = []
    check = False
    previews = True
    verbose = False
    keep = False
    list_only = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--only":
            only.append(argv[i + 1])
            i += 1
        elif a.startswith("--only="):
            only.append(a[7:])
        elif a == "--check":
            check = True
        elif a == "--no-previews":
            previews = False
        elif a in ("-v", "--verbose"):
            verbose = True
        elif a == "--keep-temp":
            keep = True
        elif a == "--list":
            list_only = True
        elif a in ("-h", "--help"):
            print("usage: build_assets.py [--only NAME[,NAME]] [--check] [--no-previews] [--list] [-v] "
                  "[--keep-temp]\nNAME = generator name (gen_<name>.py), track id, or a kind: art sfx sample tracks")
            return 0
        else:
            print("build: unknown argument %r" % a)
            return 2
        i += 1
    gens = discover(root, only)             # imports only the selected generators
    if list_only:
        for g in gens:
            print("%-14s %-6s %s%s" % (g.name, g.kind, os.path.relpath(g.track_src or g.path, root),
                                       "  (IMPORT ERROR)" if g.load_error else ""))
        return 0
    trackc = None
    tc_path = os.path.join(root, "tools", "trackc", "trackc.py")
    if any(g.kind == "track" for g in gens) and os.path.exists(tc_path):
        tc_dir = os.path.dirname(tc_path)
        if tc_dir not in sys.path:
            sys.path.insert(0, tc_dir)
        try:
            trackc = _load_module(tc_path, "trackc")
        except (Exception, SystemExit):
            sys.modules.pop("trackc", None)
            trackc = ImportFailure(traceback.format_exc())
    print("build: %d generator(s)%s, root %s" % (len(gens), " (check)" if check else "", root))
    results = []
    for g in gens:
        print("build: %s (%s)" % (g.name, g.kind))
        results.append(run_gen(g, root, check, previews, verbose, keep, trackc))
    full = not only and not check
    final = []
    if full:
        final = validate.errors(validate.check_paths([os.path.join(root, "data")]))
        for p in final:
            print("  [data] %s: ERROR [%s] %s" % (os.path.relpath(p.path, root), p.code, p.msg))
    print("")
    print("%-14s %-6s %5s %7s %9s %-10s %s" % ("asset", "kind", "files", "tris", "KB", "budget", "result"))
    bad = 0
    for r in results:
        if r.skipped:
            print("%-14s %-6s %5s %7s %9s %-10s %s" % (r.gen.name, r.gen.kind, "-", "-", "-", "-", r.status))
            continue
        budget = "ok" if r.errors == 0 else "%d err" % r.errors
        print("%-14s %-6s %5d %7d %9.1f %-10s %s (%.1fs)" % (r.gen.name, r.gen.kind, len(r.files), r.tris, r.kb,
                                                           budget, r.status, r.secs))
        if not r.ok:
            bad += 1
    if full:
        print("data/ validation: %d errors" % len(final))
    ok = bad == 0 and not final
    print("build: %s" % ("OK" if ok else "FAILED (%d generator(s))" % (bad + (1 if final else 0))))
    return 0 if ok else 1


def main_for(gen_file, argv=None):
    """Run one generator file as `python3 tools/art/gen_x.py [--check] [--no-previews]`."""
    name = os.path.basename(gen_file)[4:-3]
    args = list(sys.argv[1:] if argv is None else argv)
    return main(["--only", name] + args)


if __name__ == "__main__":
    sys.exit(main())
