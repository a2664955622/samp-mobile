#!/usr/bin/env python3
"""
Build install-and-play bundled APKs with the game data embedded inside the APK.

Two editions (matching the launcher's own Lite/Full verification profiles):

  lite  — 精简版: no .osw audio streams (~1.04 GiB unpacked), built from
          build/gamedata/gamedata-lite.zip.
  full  — 完整版: includes full GTA:SA SFX/radio streams (~2.10 GiB unpacked),
          built from build/gamedata/gamedata-full.zip (--keep-audio).

What the script does per edition:

  1. Ensures the source data .zip exists (invoking tools/gamedata/build_gamedata.py
     when needed; inputs: dist/*.obb + PC GTA:SA with SA-MP 0.3.7).
  2. Recompresses the zip entries DEFLATE into a .gdz cache
     (build/gamedata/.cache/) — the default STORED zips compress to ~50%,
     which is worth minutes of one-time build CPU when the APK is meant to be
     sent over chat apps.
  3. Prepares an assets overlay (build/bundled-apk/overlay-<edition>/gamedata/)
     with bundle.gdz + bundle.json metadata.
  4. Builds release (or debug with --build-type debug) with
     -PsampBundleAssets=<overlay> signed by the throwaway test key
     dist/friends.jks (generated once, reused forever so later builds keep
     installing as updates over previous ones; debug builds use the same key
     for the same reason).
  5. Copies the result to build/apk/samp-mobile-<version>-<edition>-bundled.apk
     (…-bundled-debug.apk for debug) and verifies the bundle inside is present
     and STORED (not re-compressed).

The launcher side (already in the app): on first launch with missing game data,
MainActivity offers one-tap extraction of the embedded bundle, streaming it
straight out of APK assets — no network, no manual zip import.

Usage:
  python tools/packaging/build_bundled_apk.py                 # both editions
  python tools/packaging/build_bundled_apk.py --edition lite  # just one
  python tools/packaging/build_bundled_apk.py --rebuild       # regenerate data zips from scratch

NOTE: the output APKs contain proprietary Rockstar game data — do not publish
them (see NOTICE).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from pkgutil import (
    APK_OUT_DIR,
    EDITIONS,
    GDZ_CACHE_DIR,
    OVERLAY_ROOT,
    ensure_data_zip,
    ensure_keystore,
    fail,
    find_java_home,
    gradle_apk_path,
    hardlink_or_copy,
    log,
    read_app_version,
    run_gradle,
)

TAG = "bundled-apk"


# ---------------------------------------------------------------------- recompress


def recompress_to_gdz(edition: str, src_zip: Path, force: bool = False) -> tuple[Path, dict]:
    """Rewrite a game-data zip with DEFLATE entries into the cache, streaming entry
    by entry (never holding a whole .img in memory). Returns (gdz_path, stats)."""
    GDZ_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    out = GDZ_CACHE_DIR / f"gamedata-{edition}.gdz"
    meta_path = GDZ_CACHE_DIR / f"gamedata-{edition}.json"

    st = src_zip.stat()
    if not force and meta_path.is_file() and out.is_file():
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
            if meta.get("sourceSize") == st.st_size and meta.get("sourceMtime") == st.st_mtime:
                log(f"{edition}: compressed cache is fresh -> {out}", TAG)
                return out, meta
        except Exception:
            pass

    tmp_out = out.with_suffix(".gdz.tmp")
    log(f"{edition}: recompressing {src_zip.name} -> DEFLATE .gdz (one-time, a few minutes)…", TAG)
    t0 = time.time()

    with zipfile.ZipFile(src_zip, "r") as zin:
        infos = [i for i in zin.infolist() if not i.is_dir()]
        total_unc = sum(i.file_size for i in infos)
        done = 0
        last_report = 0.0
        with zipfile.ZipFile(tmp_out, "w", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=6, allowZip64=True) as zout:
            for idx, info in enumerate(infos, 1):
                zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                zi.compress_type = zipfile.ZIP_DEFLATED
                zi.external_attr = info.external_attr
                with zin.open(info, "r") as fin, zout.open(zi, "w") as fout:
                    while True:
                        chunk = fin.read(1024 * 1024)
                        if not chunk:
                            break
                        fout.write(chunk)
                        done += len(chunk)
                        now = time.time()
                        if now - last_report >= 5.0:
                            last_report = now
                            pct = done * 100.0 / max(1, total_unc)
                            log(f"{edition}:   deflate {pct:.0f}% ({done / 1048576:.0f}/{total_unc / 1048576:.0f} MB)", TAG)
    os.replace(tmp_out, out)

    stats = {
        "sourceZip": str(src_zip),
        "sourceSize": st.st_size,
        "sourceMtime": st.st_mtime,
        "entryCount": len(infos),
        "uncompressedBytes": total_unc,
        "zipBytes": out.stat().st_size,
    }
    meta_path.write_text(json.dumps(stats, ensure_ascii=False, indent=2), encoding="utf-8")
    ratio = stats["zipBytes"] / max(1, total_unc)
    log(
        f"{edition}: .gdz ready in {(time.time() - t0) / 60:.1f} min — "
        f"{stats['zipBytes'] / 1048576:.0f} MB (ratio {ratio:.2f}, {len(infos)} entries)",
        TAG,
    )
    return out, stats


# ---------------------------------------------------------------------- overlay


def prepare_overlay(edition: str, gdz: Path, stats: dict) -> Path:
    overlay = OVERLAY_ROOT / f"overlay-{edition}" / "gamedata"
    overlay.mkdir(parents=True, exist_ok=True)
    hardlink_or_copy(gdz, overlay / "bundle.gdz")
    meta = {
        "edition": edition,
        "label": EDITIONS[edition]["label"],
        "zipBytes": gdz.stat().st_size,
        "uncompressedBytes": stats.get("uncompressedBytes", -1),
        "entryCount": stats.get("entryCount", -1),
        "appVersion": read_app_version(),
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    (overlay / "bundle.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    return overlay.parent  # assets overlay root (contains gamedata/)


# ---------------------------------------------------------------------- verify


def verify_apk(apk: Path, edition: str) -> None:
    problems = []
    with zipfile.ZipFile(apk, "r") as z:
        names = set(z.namelist())
        bundle = None
        for i in z.infolist():
            if i.filename == "assets/gamedata/bundle.gdz":
                bundle = i
                break
        if bundle is None:
            problems.append("assets/gamedata/bundle.gdz missing")
        elif bundle.compress_type != zipfile.ZIP_STORED:
            problems.append(
                "bundle.gdz is DEFLATED inside the APK — aaptOptions noCompress \"gdz\" "
                "not effective; extraction would double-inflate on the phone"
            )
        if "assets/gamedata/bundle.json" not in names:
            problems.append("assets/gamedata/bundle.json missing")
        for abi in ("arm64-v8a", "armeabi-v7a"):
            if f"lib/{abi}/libSAMPMOBILE.so" not in names:
                problems.append(f"lib/{abi}/libSAMPMOBILE.so missing")
            if f"lib/{abi}/libGTASA.so" not in names:
                problems.append(f"lib/{abi}/libGTASA.so missing")
    if problems:
        for p in problems:
            print(f"[{TAG}]   VERIFY FAIL: {p}", file=sys.stderr)
        fail(f"{edition}: APK verification failed", TAG)
    log(f"{edition}: APK content verification passed (bundle STORED, dual-ABI libs present)", TAG)


# ---------------------------------------------------------------------- main


def main() -> None:
    ap = argparse.ArgumentParser(description="Build install-and-play bundled APKs with embedded game data.")
    ap.add_argument("--edition", choices=["lite", "full", "both"], default="both",
                    help="which edition(s) to build (default: both)")
    ap.add_argument("--pc-samp", default=None,
                    help="PC GTA:SA dir with SA-MP 0.3.7 (default: auto-detect common install paths)")
    ap.add_argument("--out", default=str(APK_OUT_DIR),
                    help=f"output directory for final APKs (default: {APK_OUT_DIR})")
    ap.add_argument("--rebuild", action="store_true",
                    help="regenerate the source data zips via build_gamedata.py even if they exist")
    ap.add_argument("--recompress", action="store_true",
                    help="force the DEFLATE recompression pass even if the .gdz cache looks fresh")
    ap.add_argument("--build-type", choices=["release", "debug"], default="release",
                    help="gradle build type (default: release); debug keeps the app debuggable "
                         "and is still signed with the shared test key so it installs as an "
                         "update over release builds")
    args = ap.parse_args()

    editions = ["lite", "full"] if args.edition == "both" else [args.edition]
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    java_home = find_java_home()
    if not java_home:
        fail("no JDK 17-21 found (~/.jdks/jbr-21.0.11 or JAVA_HOME); required by Gradle", TAG)
    log(f"JAVA_HOME = {java_home}", TAG)

    ensure_keystore(java_home)

    version = read_app_version()
    results = []
    for edition in editions:
        log(f"===== edition: {edition} =====", TAG)
        src_zip = ensure_data_zip(edition, args.pc_samp, args.rebuild, TAG)
        gdz, stats = recompress_to_gdz(edition, src_zip, force=args.recompress)
        overlay = prepare_overlay(edition, gdz, stats)
        task = "assembleRelease" if args.build_type == "release" else "assembleDebug"
        run_gradle(task, java_home, sign=True,
                   extra_args=[f"-PsampBundleAssets={overlay}"], tag=TAG)
        apk = gradle_apk_path(args.build_type)
        if not apk.is_file():
            fail(f"{edition}: expected output missing: {apk}", TAG)
        verify_apk(apk, edition)

        suffix = "-debug" if args.build_type == "debug" else ""
        final = out_dir / f"samp-mobile-{version}-{edition}-bundled{suffix}.apk"
        shutil.copyfile(apk, final)
        size_mb = final.stat().st_size / 1048576
        log(f"{edition}: FINAL APK -> {final} ({size_mb:.0f} MB)", TAG)
        results.append((edition, final, size_mb))

    print()
    log("===== summary =====", TAG)
    for edition, final, size_mb in results:
        print(f"  {edition:5s}  {size_mb:7.0f} MB  {final}")
    print()
    print("  两个包同包名同签名，可互相覆盖安装（后装者覆盖先装者）。")
    print("  安装后首次打开会提示解压内置资源，解压完成即可进服。")
    print("  注意：APK 内含 Rockstar 专有游戏数据，请勿公开发布（见 NOTICE）。")


if __name__ == "__main__":
    main()
