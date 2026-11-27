# SA-MP Mobile

**[English](README.md) | 简体中文**

面向 **GTA: San Andreas Android v2.10** 的 Android SA-MP 联机客户端（Java 启动器 + C++ 原生库 `libSAMPMOBILE.so`），支持 `arm64-v8a` 与 `armeabi-v7a` 双架构构建。

> 版权、第三方组件与合规声明请参阅 [NOTICE](NOTICE)。

![启动器主界面](images/4.jpg)

---

## 项目简介

本项目基于开源的 Alyn SA-MP Mobile 架构持续演进，面向现代 Android（支持至 **Android 14 / 15，SDK 35**）完成了**启动器全面重写、原生协议层与引擎层地基重构、SA-MP / `open.mp` 广度与深度行为对齐、五合一模组生态共存以及全链路稳定性加固**。

客户端设计追求**「向 PC 端 SA-MP / `open.mp` 行为严格对齐，不为移动化而破坏联机协议与反作弊契约」**：凡涉及核心同步、战斗判定、载具状态机、单机残留屏蔽与服务器反作弊兼容的环节，均以 `open.mp` 上游源码、官方文档及 `libGTASA.so`（arm64 / arm32）反汇编作为对照基准；移动化改造仅限于触屏交互、Android 窗口/进程隔离、高分中文字体图集与现代系统安全策略适配。

本项目不定期维护，完全开源透明，不含任何隐藏逻辑或私有功能。仓库不含游戏资源与商用字体——所有运行文件由用户从自己的游戏副本提取。欢迎构建、修改与 fork；衍生版本须以 **GNU GPL v3.0** 同等开源。遇到问题欢迎提 Issue。

---

## 快速开始

**一个入口：`build_tool.bat`**（双击运行，启动时选中/英文——选过一次以后回车即可）。所有日常构建操作都在菜单里：

```
1) 一体包 —— APK 内置游戏数据，装上解压即玩（推荐分发）
2) 普通 APK —— 不内置数据（release/debug 可选）
3) 游戏数据包 zip —— 手动导入用（精简/完整可选）
4) 提取 GTA:SA 运行库 —— 编译前提，整个项目只需跑一次
```

**首次使用三步：**

1. 把三样东西放进仓库根的 `dist/` 文件夹：
   - GTA:SA v2.10 的安装包，**文件名以 `gtasa` 开头**（如 `gtasa_2.10_arm64.apk`；32/64 位两个包就都放）
   - 游戏数据 `main`/`patch` **`.obb`**（或 2 GB+ 完整版 apk，直接放，不用改名）
   - （可选）开源中文字体，文件名带 `font`（`.ttf`/`.ttc`/`.otf`）
2. **双击 `build_tool.bat`**——菜单会自动检测环境：缺 `local.properties` 时检测到 Android SDK 可一键写入；PC 版 GTA:SA 路径检测不到时让你填一次（之后记住）
3. 先选 **4** 提取运行库（只跑一次），再选你要构建的东西；产物在 **`build\apk\`** 和 **`build\gamedata\`**

两种分发方式的区别：

| 方式 | 安装后的体验 | 体积 | 适合 |
|---|---|---|---|
| **一体包**（推荐） | 装上 → 首启点「立即解压」→ 等几分钟 → 进服玩 | 精简版 ~530 MB / 完整版 ~1.6 GB | 零操作门槛，最省事 |
| **普通 APK + 数据包**（菜单 2 + 3） | 装 APK + 拷贝 zip 到手机 → 设置→文件验证→本地导入 | APK ~60 MB + zip 1.0/2.1 GB | 愿意动手导包；或想分开更新 APK 与数据 |

> - 精简版 = 无音频流（音效/电台静音），完整版 = 全音频；同包名同签名，可互相覆盖安装。
> - **完整版解压后占用约 2.1 GB + APK 自身 1.6 GB，设备至少预留 ~4 GB 空间**（精简版约一半）。
> - `dist/friends.jks` 是测试签名（首次自动生成），**别删它**——删了之后打的包无法覆盖安装，只能卸载重装。
> - 开发调试选菜单 2 的 debug 即可；懂行的人也可以直接 `.\gradlew.bat assembleDebug`（先配好 JDK 17-21 的 `JAVA_HOME`，见环境表）。macOS / Linux 直接跑 `tools/packaging/` 下的脚本（参数见 `--help`）。
> - **合规**：产出的 APK/zip 内含 Rockstar 专有游戏数据，**请勿公开发布**（见 [NOTICE](NOTICE)）。

---

## 资源准备明细

仓库只含源码与开源依赖，以下资源需自备（版权合规要求）：

| 资源 | 用途 | 怎么提供 |
|---|---|---|
| **GTA:SA Android v2.10 安装包** | 提取 `libGTASA.so` 等运行库 + 轻量 assets（编译 APK 必需） | 文件名以 `gtasa` 开头放进 `dist/`（不区分大小写；多个 apk 会一起处理），跑一次 `prepare_gtasa.py`；手机已安装的话也可 `--from-adb` 直接提取 |
| **GTA:SA 游戏数据**（obb 或 2GB+ 完整 apk） | 单机底图/模型/动画/贴图（打数据包必需） | `main`/`patch` `.obb` 直接放进 `dist/` 根目录，脚本自动识别 |
| **PC 版 GTA:SA + SA-MP 0.3.7** | `SAMP.img`/`SAMP.ide`/`SAMPCOL.img` 与 PC 文字贴图 | 打包脚本会自动探测常见安装路径；探测不到会在菜单里让你填路径，或用 `--pc-samp` 指定 |
| **开源中文字体**（如思源黑体） | 生成 `SAMP/fonts/chinese.ttf` 中文渲染 | 文件名带 `font` 放进 `dist/`；省略则用内置 Roboto 兜底 |

**v2.10 注意事项：**

- Google Play 现已更新到 v2.11+，其底层内存布局与本客户端不兼容——**必须用 v2.10**。
- 市面上常见的修改版 v2.10 包带 Fastman92 注入，`prepare_gtasa.py` 会自动剥离并修复程序头表位置（不修复则 x86 模拟器进游戏秒闪退）；官方原版库不受影响。
- **若你的 `libGTASA.so` 是 2026-11-23 之前提取的**（脚本还没有上述修复），在模拟器上会闪退，重跑一次提取并重编译即可。
- 只有 64 位的包也能用（现代 64 位手机没问题），脚本仅提示不缺 32 位。

`prepare_gtasa.py` 的其他用法：`--apk` 手动指定 apk 路径、`--libs-only` 只刷新库不动 assets、`--clean` 清理已提取的专有文件。

---

## 构建环境

| 工具 | 版本要求 | 说明 |
|---|---|---|
| **JDK** | **17 – 21**（推荐 21） | Gradle 8.13 不支持 JDK 25+（报 `major version 69`） |
| **Android SDK** | `compileSdk 35` / `targetSdk 35` / `minSdk 24` | 需 Platform 35 与 Build-Tools 35；首次 clone 后复制 `local.properties.example` 为 `local.properties` 填入 SDK 路径（Android Studio 打开会自动生成） |
| **Android NDK** | **`25.1.8937393`** | `app/build.gradle` 已锁定 |
| **CMake** | **`3.22.1`**（或兼容版本） | 可用 Android Studio SDK Manager 安装 |
| **Gradle** | **8.13**（仓库自带 Wrapper） | 无需全局安装 |
| **Python 3** | 3.10+ | `tools/` 脚本用；数据包编译需要 `numpy`（`pip install numpy`） |

其他可选项：

- **Release 签名**：`local.properties` 或环境变量配置 `SAMP_STORE_FILE` / `SAMP_STORE_PASSWORD` / `SAMP_KEY_ALIAS` / `SAMP_KEY_PASSWORD` 后 `./gradlew assembleRelease`（签名文件切勿提交）。
- **Firebase Crashlytics**：按 `app/google-services.json.example` 放置 `app/google-services.json` 即自动启用，默认关闭。
- **自定义启动器背景**：把 `launcher_backdrop.jpg`（`.png`/`.webp` 亦可）放进 `app/src/main/res/drawable-nodpi/` 重编译即可替换默认渐变背景，删除该文件恢复默认。建议 2400×1080，主体避开最左侧 1/10（会被导航栏遮挡）。

---

## 数据包手动导入（手机端操作）

1. 把 `build/gamedata/gamedata-lite.zip`（或 `-full.zip`）拷到手机任意目录（如 `Download/`），无需手动操作 `Android/data/`；
2. 打开应用 → **设置 → 文件验证 → 本地导入** 选中 zip——进服时「缺少游戏数据包」对话框里也有「一键导入 .zip」按钮；
3. 启动器流式解压到应用私有 `files/` 目录，保留已有 `settings.json` 与服务器列表，完全绕开 Android 14 目录权限坑。

> 数据包也可用命令行单独构建：`python tools/gamedata/build_gamedata.py --pc-samp "<PC GTA:SA 目录>"`（可选 `--keep-audio` 保留全音频、`--out` 指定输出、`--cjk-font` 指定字体）。

---

## 本地校验与测试

无需设备，统一入口 `tests/check.py` 跑客观校验与自动化测试（详见 [tests/README.md](tests/README.md)）：

```powershell
python tests/check.py            # 日常快检（合规、平台契约、双 ABI 符号/JNI、APK 工具校验）
python tests/check.py --full     # 全量（追加 C++ 电脑端单测、Gradle JVM 单测）
python tests/check.py --release  # 发包前严格门禁（追加 Android Lint）
```

- C++ 单测可单独跑（WSL/Linux）：`sh tests/native/run.sh`
- JVM 单测可单独跑：`./gradlew testDebugUnitTest`

---

## 界面截图

![模组与补丁管理](images/1.jpg)

![游戏设置与自定义主题](images/2.jpg)

![截图画廊与游戏实况](images/3.jpg)

---

## 常见问题

- **报 `Unsupported class file major version 69`**：当前 `JAVA_HOME` 是 JDK 25+，换成 JDK 17–21 重新编译。
- **Android 14+ 上 AML / MonetLoader 加载异常**：保持 `app/build.gradle` 的 `useLegacyPackaging = true` 与 `AndroidManifest.xml` 的 `android:allowNativeHeapPointerTagging="false"` 不要动。
- **首次 Gradle 拉依赖慢**：`settings.gradle` 已在官方仓库后配置镜像兜底。
- **`No APK given and no dist/gtasa*.apk found`**：`dist/` 里没有符合命名规则的 APK——检查位置（必须在 `dist/` 根目录）、扩展名（`.apk`，`.xapk`/`.apks` 需先解出内层 apk）与文件名前缀（必须以 `gtasa` 开头）。
- **模拟器上进游戏秒闪退**：`libGTASA.so` 是旧版脚本提取的，重跑 `prepare_gtasa.py` 并重新编译。

---

## 许可证与免责声明

本项目原创与修改代码以 **GNU General Public License v3.0** 开源，许可证全文见 [LICENSE](LICENSE)，第三方组件许可清单见 [third_party/licenses/THIRD_PARTY_NOTICES.md](third_party/licenses/THIRD_PARTY_NOTICES.md)。

本项目基于开源 Alyn SA-MP Mobile 架构持续开发，保留原文件归属声明。本项目与 Rockstar Games、Take-Two Interactive 及 SA-MP 官方团队无任何关联，不分发任何 Rockstar 专有二进制、游戏数据包或商业字体——详见 [NOTICE](NOTICE)。
