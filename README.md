# RandomMapGen

`RandomMapGen` 是《红色警戒 2：尤里的复仇》随机地图生成器，用于研究、复现和离线批量生成 RA2/YR 随机地图。

本仓库提供独立实现的原生生成器与地图验证/诊断 CLI：

- `native`：直接接收底层 `MapSeedOptions`，包括 32 位地图种子、尺寸、玩家数、`MapType`、资源、高度、植被、区域等参数。
- `game-native`：接收原版随机地图对话框的可见参数，先按原版 RNG 顺序派生隐藏参数和 16 位地图种子，再执行同一套原生生成器。

仓库不包含 CompetitiveMapGen Python 生成器、游戏原版二进制、IDA 数据库或游戏美术资源；`verify-competitive-yrini` 只负责校验外部生成器输出的地图。

## 特性

- 保持原生生成器的单一 RNG 顺序、菱形坐标、堆排序、浮点落地和 21 个阶段边界。
- 支持 `MapType=0..4`、2 至 8 人、全部 Theater 和 RA2/YR 原生参数范围。
- 输出调试文本、预览 `yrini` 或接近原版保存格式的 `yrini-product`。
- 支持用户自备的 RA2/YR 游戏资源目录，用于读取 Theater INI、MIX 和 TMP tile 元数据。
- 无游戏资源时可用内置 `yr` TileCatalog 生成预览地图。
- `--error-report-out` 为失败生成写出 schema 1 稳定错误码 sidecar，MapTask 不需要解析 stderr。
- Windows x64 与 Linux x86_64 统一使用 CMake 构建。
- 提供 `PlatformRandomMapGen::Native` 静态库和 `PlatformRandomMapGen::CLI` 命令行目标。

## CMake 构建

要求：

- CMake 3.24 或更高版本
- 支持 C++20 的编译器
- nlohmann_json 3.12.0（由 FetchContent 获取）
- PlatformDevKit 统一锁定的 OpenSSL 3.5.5 Crypto
- PlatformDevKit 源码检出，默认位于 `../PlatformDevKit`，也可设置 `PLATFORM_DEVKIT_SOURCE_DIR`
- Windows x64：MSVC 开发者终端、Ninja、Perl、NASM 与 NMake；Linux 预设需要 Ninja

OpenSSL 默认由 DevKit 在当前构建目录自动源码构建；Windows 可用 `OPENSSL_ROOT_DIR` 显式指定精确版本的开发包。依赖的 binary 目录属于当前 build tree，不跨仓共享 `FETCHCONTENT_BASE_DIR`。

Windows：

在已初始化 x64 MSVC 的开发者终端执行，预设显式选择 `cl`；Ninja 预设不接受 `-A x64`，也不自动切换架构。

```powershell
cmake --preset windows-msvc-x64
cmake --build --preset windows-msvc-x64
# 自动 shared OpenSSL 的运行库目录；显式开发包则使用其对应 bin 目录。
$env:PATH = "$PWD/build/cmake/windows-msvc-x64/_platform_deps/openssl-3.5.5/install/bin;$env:PATH"
./build/cmake/windows-msvc-x64/rmg.exe --version
```

Linux：

```bash
cmake --preset linux-release
cmake --build --preset linux-release
./build/cmake/linux-release/rmg --version
```

集成到同级 workspace 的其他 CMake 项目时，可以通过 FetchContent 的
`SOURCE_DIR` 指向本仓库；本项目提供 `PlatformRandomMapGen::Native` 静态库和
`PlatformRandomMapGen::CLI` 命令行目标。self-test 默认不加入构建，设置
`-DPLATFORMRANDOMMAPGEN_BUILD_TESTS=ON` 后会生成 `rmg_tests`。

## Release

旧 GitHub Actions 发布工作流已移除，推送 `v*` tag 不会自动生成安装包或 Release。当前使用下述 CMake/CTest 入口本地构建和验证；发布自动化另行维护。

发布内容应包含二进制、所需运行库、README、AGPL 许可证、NOTICE、第三方声明、示例和文档，并为归档提供 SHA-256。Windows 使用共享 OpenSSL 时必须部署相应 `libcrypto-3-x64.dll`，不能只复制 `rmg.exe`。

## 快速开始

直接指定底层地图种子：

以下示例使用 Windows CMake 预设的产物；Linux 将可执行路径替换为 `./build/cmake/linux-release/rmg`。

```powershell
./build/cmake/windows-msvc-x64/rmg.exe --mode native --seed 123 --width 80 --height 80 --players 2 `
  --format yrini --out out/native-123.yrini
```

模拟游戏对话框：

```powershell
./build/cmake/windows-msvc-x64/rmg.exe --mode game-native --randomization-seed 305419896 `
  --game-terrain-type 2 --time-of-day 3 --climate 1 `
  --map-size 2 --resources 2 --players 4 `
  --format yrini --out out/game-native.yrini
```

同时生成预览地图和产品地图：

```powershell
./build/cmake/windows-msvc-x64/rmg.exe --mode native --seed 123 --width 80 --height 80 --players 2 `
  --format yrini --out out/native-preview.yrini `
  --product-out out/native-product.map
```

使用本机游戏资源：

```powershell
./build/cmake/windows-msvc-x64/rmg.exe --mode native --seed 123 --width 80 --height 80 --players 2 `
  --game-resource-dir "C:\path\to\Yuris Revenge" `
  --format yrini-product --out out/native-product.map
```

游戏资源目录不随本仓库分发。工具只读取用户提供目录中的文件，不修改它们。

地图评估为只读报告，契约见 [`docs/map-evaluation.md`](docs/map-evaluation.md)。生成失败可通过 `--error-report-out` 输出 schema 1 错误报告，稳定错误码为 `RMG_STARTS_INSUFFICIENT` 或 `RMG_GENERATION_FAILED`。

## 验证与诊断

`rmg --help` 列出完整 CLI。发布/readiness 入口包括 `verify-native-game-options`、`verify-native-options-report`、`verify-yrini-product` 和 `verify-competitive-yrini`。地图检查还包括 `verify-yrini`、`verify-competitive-yrini`、`verify-native-options-report`、`signature-yrini`、`inspect-yrini`、`compare-yrini`、`verify-map-writer`、`verify-map-evaluation`、`verify-codecs`、`native-stage-report` 和 `compare-stage-report`。

`verify-competitive-yrini` 只验证外部 CompetitiveMapGen 产生的预览或产品地图；Python 算法、运行环境和测试仍由独立项目维护。

## CLI

运行 `rmg --help` 查看完整参数。原生参数包括：

```text
--seed --width --height --players --theater --map-type
--resources --time --water-amount --tiberium
--tiberium-layout --vegetation --ruggedness
--urban-presence --accessibility --region-size --terrain-type
--sed --rmg-config
```

`game-native` 参数包括：

```text
--randomization-seed --game-terrain-type --time-of-day
--climate --map-size --resources --players
```

`--sed` 使用原生 `[RandomMap]` 字段。直接传入的 `--width` 和 `--height`
表示可玩尺寸；通过 SED 加载的尺寸保留原版尺寸插值语义。

## 库接口

公共入口位于 `rmg/native_api.h`：

```cpp
#include "rmg/native_api.h"

rmg::MapSeedOptions options;
options.seed = 123;
options.numPlayers = 2;
options.width = 80;
options.height = 80;

const rmg::NativeGenerationResult result =
    rmg::generateNativeMap(options);
```

需要真实 Theater TMP 元数据时，先加载 `NativeTheaterCatalog`，再将指针传给
`generateNativeMap()`。

## 确定性

相同代码、编译器和优化配置下，相同输入应产生相同输出。以下内容属于输入契约：

- 代码版本和构建平台
- 地图种子与全部生成参数
- RMG 配置
- 游戏资源目录内容

原生核心通过展开源码哈希和固定输入回归防止无意变化。完整原版 `.map`
逐字节一致性需要额外的原版 Oracle 样本验证；仓库不附带原版游戏文件。

当前产品地图对照结果见
[`docs/consistency-report.md`](docs/consistency-report.md)。

## 验证

```powershell
pwsh -NoProfile -File ./scripts/verify-native-only-tree.ps1
cmake --preset windows-msvc-x64-tests
cmake --build --preset windows-msvc-x64-tests
$env:PATH = "$PWD/build/cmake/windows-msvc-x64-tests/_platform_deps/openssl-3.5.5/install/bin;$env:PATH"
ctest --preset windows-msvc-x64-tests
```

Linux 使用 `cmake --preset linux-tests`、`cmake --build --preset linux-tests`、`ctest --preset linux-tests`。测试预设默认串行并要求至少一个测试。配置时找到 `pwsh` 会把 `tests/test_native_cli.ps1` 注册为 `rmg_native_cli`，否则只运行原生 self-test；也可显式调用脚本并通过 `-RmgPath` 传入对应产物。游戏资源和原版 Oracle 对照仍需额外输入，不由这些测试自动覆盖。

## 许可证

本项目使用 `AGPL-3.0-or-later`。第三方依赖和声明见
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。

本项目与 Electronic Arts、Westwood Studios 或《命令与征服》品牌无隶属或授权关系。
