#!/usr/bin/env python3
"""Build the ux0:data/zombiewood/ folder for the Zombiewood Vita loader from YOUR OWN game files.

You need, from your own copy of Zombiewood 1.5.3 (version code 15325) for Android:
  - the APK  (com.gameloft.android.ANMP.GloftZOHM ... .apk)
  - both OBB files, from Android/obb/com.gameloft.android.ANMP.GloftZOHM/ on a device where the
    game is installed:
      main.1501.com.gameloft.android.ANMP.GloftZOHM.obb
      patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb

Usage:
  python3 prepare_data.py --apk <file.apk> --obb-dir <folder with both .obb files> [--out <folder>]

The output folder (default: ./zombiewood) is what you copy to ux0:data/zombiewood/ on the Vita.
Only Python 3's standard library is used. Nothing is downloaded and nothing leaves your computer.
"""
import argparse
import hashlib
import io
import os
import shutil
import sys
import zipfile
import zlib

PKG = "com.gameloft.android.ANMP.GloftZOHM"
MAIN_OBB = "main.1501." + PKG + ".obb"
PATCH_OBB = "patch.15325." + PKG + ".obb"

# The loader patches fixed code offsets of this exact build and checks these values at start.
SO_FILES = {
    "libZombieWood.so": (13500416, 0xA83B9A52),
    "libgenerator.so": (447860, 0x337CC20D),
}
OBB_SIZES = {MAIN_OBB: 289401986, PATCH_OBB: 1479926}
MANIFEST = "zw_data.txt"
TESTED_APK_SHA1 ="14fa6e1e7840465c4070f81e75d9abade0022f92"
# These files from effects.bar (a zip inside the OBB) also go next to it. Exactly this set is what
# the port was tested with; other loose shaders could change which ones the game picks.
UNPACK_BARS = ("effects.bar",)
BAR_FILES = {
    "Bloom.bdae", "BloomFS.glsl", "BloomVS.glsl", "DefaultEffects.bdae", "ShadowMapDepth.bdae",
    "ShadowMapNoDepth.bdae", "ShadowProjection.bdae", "ShadowVolume.bdae", "Simple.bdae",
    "SimpleFS.glsl", "SimpleFS_low.glsl", "SimpleVS.glsl", "TerrainEffects.bdae",
    "UnlitCubeMapShadow.bdae", "UnlitCubeMapShadowFP.glsl", "UnlitCubeMapShadowMapFP.glsl",
    "UnlitCubeMapShadowMapVP.glsl", "UnlitCubeMapShadowVP.glsl", "game_effects.bdae",
    "gameswf_effects.bdae",
}


def fail(msg):
    print("ERROR: " + msg)
    sys.exit(1)


def crc32_of(path):
    crc = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def unpack_flat(zip_path, out, overwrite, only=None):
    """Extract the files of a zip (or only those named in `only`) into out/ by base name."""
    n = 0
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            name = os.path.basename(info.filename)
            if info.is_dir() or not name or name.startswith("."):
                continue
            if only is not None and name not in only:
                continue
            dst = os.path.join(out, name)
            if os.path.exists(dst) and not overwrite:
                continue
            with z.open(info) as src, open(dst, "wb") as f:
                shutil.copyfileobj(src, f, 1 << 20)
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description="Prepare ux0:data/zombiewood/ from your own Zombiewood 1.5.3 files.")
    ap.add_argument("--apk", required=True, help="your Zombiewood 1.5.3 APK")
    ap.add_argument("--obb-dir", required=True, help="folder containing both OBB files")
    ap.add_argument("--out", default="zombiewood", help="output folder (default: ./zombiewood)")
    a = ap.parse_args()

    if not os.path.isfile(a.apk):
        fail("APK not found: " + a.apk)
    obbs = {n: os.path.join(a.obb_dir, n) for n in OBB_SIZES}
    for n, p in obbs.items():
        if not os.path.isfile(p):
            fail("missing %s in %s (copy it from Android/obb/%s/ on your device)" % (n, a.obb_dir, PKG))
        if os.path.getsize(p) != OBB_SIZES[n]:
            fail("%s has the wrong size (%d bytes, expected %d): not version 1.5.3" % (n, os.path.getsize(p), OBB_SIZES[n]))
    os.makedirs(a.out, exist_ok=True)

    print("1/4 Checking the APK ...")
    if sha1_of(a.apk) != TESTED_APK_SHA1:
        print("    note: this APK differs from the tested one; continuing, the libraries are checked next.")

    print("2/4 Extracting the game libraries from the APK ...")
    if not zipfile.is_zipfile(a.apk):
        fail(a.apk + " is not a valid APK (the file is damaged or not an APK)")
    with zipfile.ZipFile(a.apk) as z:
        # a plain APK has lib/armeabi-v7a/ itself; an XAPK / split-APK bundle has it in an inner .apk
        inner = [i for i in z.infolist() if i.filename.lower().endswith(".apk")]
        for name, (size, crc) in SO_FILES.items():
            data = None
            try:
                data = z.read("lib/armeabi-v7a/" + name)
            except KeyError:
                for i in inner:
                    with zipfile.ZipFile(io.BytesIO(z.read(i))) as zi:
                        try:
                            data = zi.read("lib/armeabi-v7a/" + name)
                            break
                        except KeyError:
                            pass
            if data is None:
                fail(name + " is not in this APK (is it the ARMv7 build of Zombiewood 1.5.3?)")
            if len(data) != size or (zlib.crc32(data) & 0xFFFFFFFF) != crc:
                fail(name + " does not match version 1.5.3 (15325); this loader only supports that build")
            with open(os.path.join(a.out, name), "wb") as f:
                f.write(data)

    print("3/4 Copying and unpacking the OBB files (about 300 MB, takes a minute) ...")
    for n, p in obbs.items():
        dst = os.path.join(a.out, n)
        if not (os.path.isfile(dst) and os.path.getsize(dst) == OBB_SIZES[n]):
            shutil.copyfile(p, dst)
    n_main = unpack_flat(obbs[MAIN_OBB], a.out, overwrite=True)
    n_patch = unpack_flat(obbs[PATCH_OBB], a.out, overwrite=True)   # the patch replaces main's versions

    print("4/4 Unpacking shader archives ...")
    # Taken from the MAIN OBB's effects.bar (the version the port was tested with), not the
    # patch's newer one that now sits in the output folder.
    n_bar = 0
    with zipfile.ZipFile(obbs[MAIN_OBB]) as z:
        for bar in UNPACK_BARS:
            entry = next((i for i in z.infolist() if os.path.basename(i.filename) == bar), None)
            if entry is None:
                fail(bar + " is missing from the main OBB")
            tmp = os.path.join(a.out, bar + ".main.tmp")
            with z.open(entry) as src, open(tmp, "wb") as f:
                shutil.copyfileobj(src, f, 1 << 20)
            try:
                n_bar += unpack_flat(tmp, a.out, overwrite=True, only=BAR_FILES)
            finally:
                os.remove(tmp)
    if n_bar != len(BAR_FILES):
        fail("expected %d shader files, found %d" % (len(BAR_FILES), n_bar))

    # Written last: the loader checks every listed file (name and size) at start, so a copy to the
    # Vita that stopped partway shows an error screen instead of crashing in the game.
    names = sorted(e.name for e in os.scandir(a.out) if e.is_file() and e.name != MANIFEST)
    with open(os.path.join(a.out, MANIFEST), "w", newline="\n") as f:
        f.write("ZWDATA 1\n")
        for n in names:
            f.write("%d %s\n" % (os.path.getsize(os.path.join(a.out, n)), n))
        f.write("END %d\n" % len(names))

    total = sum(1 for _ in os.scandir(a.out))
    mb = sum(e.stat().st_size for e in os.scandir(a.out) if e.is_file()) / (1 << 20)
    print("\nDone: %d files, %.0f MB in %s" % (total, mb, os.path.abspath(a.out)))
    print("      (%d from the main OBB, %d from the patch, %d shader files)" % (n_main, n_patch, n_bar))
    print("Copy the CONTENTS of that folder to ux0:data/zombiewood/ on your Vita.")


if __name__ == "__main__":
    try:
        main()
    except (zipfile.BadZipFile, zlib.error, EOFError) as e:
        # zipfile verifies every entry's CRC while unpacking: a damaged OBB of the right size ends here
        fail("a game file is damaged (%s). Copy the APK and both OBB files again from your device." % e)
    except OSError as e:
        fail("could not read or write a file: %s (is there enough free disk space?)" % e)
