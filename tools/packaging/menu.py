#!/usr/bin/env python3
"""
Interactive bilingual (中文/English) one-stop build menu — this is what
build_tool.bat launches when double-clicked. Zero commands to remember:
pick numbers, occasionally fill in a path, everything else is auto-detected.

Covers every routine build operation in one place:
  1) bundled APK (game data embedded)
  2) plain APK only (release/debug)
  3) game-data zip only (lite/full)
  4) GTA:SA runtime extraction (the once-per-clone prerequisite)

Language is picked explicitly at startup (choice remembered as the default for
next time) and can also be flipped inside the menu. It is a thin driver: real
work happens in build_bundled_apk.py / build_split.py /
tools/engine/prepare_gtasa.py (all scriptable from the CLI).
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))

from pkgutil import (
    APK_OUT_DIR,
    DIST_DIR,
    EDITIONS,
    GAMEDATA_OUT_DIR,
    detect_pc_samp,
    gtasa_libs_present,
    load_config,
    save_config_key,
)

for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


# ---------------------------------------------------------------------- i18n


def detect_lang() -> str:
    override = os.environ.get("SAMP_MENU_LANG", "").lower()
    if override in ("zh", "en"):
        return override
    if sys.platform == "win32":
        try:
            import ctypes
            langid = ctypes.windll.kernel32.GetUserDefaultUILanguage()
            # primary language identifier: low 10 bits; 0x04 = Chinese
            return "zh" if (langid & 0x3FF) == 0x04 else "en"
        except Exception:
            pass
    for var in ("LANG", "LC_ALL", "LC_MESSAGES"):
        val = os.environ.get(var, "")
        if val:
            return "zh" if val.lower().startswith("zh") else "en"
    return "en"


def pick_language() -> str:
    """Startup language picker — always shown, never silent auto-detection.
    The previous explicit choice becomes the default (bare Enter accepts);
    SAMP_MENU_LANG=zh|en skips the prompt entirely (scripting/testing)."""
    override = os.environ.get("SAMP_MENU_LANG", "").lower()
    if override in ("zh", "en"):
        return override
    saved = load_config().get("menuLang")
    default = saved if saved in ("zh", "en") else detect_lang()
    print()
    print("=" * 56)
    print("  SA-MP Mobile 构建工具 / Build Tool")
    print("=" * 56)
    print()
    print("  1) 中文")
    print("  2) English")
    hint = "1" if default == "zh" else "2"
    while True:
        try:
            raw = input(f"  请选择语言 / Choose language [1/2] (default {hint}): ").strip()
        except EOFError:
            print()
            raw = ""
        if not raw:
            raw = hint
        if raw in ("1", "2"):
            lang = "zh" if raw == "1" else "en"
            if lang != saved:
                save_config_key("menuLang", lang)
            return lang
        print("  请输入 1 或 2 / please enter 1 or 2")


LANG = pick_language()

S = {
    "title": ("SA-MP Mobile 构建工具", "SA-MP Mobile Build Tool"),
    "envcheck": ("环境检测：", "Environment check:"),
    "libs_ok": ("GTA:SA 运行库已提取（app/src/main/jniLibs）", "GTA:SA runtime libs extracted (app/src/main/jniLibs)"),
    "datasource": ("dist/ 里的游戏数据源（obb 或完整 apk）", "game data source in dist/ (obb or full apk)"),
    "gtasaapk": ("dist/ 里的 gtasa*.apk（提取运行库用）", "gtasa*.apk in dist/ (for runtime extraction)"),
    "pc_found": ("PC 版 GTA:SA + SA-MP：", "PC GTA:SA + SA-MP: "),
    "pc_missing": ("PC 版 GTA:SA + SA-MP 未自动检测到", "PC GTA:SA + SA-MP not auto-detected"),
    "font": ("中文字体", "CJK font"),
    "font_none": ("（可选，没有就用内置 fallback）", "(optional, bundled fallback used if absent)"),
    "localprops": ("local.properties（Android SDK 路径）", "local.properties (Android SDK path)"),
    "libs_hint": ("提示：还没提取 GTA:SA 运行库——任何 APK 构建前需先选 4 跑一次。",
                  "Hint: runtime libs not extracted yet — run option 4 once before any APK build."),
    "what": ("要做什么：", "What do you want to do:"),
    "opt_bundled": ("一体包 —— APK 内置游戏数据，装上解压即玩（推荐分发）",
                    "Bundled APK — game data embedded, install-extract-play (recommended)"),
    "opt_apk": ("普通 APK —— 不内置数据（release/debug 可选，配数据包用）",
                "Plain APK — no embedded data (release/debug, pairs with the data zip)"),
    "opt_zip": ("游戏数据包 zip —— 手动导入用（精简/完整可选）",
                "Game-data zip — for manual in-app import (lite/full)"),
    "opt_extract": ("提取 GTA:SA 运行库 —— 编译前提，整个项目只需跑一次",
                    "Extract GTA:SA runtime — build prerequisite, once per clone"),
    "opt_exit": ("退出", "Exit"),
    "opt_lang": ("Switch to English", "切换到中文"),
    "choose_main": ("请选择 [1-4/9/0]: ", "Choose [1-4/9/0]: "),
    "invalid": ("  输入无效，请输入有效选项", "  Invalid input, please enter a valid option"),
    "bundled_editions": ("一体包版本选择：", "Bundled edition:"),
    "ed_lite": ("精简版 —— 无音频流，APK 约 530 MB", "lite — no audio streams, APK ~530 MB"),
    "ed_full": ("完整版 —— 含全音频，APK 约 1.6 GB", "full — complete audio, APK ~1.6 GB"),
    "ed_both": ("两个都打", "build both"),
    "choose123": ("请选择 [1/2/3]（默认 1）: ", "Choose [1/2/3] (default 1): "),
    "first_build_note": (
        "首次打包需要几分钟到几十分钟（数据压缩 + 编译），之后重复打包只需一两分钟。",
        "The first build takes minutes to tens of minutes (data compression + compile); repeat packaging takes only a minute or two."),
    "done_apk": ("完成！APK 在 build\\apk\\ 下", "Done! APK(s) in build\\apk\\"),
    "done_zip": ("完成！数据包在 build\\gamedata\\ 下", "Done! Data zip(s) in build\\gamedata\\"),
    "failed": ("构建失败（退出码 {}），把上面的报错发出来排查。", "Build failed (exit code {}) — check the errors above."),
    "op_failed": ("操作失败（退出码 {}），检查上面的报错。", "Operation failed (exit code {}) — check the errors above."),
    "zip_edition": ("数据包版本：", "Data zip edition:"),
    "zip_lite": ("精简版 —— 无音频流，zip 约 1.0 GB", "lite — no audio streams, zip ~1.0 GB"),
    "zip_full": ("完整版 —— 含全音频，zip 约 2.1 GB", "full — complete audio, zip ~2.1 GB"),
    "choose12": ("请选择 [1/2]（默认 1）: ", "Choose [1/2] (default 1): "),
    "apk_type": ("APK 类型：", "APK type:"),
    "apk_release": ("release —— 正式签名版，适合分发（默认）", "release — signed, ready to distribute (default)"),
    "apk_debug": ("debug —— 调试版，适合自己开发", "debug — for your own development"),
    "need_pc1": ("需要构建数据包，但没有检测到 PC 版 GTA:SA（含 SA-MP 0.3.7）的安装目录。",
                 "The data zip needs building, but no PC GTA:SA (with SA-MP 0.3.7) directory was detected."),
    "need_pc2": ("例如：D:\\Program Files (x86)\\Rockstar Games\\GTA San Andreas",
                 "e.g. D:\\Program Files (x86)\\Rockstar Games\\GTA San Andreas"),
    "need_pc_prompt": ("请粘贴目录路径（留空跳过；输入一次以后会记住）: ",
                       "Paste the directory path (empty to skip; entered once, remembered afterwards): "),
    "pc_bad": ("  该路径不存在，按未提供处理。", "  That path does not exist — ignored."),
    "pc_saved": ("  已记住该路径（存于 dist\\packaging-config.json）", "  Path remembered (stored in dist\\packaging-config.json)"),
    "back": ("按回车返回主菜单…", "Press Enter to return to the main menu…"),
    "cancelled": ("\n已取消。", "\nCancelled."),
    "exited": ("\n已退出。", "\nExited."),
    "run": ("执行：", "Running: "),
    "ext_src": ("提取来源：", "Extraction source:"),
    "ext_dist": ("从 dist/ 里的 gtasa*.apk 提取（默认）", "from gtasa*.apk in dist/ (default)"),
    "ext_adb": ("从 adb 连接的已安装手机提取", "from a phone connected over adb (game already installed)"),
    "ext_noapk": ("dist/ 里没有 gtasa 开头的 apk——先把 GTA:SA v2.10 安装包改名放进去再试。",
                  "No gtasa*.apk in dist/ — drop the GTA:SA v2.10 package in first (filename starting with gtasa)."),
    "ext_done": ("提取完成，现在可以构建 APK 了。", "Extraction done — APKs can be built now."),
    "ext_redo": ("运行库已经提取过了；重跑会跳过未变化的文件，通常只在换包/修复时才需要。",
                 "Runtime libs already extracted; re-running skips unchanged files and is usually only needed when switching packages or repairing."),
    "sdk_ask": ("检测到 Android SDK 在 {}，自动写入 local.properties？[Y/n] ",
                "Android SDK detected at {} — write it into local.properties? [Y/n] "),
    "sdk_written": ("  已写入 local.properties", "  local.properties written"),
    "sdk_manual": ("未找到 Android SDK——请手动复制 local.properties.example 为 local.properties 并填入 sdk.dir。",
                   "Android SDK not found — manually copy local.properties.example to local.properties and fill in sdk.dir."),
    "pair_hint": ("提示：手动导入的分发方式 = 选项 2 的普通 APK + 选项 3 的数据包 zip。",
                  "Hint: the manual-import distribution = plain APK (option 2) + data zip (option 3)."),
}


def tr(key: str) -> str:
    zh, en = S[key]
    return en if LANG == "en" else zh


def ask(prompt: str, valid: list[str], default: str | None = None) -> str:
    """Loop until the user enters one of `valid` (empty input = default if given)."""
    while True:
        try:
            raw = input(prompt).strip()
        except EOFError:
            print()
            sys.exit(0)
        if not raw and default is not None:
            return default
        if raw.lower() in valid:
            return raw.lower()
        print(tr("invalid"))


# ---------------------------------------------------------------------- environment helpers


def local_properties_ok() -> bool:
    lp = REPO_ROOT / "local.properties"
    if not lp.is_file():
        return False
    try:
        return "sdk.dir" in lp.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return False


def detect_android_sdk() -> str | None:
    for var in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        val = os.environ.get(var)
        if val and os.path.isdir(val):
            return val
    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidate = os.path.join(local, "Android", "Sdk")
        if os.path.isdir(candidate):
            return candidate
    return None


def offer_local_properties_fix() -> None:
    """Once per run: if local.properties is missing, offer to auto-write it."""
    sdk = detect_android_sdk()
    if not sdk:
        print(tr("sdk_manual"))
        return
    answer = ask(tr("sdk_ask").format(sdk), ["y", "n"], default="y")
    if answer != "y":
        return
    # Gradle accepts forward slashes on Windows — no escaping headaches.
    content = "sdk.dir=" + sdk.replace("\\", "/") + "\n"
    try:
        (REPO_ROOT / "local.properties").write_text(content, encoding="utf-8")
        print(tr("sdk_written"))
    except OSError as e:
        print(f"  write failed: {e}")


# ---------------------------------------------------------------------- detection


def detect_inputs() -> dict:
    obbs = sorted(DIST_DIR.glob("*.obb"))
    gtasa_apks = sorted(p for p in DIST_DIR.glob("gtasa*.apk"))
    big_apks = sorted(
        p for p in DIST_DIR.glob("*.apk")
        if p.stat().st_size > 500 * 1024 * 1024 and not p.name.lower().startswith("gtasa")
    )
    fonts = sorted(
        p for p in DIST_DIR.iterdir()
        if p.is_file() and p.suffix.lower() in (".ttf", ".ttc", ".otf")
    ) if DIST_DIR.is_dir() else []
    return {
        "libs": gtasa_libs_present(),
        "obbs": obbs,
        "gtasa_apks": gtasa_apks,
        "big_apks": big_apks,
        "fonts": fonts,
        "pc_samp": detect_pc_samp(),
        "localprops": local_properties_ok(),
    }


def yn(ok: bool) -> str:
    return " √ " if ok else " × "


def print_status(info: dict) -> None:
    print(tr("envcheck"))
    print(f" [{yn(info['libs'])}] {tr('libs_ok')}")
    print(f" [{yn(info['localprops'])}] {tr('localprops')}")
    has_data_src = bool(info["obbs"] or info["big_apks"])
    detail = ""
    names = info["obbs"] or info["big_apks"]
    if names:
        detail = "（" + "、".join(p.name for p in names) + "）" if LANG == "zh" else " (" + ", ".join(p.name for p in names) + ")"
    print(f" [{yn(has_data_src)}] {tr('datasource')}{detail}")
    print(f" [{yn(bool(info['gtasa_apks']))}] {tr('gtasaapk')}")
    if info["pc_samp"]:
        print(f" [ √ ] {tr('pc_found')}{info['pc_samp']}")
    else:
        print(f" [ × ] {tr('pc_missing')}")
    font_note = f"（{info['fonts'][0].name}）" if info["fonts"] and LANG == "zh" else (
        f" ({info['fonts'][0].name})" if info["fonts"] else tr("font_none"))
    print(f" [{yn(bool(info['fonts']))}] {tr('font')} {font_note}")
    print()


def resolve_pc_samp(info: dict) -> str | None:
    """Auto-detected path, or ask the user to type one when it's actually needed.
    A manually entered path is persisted and reused from then on."""
    if info["pc_samp"]:
        return info["pc_samp"]
    print(tr("need_pc1"))
    print(tr("need_pc2"))
    try:
        raw = input(tr("need_pc_prompt")).strip().strip('"')
    except EOFError:
        return None
    if raw and os.path.isdir(raw):
        save_config_key("pcSampDir", raw)
        print(tr("pc_saved"))
        return raw
    if raw:
        print(tr("pc_bad"))
    return None


def data_zip_missing(editions: list[str]) -> bool:
    return any(not EDITIONS[e]["source_zip"].is_file() for e in editions)


def run_script(script: str, extra: list[str], engine: bool = False) -> int:
    base = REPO_ROOT / "tools" / ("engine" if engine else "packaging") / script
    cmd = [sys.executable, str(base)] + extra
    print()
    print("-" * 56)
    print(tr("run") + " ".join(cmd))
    print("-" * 56)
    sys.stdout.flush()
    proc = subprocess.run(cmd, cwd=REPO_ROOT)
    return proc.returncode


def report_done(rc: int, what: str) -> None:
    print()
    if rc == 0:
        if what == "apk":
            print(f"{tr('done_apk')}（{APK_OUT_DIR}）" if LANG == "zh" else f"{tr('done_apk')} ({APK_OUT_DIR})")
        elif what == "zip":
            print(f"{tr('done_zip')}（{GAMEDATA_OUT_DIR}）" if LANG == "zh" else f"{tr('done_zip')} ({GAMEDATA_OUT_DIR})")
        elif what == "extract":
            print(tr("ext_done"))
    else:
        print(tr("failed").format(rc) if what != "extract" else tr("op_failed").format(rc))


# ---------------------------------------------------------------------- flows


def bundled_flow(info: dict) -> None:
    print()
    print(tr("bundled_editions"))
    print(f"  1) {tr('ed_lite')}")
    print(f"  2) {tr('ed_full')}")
    print(f"  3) {tr('ed_both')}")
    choice = ask(tr("choose123"), ["1", "2", "3"], default="1")
    editions = {"1": ["lite"], "2": ["full"], "3": ["lite", "full"]}[choice]
    edition_arg = {"1": "lite", "2": "full", "3": "both"}[choice]

    print()
    print(tr("apk_type"))
    print(f"  1) {tr('apk_release')}")
    print(f"  2) {tr('apk_debug')}")
    bt = ask(tr("choose12"), ["1", "2"], default="1")
    build_type = "release" if bt == "1" else "debug"

    extra = ["--edition", edition_arg, "--build-type", build_type]
    if data_zip_missing(editions):
        pc = resolve_pc_samp(info)
        if pc:
            extra += ["--pc-samp", pc]
    print()
    print(tr("first_build_note"))
    rc = run_script("build_bundled_apk.py", extra)
    report_done(rc, "apk")


def apk_flow(info: dict) -> None:
    print()
    print(tr("apk_type"))
    print(f"  1) {tr('apk_release')}")
    print(f"  2) {tr('apk_debug')}")
    at = ask(tr("choose12"), ["1", "2"], default="1")
    apk_type = "release" if at == "1" else "debug"
    rc = run_script("build_split.py", ["--apk-only", "--apk", apk_type])
    report_done(rc, "apk")


def zip_flow(info: dict) -> None:
    print()
    print(tr("zip_edition"))
    print(f"  1) {tr('zip_lite')}")
    print(f"  2) {tr('zip_full')}")
    ed = ask(tr("choose12"), ["1", "2"], default="1")
    edition = "lite" if ed == "1" else "full"

    extra = ["--data-only", "--edition", edition]
    if data_zip_missing([edition]):
        pc = resolve_pc_samp(info)
        if pc:
            extra += ["--pc-samp", pc]
    rc = run_script("build_split.py", extra)
    report_done(rc, "zip")


def extract_flow(info: dict) -> None:
    print()
    if info["libs"]:
        print(tr("ext_redo"))
        print()
    print(tr("ext_src"))
    print(f"  1) {tr('ext_dist')}")
    print(f"  2) {tr('ext_adb')}")
    src = ask(tr("choose12"), ["1", "2"], default="1")

    if src == "1" and not info["gtasa_apks"]:
        print()
        print(tr("ext_noapk"))
        return
    extra = [] if src == "1" else ["--from-adb"]
    rc = run_script("prepare_gtasa.py", extra, engine=True)
    report_done(rc, "extract")


# ---------------------------------------------------------------------- main loop


def main() -> None:
    global LANG
    os.chdir(REPO_ROOT)
    props_offered = False
    while True:
        print()
        print("=" * 56)
        print(f"  {tr('title')}")
        print("=" * 56)
        info = detect_inputs()
        print_status(info)

        if not info["localprops"] and not props_offered:
            props_offered = True
            offer_local_properties_fix()
            print()
            info = detect_inputs()

        if not info["libs"]:
            print(tr("libs_hint"))
            print()

        print(tr("what"))
        print(f"  1) {tr('opt_bundled')}")
        print(f"  2) {tr('opt_apk')}")
        print(f"  3) {tr('opt_zip')}")
        print(f"  4) {tr('opt_extract')}")
        print(f"  9) {tr('opt_lang')}")
        print(f"  0) {tr('opt_exit')}")
        print()
        print(tr("pair_hint"))
        choice = ask(tr("choose_main"), ["1", "2", "3", "4", "9", "0"], default="0")

        if choice == "0":
            return
        if choice == "9":
            LANG = "en" if LANG == "zh" else "zh"
            continue
        try:
            if choice == "1":
                bundled_flow(info)
            elif choice == "2":
                apk_flow(info)
            elif choice == "3":
                zip_flow(info)
            elif choice == "4":
                extract_flow(info)
        except KeyboardInterrupt:
            print(tr("cancelled"))
        print()
        try:
            input(tr("back"))
        except EOFError:
            return


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(tr("exited"))
