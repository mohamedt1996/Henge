#!/usr/bin/env python3 -I
"""What a Mac app imports that iOS does not export (Mac-side analysis for the
translation layer). For each system library the app's code links: how many
symbols it takes from it, and which of them the iOS SDK's stubs (.tbd) lack.
Libraries inside the app are followed too. Private frameworks are not in the
SDK, so they are reported as unknown rather than missing.

usage: tl_ios_gap.py [--list N] [--deny FILE] APP_OR_BINARY...
--deny FILE writes what iOS lacks, one "INSTALL_NAME<tab>SYMBOL" per line ("*" for a whole library), for
tlrun -deny: a Mac run that binds none of it behaves, as far as libraries go, as the app would on iOS."""
import os, re, subprocess, sys

SDK = subprocess.run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
_tbd_cache = {}

def ios_tbd_path(install_name):
    if install_name.startswith("/System/iOSSupport/"):  # Mac Catalyst's copies of iOS frameworks
        install_name = install_name[len("/System/iOSSupport"):]
    m = re.match(r"^/System/Library/(Private)?Frameworks/([^/]+)\.framework/(?:Versions/[^/]+/)?([^/]+)$", install_name)
    if m:
        p = f"{SDK}/System/Library/{m.group(1) or ''}Frameworks/{m.group(2)}.framework/{m.group(3)}.tbd"
        return p if os.path.exists(p) else None
    if install_name.startswith("/usr/lib/"):
        p = SDK + os.path.splitext(install_name)[0] + ".tbd"
        return p if os.path.exists(p) else None
    return None

def tbd_exports(path, seen=None):
    """Every symbol a .tbd exports for iOS, re-exported libraries included."""
    if path in _tbd_cache: return _tbd_cache[path]
    seen = seen or set()
    if path in seen: return set()
    seen.add(path)
    text = open(path).read()
    out = set()
    # tbd v4: blocks of "- targets: [...]" followed by keys with [ ... ] lists.
    for block in re.split(r"\n  - targets:", text):
        if "ios" not in block.split("]", 1)[0] and not block.startswith("--- "): continue
        for key, body in re.findall(r"(symbols|weak-symbols|thread-local-symbols|objc-classes|objc-eh-types|objc-ivars|libraries):\s*\[(.*?)\]", block, re.S):
            items = [x.strip().strip("'\"") for x in body.split(",") if x.strip()]
            for it in items:
                if key in ("symbols", "weak-symbols", "thread-local-symbols"): out.add(it)
                elif key == "objc-classes": out.update({f"_OBJC_CLASS_$_{it}", f"_OBJC_METACLASS_$_{it}"})
                elif key == "objc-eh-types": out.add(f"_OBJC_EHTYPE_$_{it}")
                elif key == "objc-ivars": out.add(f"_OBJC_IVAR_$_{it}")
                elif key == "libraries":
                    sub = ios_tbd_path(it)
                    if sub: out |= tbd_exports(sub, seen)
    _tbd_cache[path] = out
    return out

# Where iOS keeps what macOS exports from an umbrella or a Mac-only library (ApplicationServices re-exports
# CoreGraphics and CoreText; AudioUnit's calls live in AudioToolbox on iOS): Mac install names, tried in order.
ALTERNATES = ["/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
              "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText",
              "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO",
              "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
              "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation",
              "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox",
              "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices",
              "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate",
              "/usr/lib/libSystem.B.dylib"]

def alternate_for(sym, not_lib):
    for alt in ALTERNATES:
        if alt == not_lib: continue
        tbd = ios_tbd_path(alt)
        if tbd and sym in tbd_exports(tbd): return alt
    return None

def dyld_info(args):
    return subprocess.run(["xcrun", "dyld_info"] + args, capture_output=True, text=True).stdout

def arch_of(binary):
    archs = subprocess.run(["lipo", "-archs", binary], capture_output=True, text=True).stdout.split()
    return "arm64" if "arm64" in archs else "arm64e" if "arm64e" in archs else (archs[0] if archs else None)

def leaf(install_name):
    base = install_name.rsplit("/", 1)[-1]
    base = re.sub(r"\.dylib$", "", base)
    base = re.sub(r"(\.[0-9]+)+$", "", base)
    return re.sub(r"\.[A-C]$", "", base)

def analyze(binary, app_dir, report, done):
    if binary in done: return
    done.add(binary)
    arch = arch_of(binary)
    if not arch or arch not in ("arm64", "arm64e"):
        report.setdefault("(no arm64 code: " + os.path.basename(binary) + ")", [])
        return
    deps = [l.split()[-1] for l in dyld_info(["-arch", arch, "-dependents", binary]).splitlines()[2:] if l.strip()]
    by_leaf = {}
    for d in deps: by_leaf.setdefault(leaf(d), d)
    for line in dyld_info(["-arch", arch, "-imports", binary]).splitlines():
        # With or without the fixup address in front (dyld_info prints both, depending on the binary's format).
        m = re.match(r"\s+(?:0x[0-9A-Fa-f]+\s+)?(\S+)\s+(?:\[weak-import\]\s+)?\(from (.+)\)", line)
        if not m: continue
        sym, lib = re.sub(r"\+0x[0-9A-Fa-f]+$", "", m.group(1)), m.group(2)
        name = by_leaf.get(lib, lib)
        report.setdefault(name, []).append((sym, "[weak-import]" in line))
    # Libraries inside the app are guest code too: follow their imports.
    for d in deps:
        if d.startswith("@") and app_dir:
            for cand in (d.replace("@rpath/", "Contents/Frameworks/").replace("@executable_path/../", "Contents/").replace("@loader_path/../", "Contents/"),):
                p = os.path.join(app_dir, cand)
                if os.path.isfile(p): analyze(p, app_dir, report, done)

def main():
    args = sys.argv[1:]
    nlist = 12
    deny = None
    while args[:1] in (["--list"], ["--deny"]):
        if args[0] == "--list": nlist = int(args[1])
        else: deny = open(args[1], "w")
        args = args[2:]
    for target in args:
        app_dir = target if target.endswith(".app") else None
        binary = target
        if app_dir:
            import plistlib
            info = plistlib.load(open(os.path.join(app_dir, "Contents/Info.plist"), "rb"))
            binary = os.path.join(app_dir, "Contents/MacOS", info["CFBundleExecutable"])
        report = {}
        analyze(binary, app_dir, report, set())
        total = sum(len(v) for v in report.values())
        print(f"== {target} ({arch_of(binary)}): {total} imports from {len(report)} libraries")
        missing_total = unknown_total = 0
        rows = []
        for lib, syms in sorted(report.items(), key=lambda kv: -len(kv[1])):
            if lib.startswith("@") or lib.startswith("<"): continue
            tbd = ios_tbd_path(lib)
            if not tbd:
                if deny:
                    deny.write(f"{lib}\t*\n")
                    for sym, weak in sorted(set(syms)):
                        alt = alternate_for(sym, lib)
                        if alt: deny.write(f"{lib}\t{sym}\t={alt}\n")
                private = "/PrivateFrameworks/" in lib
                unknown_total += len(syms)
                rows.append((lib, len(syms), None, "private on macOS: unknown on iOS" if private else "NOT ON iOS", []))
                continue
            exports = tbd_exports(tbd)
            miss = sorted({s for s, weak in syms if s not in exports and not s.startswith("$ld$")})
            if deny:
                for sym in miss:
                    alt = alternate_for(sym, lib)
                    deny.write(f"{lib}\t{sym}" + (f"\t={alt}" if alt else "") + "\n")
            missing_total += len(miss)
            rows.append((lib, len(syms), len(miss), "", miss))
        for lib, n, m, note, miss in rows:
            short = lib.replace("/System/iOSSupport/", "(Catalyst) /").replace("/System/Library/Frameworks/", "").replace("/System/Library/PrivateFrameworks/", "(private) ")
            if m is None: print(f"  {short:70} {n:5} imports  {note}")
            else: print(f"  {short:70} {n:5} imports  {m:5} missing on iOS")
            classes = [s.replace("_OBJC_CLASS_$_", "") for s in miss if s.startswith("_OBJC_CLASS_$_")]
            if classes: print(f"      missing classes: {', '.join(classes[:nlist])}{' ...' if len(classes) > nlist else ''}")
            other = [s for s in miss if not s.startswith("_OBJC_")]
            if other: print(f"      missing symbols: {', '.join(other[:nlist])}{' ...' if len(other) > nlist else ''}")
        print(f"  => {missing_total} imports missing from iOS's public libraries, {unknown_total} from libraries iOS does not have publicly")

main()
