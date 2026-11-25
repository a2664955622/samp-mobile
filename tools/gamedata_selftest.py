#!/usr/bin/env python3
"""
gamedata 发布链路的离线契约自检（不需要设备、不需要 gradle）。

    python tools/gamedata_selftest.py

校验三件事：
  1. publish_gamedata.py 产出的 manifest.json 满足 DataManifest.java 的必填约束
  2. 分片大小之和 == 整包大小（客户端会硬性拒绝不一致）
  3. 分片内容真的能拼回那份 zip，且 SHA-256 对得上（端到端闭环）

第 3 条是关键：客户端下载完就是做这件事，这里提前跑一遍，避免把错的清单发上线。
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PUBLISH = ROOT / "tools" / "publish_gamedata.py"

passed = 0
failed = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global passed, failed
    if cond:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}" + (f"  [{detail}]" if detail else ""))


def is_hex64(s: str) -> bool:
    return isinstance(s, str) and len(s) == 64 and all(c in "0123456789abcdef" for c in s.lower())


def validate_manifest(m: dict) -> tuple[bool, str]:
    """按 DataManifest.parse 的规则校验，返回 (ok, 原因)。"""
    if m.get("schema") != 1:
        return False, f"schema={m.get('schema')}"
    if not m.get("version"):
        return False, "缺少 version"
    groups = [k for k in ("game", "online") if k in m]
    if not groups:
        return False, "既无 game 也无 online"

    for key in groups:
        g = m[key]
        pkgs = g.get("packages")
        if not pkgs:
            return False, f"{key}.packages 为空"
        for i, p in enumerate(pkgs):
            where = f"{key}.packages[{i}]"
            pid = p.get("id", "")
            if not pid:
                return False, f"{where} 缺少 id"
            if "/" in pid or "\\" in pid or ".." in pid:
                return False, f"{where}.id 含非法字符"
            if not isinstance(p.get("bytes"), int) or p["bytes"] <= 0:
                return False, f"{where}.bytes 非法"
            if not is_hex64(p.get("sha256", "")):
                return False, f"{where}.sha256 非 hex64"
            chunks = p.get("chunks")
            if not chunks:
                return False, f"{where}.chunks 为空"
            s = 0
            for j, c in enumerate(chunks):
                if not isinstance(c.get("index"), int) or c["index"] < 0:
                    return False, f"{where}.chunks[{j}].index 非法"
                if not c.get("url"):
                    return False, f"{where}.chunks[{j}] 缺 url"
                if not isinstance(c.get("bytes"), int) or c["bytes"] <= 0:
                    return False, f"{where}.chunks[{j}].bytes 非法"
                if c.get("sha256") and not is_hex64(c["sha256"]):
                    return False, f"{where}.chunks[{j}].sha256 非 hex64"
                s += c["bytes"]
            if s != p["bytes"]:
                return False, f"{where} 分片和 {s} != bytes {p['bytes']}"
    return True, ""


def main() -> int:
    if not PUBLISH.is_file():
        print(f"FAIL  找不到 {PUBLISH}")
        return 2

    work = Path(tempfile.mkdtemp(prefix="gdtest-"))
    try:
        src = work / "data"
        (src / "SAMP" / "fonts").mkdir(parents=True)
        (src / "a.txt").write_text("hello 数据包\n", encoding="utf-8")
        # 造一个略大于 1MB 的文件，确保至少两个分片逻辑被覆盖（用小 chunk 模拟）
        (src / "SAMP" / "big.bin").write_bytes(os.urandom(1_300_000))
        (src / "SAMP" / "fonts" / "chinese.ttf").write_bytes(os.urandom(2048))
        (src / ".hidden").write_text("should be skipped")

        out = work / "cdn"
        print("== 1. 打包产物结构 ==")
        # 用 512KB 分片，确保 1.3MB 的数据被切成多片（覆盖断点续传的多分片路径）
        env = dict(os.environ, SAMP_GAMEDATA_CHUNK_SIZE=str(512 * 1024))
        r = subprocess.run(
            [sys.executable, str(PUBLISH), "--game", str(src),
             "--out", str(out), "--work", str(work / "w"), "--version", "7"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
        check("publish_gamedata.py 退出码 0", r.returncode == 0, (r.stderr or "")[-300:])
        if r.returncode != 0:
            print(r.stderr)
            return 1

        mf = out / "manifest.json"
        check("生成 manifest.json", mf.is_file())
        m = json.loads(mf.read_text(encoding="utf-8"))

        print("\n== 2. 清单符合 DataManifest 契约 ==")
        ok, why = validate_manifest(m)
        check("清单通过客户端解析规则", ok, why)
        check("version 透传正确", m.get("version") == "7", str(m.get("version")))
        check("只给 --game 时不生成 online 分组", "online" not in m)

        print("\n== 3. 分片可拼回原 zip 且哈希一致 ==")
        pkg = m["game"]["packages"][0]
        chunks = pkg["chunks"]
        check(f"分片数 >= 3（实际 {len(chunks)}，覆盖多分片路径）", len(chunks) >= 3)
        check("分片序号连续从 0 开始",
              [c["index"] for c in chunks] == list(range(len(chunks))))

        rebuilt = b"".join((out / c["url"]).read_bytes() for c in chunks)
        check("拼装后大小 == manifest.bytes",
              len(rebuilt) == pkg["bytes"], f"{len(rebuilt)} vs {pkg['bytes']}")
        check("拼装后 SHA-256 == manifest.sha256",
              hashlib.sha256(rebuilt).hexdigest() == pkg["sha256"])

        for i, c in enumerate(chunks):
            data = (out / c["url"]).read_bytes()
            check(f"分片 {i} 大小与哈希一致",
                  len(data) == c["bytes"] and hashlib.sha256(data).hexdigest() == c["sha256"])

        print("\n== 4. 拼装结果仍是可解压的 zip ==")
        zpath = work / "rebuilt.zip"
        zpath.write_bytes(rebuilt)
        with zipfile.ZipFile(zpath) as zf:
            names = zf.namelist()
            bad = zf.testzip()
        check("zip 完整（无损坏条目）", bad is None, str(bad))
        check("包含 a.txt", "a.txt" in names, str(names))
        check("包含 SAMP/big.bin", "SAMP/big.bin" in names)
        check("隐藏文件被跳过", not any(n.startswith(".") for n in names), str(names))
        check("中文文件名正常", "SAMP/fonts/chinese.ttf" in names, str(names))

        print("\n== 5. 非法清单会被拒绝 ==")
        cases = {
            "schema 错误": {"schema": 2, "version": "1", "game": m["game"]},
            "缺 version": {"schema": 1, "game": m["game"]},
            "无分组": {"schema": 1, "version": "1"},
        }
        for name, bad_manifest in cases.items():
            good, _ = validate_manifest(bad_manifest)
            check(f"拒绝：{name}", not good)

        broken = json.loads(json.dumps(m))
        broken["game"]["packages"][0]["chunks"][0]["bytes"] += 1
        good, _ = validate_manifest(broken)
        check("拒绝：分片和与 bytes 不一致", not good)

        broken2 = json.loads(json.dumps(m))
        broken2["game"]["packages"][0]["id"] = "../../etc"
        good, _ = validate_manifest(broken2)
        check("拒绝：id 含路径穿越", not good)

    finally:
        shutil.rmtree(work, ignore_errors=True)

    print()
    print(f"{passed} passed, {failed} failed")
    if failed == 0:
        print("GAMEDATA SELFTEST OK")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
