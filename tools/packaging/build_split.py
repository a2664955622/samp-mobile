#!/usr/bin/env python3
"""
Build the split distribution: a plain APK (no embedded game data) plus a
standalone game-data .zip the user imports manually in the launcher.

Use this when the recipient is comfortable copying a zip onto the phone and
importing it via 设置 → 文件验证 → 本地导入 (or when you want a small APK).

Outputs:

  build/apk/samp-mobile-<version>.apk          (release, signed with dist/friends.jks)
  build/apk/samp-mobile-<version>-debug.apk    (debug)
  build/gamedata/gamedata-lite.zip             (no audio streams, ~1.04 GiB)
  build/gamedata/gamedata-full.zip             (full audio, ~2.10 GiB; --edition full|both)

Usage:
  python tools/packaging/build_split.py                          # lite zip + release APK
  python tools/packaging/build_split.py --edition full           # full-audio zip
  python tools/packaging/build_split.py --apk debug              # debug APK instead
  python tools/packaging/build_split.py --data-only              # just the zip, no APK
  python tools/packaging/build_split.py --apk-only               # just the APK, no zip
  python tools/packaging/build_split.py --rebuild                # regenerate the zip from scratch

NOTE: the data zip contains proprietary Rockstar game data — do not publish
it (see NOTICE).
"""

from __future__ import annotations

import argparse
import shutil
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from pkgutil import (
    APK_OUT_DIR,
    EDITIONS,
    GAMEDATA_OUT_DIR,
    ensure_data_zip,
    ensure_keystore,
    fail,
    find_java_home,
    gradle_apk_path,
    gtasa_libs_present,
    log,
    read_app_version,
    run_gradle,
)

TAG = "split-build"


def verify_plain_apk(apk: Path) -> None:
    problems = []
    with zipfile.ZipFile(apk, "r") as z:
        names = set(z.namelist())
        for abi in ("arm64-v8a", "armeabi-v7a"):
            if f"lib/{abi}/libSAMPMOBILE.so" not in names:
                problems.append(f"lib/{abi}/libSAMPMOBILE.so missing")
            if f"lib/{abi}/libGTASA.so" not in names:
                problems.append(f"lib/{abi}/libGTASA.so missing")
        if "assets/gamedata/bundle.gdz" in names:
            problems.append(
                "assets/gamedata/bundle.gdz present — this is supposed to be a plain "
                "APK without embedded data (stale overlay leak?)"
            )
    if problems:
        for p in problems:
            print(f"[{TAG}]   VERIFY FAIL: {p}", file=sys.stderr)
        fail(f"APK verification failed: {apk}", TAG)
    log("APK content verification passed (dual-ABI libs present, no embedded bundle)", TAG)


def main() -> None:
    ap = argparse.ArgumentParser(description="Build plain APK + standalone game-data zip (split distribution).")
    ap.add_argument("--edition", choices=["lite", "full", "both"], default="lite",
                    help="which data zip(s) to produce (default: lite)")
    ap.add_argument("--apk", choices=["release", "debug"], default="release",
                    help="which APK build type to produce (default: release, signed with the shared test key)")
    ap.add_argument("--data-only", action="store_true", help="skip the APK build")
    ap.add_argument("--apk-only", action="store_true", help="skip the data zip")
    ap.add_argument("--pc-samp", default=None,
                    help="PC GTA:SA dir with SA-MP 0.3.7 (default: auto-detect common install paths)")
    ap.add_argument("--rebuild", action="store_true",
                    help="regenerate the data zip(s) via build_gamedata.py even if they exist")
    args = ap.parse_args()

    if args.data_only and args.apk_only:
        fail("--data-only and --apk-only are mutually exclusive", TAG)

    java_home = find_java_home()
    if not java_home:
        fail("no JDK 17-21 found (~/.jdks/jbr-21.0.11 or JAVA_HOME); required by Gradle", TAG)
    log(f"JAVA_HOME = {java_home}", TAG)

    zips = []
    if not args.apk_only:
        editions = ["lite", "full"] if args.edition == "both" else [args.edition]
        for edition in editions:
            zips.append((edition, ensure_data_zip(edition, args.pc_samp, args.rebuild, TAG)))

    final_apk = None
    if not args.data_only:
        if not gtasa_libs_present():
            fail(
                "GTA:SA runtime libs not extracted yet — run once first:\n"
                "  python tools/engine/prepare_gtasa.py\n"
                "(put the gtasa*.apk into dist/ beforehand)",
                TAG,
            )
        build_type = args.apk
        if build_type == "release":
            ensure_keystore(java_home)
        run_gradle(f"assemble{build_type.capitalize()}", java_home,
                   sign=(build_type == "release"), tag=TAG)
        apk = gradle_apk_path(build_type)
        if not apk.is_file():
            fail(f"expected output missing: {apk}", TAG)
        verify_plain_apk(apk)

        version = read_app_version()
        suffix = "" if build_type == "release" else "-debug"
        APK_OUT_DIR.mkdir(parents=True, exist_ok=True)
        final_apk = APK_OUT_DIR / f"samp-mobile-{version}{suffix}.apk"
        shutil.copyfile(apk, final_apk)
        log(f"FINAL APK -> {final_apk} ({final_apk.stat().st_size / 1048576:.0f} MB)", TAG)

    print()
    log("===== summary =====", TAG)
    for edition, z in zips:
        print(f"  数据包 {edition:5s}  {z.stat().st_size / 1048576:7.0f} MB  {z}")
    if final_apk:
        print(f"  APK        {final_apk.stat().st_size / 1048576:7.0f} MB  {final_apk}")
    print()
    if zips and final_apk:
        print("  使用方式：")
        print("  1. 安装 APK，把数据 zip 拷到手机任意目录（如 Download/）")
        print("  2. 打开应用 → 设置 → 文件验证 → 本地导入，选中 zip 一键导入")
        print("     （进服时「缺少游戏数据包」对话框里也有一键导入按钮）")
    print("  注意：数据包含 Rockstar 专有游戏数据，请勿公开发布（见 NOTICE）。")


if __name__ == "__main__":
    main()
