#!/usr/bin/env python3
"""
Shared machinery for the packaging entry scripts (build_bundled_apk.py / build_split.py).

Owns every path and constant in one place so the two user-facing scripts never disagree:

  dist/                      pure INPUT area — obb/apk/font/friends.jks live here
  build/apk/                 final APK artifacts
  build/gamedata/            data zip artifacts (gamedata-lite.zip / gamedata-full.zip)
  build/gamedata/.cache/     DEFLATE recompression cache for the bundled flow
  build/bundled-apk/         internal assets-overlay staging for the bundled flow

`build/` may be wiped wholesale (gradlew clean does it too) — everything inside is
regenerable. `dist/friends.jks` is the ONE exception that must survive: it is the
test-signing key shared by every distributed APK; losing it forces every device
to uninstall/reinstall instead of updating in place.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DIST_DIR = REPO_ROOT / "dist"
BUILD_DIR = REPO_ROOT / "build"
APK_OUT_DIR = BUILD_DIR / "apk"
GAMEDATA_OUT_DIR = BUILD_DIR / "gamedata"
GDZ_CACHE_DIR = GAMEDATA_OUT_DIR / ".cache"
OVERLAY_ROOT = BUILD_DIR / "bundled-apk"

KEYSTORE_PATH = DIST_DIR / "friends.jks"
# Throwaway test-signing credentials, fixed on purpose: the key itself is the
# only thing protecting update compatibility, and it must simply never change.
# (Filename kept as-is forever — renaming it would regenerate the key and break
# update-in-place for every device that installed a previous build.)
KEYSTORE_PASSWORD = "sampmobile-friend-test"
KEY_ALIAS = "friends"

# Small persistent user config (entered paths etc.) — lives next to the keystore
# because both must survive `gradlew clean`, which wipes build/.
CONFIG_PATH = DIST_DIR / "packaging-config.json"

EDITIONS = {
    "lite": {
        "source_zip": GAMEDATA_OUT_DIR / "gamedata-lite.zip",
        "label": "精简版",
        "keep_audio": False,
    },
    "full": {
        "source_zip": GAMEDATA_OUT_DIR / "gamedata-full.zip",
        "label": "完整版（含全音频）",
        "keep_audio": True,
    },
}

DEFAULT_PC_SAMP_CANDIDATES = [
    r"D:\Program Files (x86)\Rockstar Games\GTA San Andreas",
    r"C:\Program Files (x86)\Rockstar Games\GTA San Andreas",
    r"E:\Program Files (x86)\Rockstar Games\GTA San Andreas",
    r"D:\Games\GTA San Andreas",
    r"E:\Games\GTA San Andreas",
]

CHUNK = 1024 * 1024

# PowerShell displays mojibake when Python falls back to the legacy console codepage.
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def log(msg: str, tag: str = "packaging") -> None:
    print(f"[{tag}] {msg}", flush=True)


def fail(msg: str, tag: str = "packaging") -> None:
    print(f"[{tag}] ERROR: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


# ---------------------------------------------------------------------- environment


def find_java_home() -> str | None:
    candidate = Path.home() / ".jdks" / "jbr-21.0.11"
    if (candidate / "bin" / "java.exe").is_file():
        return str(candidate)
    jdks = Path.home() / ".jdks"
    if jdks.is_dir():
        for sub in sorted(jdks.iterdir()):
            if sub.name.startswith(("jbr-21", "jbr-17")) and (sub / "bin" / "java.exe").is_file():
                return str(sub)
    env = os.environ.get("JAVA_HOME")
    if env and (Path(env) / "bin" / "java.exe").is_file():
        return env
    return None


def read_app_version() -> str:
    gradle = (REPO_ROOT / "app" / "build.gradle").read_text(encoding="utf-8")
    for line in gradle.splitlines():
        line = line.strip()
        if line.startswith("versionName"):
            return line.split('"')[1]
    return "unknown"


def gtasa_libs_present() -> bool:
    jni = REPO_ROOT / "app" / "src" / "main" / "jniLibs"
    return (jni / "arm64-v8a" / "libGTASA.so").is_file()


def ensure_keystore(java_home: str) -> Path:
    if KEYSTORE_PATH.is_file():
        return KEYSTORE_PATH
    keytool = Path(java_home) / "bin" / ("keytool.exe" if os.name == "nt" else "keytool")
    if not keytool.is_file():
        fail(f"keytool not found at {keytool}; cannot generate the test keystore")
    log(f"generating throwaway test keystore -> {KEYSTORE_PATH}")
    cmd = [
        str(keytool), "-genkeypair",
        "-keystore", str(KEYSTORE_PATH),
        "-alias", KEY_ALIAS,
        "-keyalg", "RSA", "-keysize", "2048",
        "-validity", "10950",
        "-storepass", KEYSTORE_PASSWORD,
        "-keypass", KEYSTORE_PASSWORD,
        "-dname", "CN=SAMP Mobile Test Sign, OU=dev, O=local, L=local, ST=local, C=CN",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not KEYSTORE_PATH.is_file():
        fail(f"keytool failed: {proc.stdout}\n{proc.stderr}")
    return KEYSTORE_PATH


# ---------------------------------------------------------------------- gradle


def run_gradle(task: str, java_home: str, sign: bool, extra_args: list[str] | None = None,
               tag: str = "packaging") -> None:
    env = dict(os.environ)
    env["JAVA_HOME"] = java_home
    if sign:
        env["SAMP_STORE_FILE"] = str(KEYSTORE_PATH)
        env["SAMP_STORE_PASSWORD"] = KEYSTORE_PASSWORD
        env["SAMP_KEY_ALIAS"] = KEY_ALIAS
        env["SAMP_KEY_PASSWORD"] = KEYSTORE_PASSWORD

    gradlew = REPO_ROOT / ("gradlew.bat" if os.name == "nt" else "gradlew")
    args = [task, "--offline", "--console=plain"] + (extra_args or [])
    # CreateProcess cannot execute .bat files directly — route through cmd.exe.
    cmd = ["cmd", "/c", str(gradlew)] + args if os.name == "nt" else [str(gradlew)] + args
    log(f"gradle {task} (native libs cached after the first build)…", tag)
    t0 = time.time()
    proc = subprocess.run(cmd, cwd=REPO_ROOT, env=env)
    if proc.returncode != 0:
        fail(f"gradle {task} failed with exit code {proc.returncode}", tag)
    log(f"{task} done in {(time.time() - t0) / 60:.1f} min", tag)


def gradle_apk_path(build_type: str) -> Path:
    return REPO_ROOT / "build" / "app" / "outputs" / "apk" / build_type / "samp.apk"


# ---------------------------------------------------------------------- data zip


def load_config() -> dict:
    try:
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}


def save_config_key(key: str, value: str) -> None:
    cfg = load_config()
    cfg[key] = value
    try:
        CONFIG_PATH.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    except OSError:
        pass


def detect_pc_samp() -> str | None:
    """PC GTA:SA+SA-MP dir: user-entered config first (explicit beats guesswork),
    then common install locations."""
    saved = load_config().get("pcSampDir")
    if saved and os.path.isdir(saved):
        return saved
    return next((p for p in DEFAULT_PC_SAMP_CANDIDATES if os.path.isdir(p)), None)


def ensure_data_zip(edition: str, pc_samp: str | None, rebuild: bool,
                    tag: str = "packaging") -> Path:
    """Make sure the STORED source data zip for an edition exists, invoking
    build_gamedata.py when missing (or when rebuild is forced)."""
    spec = EDITIONS[edition]
    src = spec["source_zip"]
    if src.is_file() and not rebuild:
        log(f"{edition}: reusing {src} ({src.stat().st_size / 1048576:.1f} MB)", tag)
        return src

    if src.exists() and rebuild:
        log(f"{edition}: rebuild requested, regenerating {src.name} from scratch", tag)
    pc = pc_samp or detect_pc_samp()
    if not pc:
        fail(
            f"{edition}: source zip {src} not found and no PC GTA:SA+SA-MP 0.3.7 directory "
            f"detected; pass --pc-samp <dir> or drop the zip in place yourself.",
            tag,
        )

    src.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        str(REPO_ROOT / "tools" / "gamedata" / "build_gamedata.py"),
        "--pc-samp", pc,
        "--out", str(src),
    ]
    if spec["keep_audio"]:
        cmd.append("--keep-audio")
    log(f"{edition}: building data zip — {' '.join(cmd)}", tag)
    t0 = time.time()
    proc = subprocess.run(cmd, cwd=REPO_ROOT)
    if proc.returncode != 0 or not src.is_file():
        fail(f"{edition}: build_gamedata.py failed with exit code {proc.returncode}", tag)
    log(f"{edition}: data zip built in {(time.time() - t0) / 60:.1f} min -> {src}", tag)
    return src


# ---------------------------------------------------------------------- small helpers


def hardlink_or_copy(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def zip_entry_count(path: Path) -> int:
    with zipfile.ZipFile(path, "r") as z:
        return sum(1 for i in z.infolist() if not i.is_dir())
