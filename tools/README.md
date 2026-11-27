# tools/

开发、构建、打包与逆向工具目录（所有自动化检测与测试套件统一位于 `tests/`）。

> **日常使用不需要读这里**：双击仓库根的 `build_tool.bat`，中英双语菜单会引导你完成所有日常构建（一体包/普通 APK/数据包/提取运行库）。
> 这里记录的是菜单背后的脚本与内部引擎，供脚本化调用或排查时参考。

## 目录结构

### `tools/packaging/` —— 打包入口（用户面向）

| 入口 | 用途 |
|---|---|
| `menu.py` | 交互式双语菜单（由仓库根 `build_tool.bat` 双击调起，启动时选语言并记住，菜单内也可切换）：环境自检（含 `local.properties` 一键写入）→ 四个操作「一体包 / 普通 APK / 数据 zip / 提取运行库」→ 自动调用下面的脚本与 `prepare_gtasa.py`；PC GTA:SA 路径检测不到时提示输入一次并记住（`dist/packaging-config.json`） |
| `build_bundled_apk.py` | **一体包**：游戏数据重压缩后嵌入 APK assets，产出安装即玩的 APK 到 `build/apk/`（`--build-type release|debug` 可选，默认 release；两者都用 friends 测试签名，可互相覆盖安装）。首启一键解压，无需联网、无需手动导 zip |
| `build_split.py` | **分开构建**：普通 APK（release/debug 可选）产出到 `build/apk/`，独立数据 zip 产出到 `build/gamedata/`；适合愿意手动「设置 → 文件验证 → 本地导入」的场景 |
| `pkgutil.py` | 两个入口脚本共用的内部模块：路径常量、JDK 检测、测试签名（`dist/friends.jks`）、用户配置、gradle 调用、数据包构建驱动。**不是入口，勿直接运行** |

两个脚本共同的命令行约定：`--edition lite|full|both`（数据版本）、`--rebuild`（强制重建数据 zip）、`--pc-samp`（PC GTA:SA+SA-MP 0.3.7 目录，默认自动探测常见安装路径）。

产物与缓存布局（全部可删重建，`gradlew clean` 也会连带清掉）：

```
build/apk/          最终 APK
build/app/          gradle 模块输出（原 app/build，已重定向；app/ 只留源码）
build/gamedata/     数据包 zip 产物 + .cache/（一体包 deflate 重压缩缓存）
build/bundled-apk/  一体包 assets overlay 暂存
dist/               纯输入区：gtasa*.apk、obb、字体 + friends.jks（测试签名，勿删）+ packaging-config.json（用户填过的路径）
```

### `tools/engine/` —— GTA:SA v2.10 引擎提取与离线逆向（内部）

| 脚本 | 用途 |
|---|---|
| `prepare_gtasa.py` | 构建前提：从 GTA:SA v2.10 APK（`dist/gtasa*.apk`）或已安装设备（`--from-adb`）提取 `libGTASA.so`、`libImmEmulatorJ.so` 与轻量 APK 资产。自动剥离 Fastman92 注入、修复程序头表位置（防 x86 模拟器闪退）；支持 `--libs-only` 与 `--clean` |
| `gtasa_inspect.py` | 零依赖双 ABI `libGTASA.so` 离线反汇编工具：`find`（查符号）、`disasm`（还原 PLT/GOT）、`xrefs`（全库交叉引用扫描） |

### `tools/gamedata/` —— 数据包编译器（内部，被打包脚本调用）

| 脚本 | 用途 |
|---|---|
| `build_gamedata.py` | 从官方 GTA:SA Android v2.10（`dist/` 里的 obb/apk）+ PC 版 GTA:SA + SA-MP 0.3.7（`--pc-samp`）自动转换贴图/模型/碰撞，打包出启动器可一键导入的数据 zip（默认 `build/gamedata/gamedata-lite.zip`）。需 `numpy`；`--keep-audio` 保留全音频流 |

### `tools/netcap/` —— `/netcap` 抓包离线分析（内部，测试引用）

| 源码 | 用途 |
|---|---|
| `ncapdump.cpp` | 解析游戏内 `/netcap` 录制的二进制抓包，列出逐条收发包摘要 |
| `ncapreplay.cpp` | 离线回放抓包，校验客户端 `Net/Proto` 解包行为 |

> `tools/netcap/` 由 `tests/native/CMakeLists.txt` 一起编译至 `build/native-tests/`，确保始终与客户端 `Net/Proto/` 源码保持同步。
