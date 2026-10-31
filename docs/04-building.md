# 构建 VPK

## 1. 前置检查

- `VITASDK` 已设置，且 `$VITASDK/share/vita.toolchain.cmake` 存在。
- `MHGU_EXEFS` 指向锁定 1.4.0 ExeFS，包含 `rtld/main/subsdk0/sdk/main.npdm`。
- Python 3.10+ 可用，且 `import lz4.block` 成功。
- Ninja 与 CMake 3.24+ 可用。

先跑一次输入身份检查（见 [游戏输入](03-game-inputs.md)）。

## 2. 一键构建

Windows：

```powershell
$env:VITASDK = "C:\tools\vitasdk"
$env:MHGU_EXEFS = "D:\MHGU_vita_port\game-input\exefs"
$env:MHGU_PYTHON = "C:\Python312\python.exe"

.\scripts\build.ps1
```

Linux/macOS：

```bash
export VITASDK=/opt/vitasdk
export MHGU_EXEFS=/data/mhgu/game-input/exefs
export MHGU_PYTHON=python3

./scripts/build.sh
```

输出：

```text
build/vita/vita_port_native_loader.vpk
build/vita/vita_port_native_loader.self
build/vita/static-shader-package.json
build/vita/generated/target_identity.h
build/vita/generated/target_tls.h
build/vita/generated/target_startup.h
```

脚本会打印 VPK 与 SELF 的 SHA256。构建目录、临时目录和所有生成物都被 Git 忽略。

### 2.1 常用参数

`scripts/build.ps1`：

| 参数 | 默认 | 说明 |
|---|---|---|
| `-ExeFs` | `$env:MHGU_EXEFS` | 游戏 ExeFS |
| `-KubridgeLib` | `$env:MHGU_KUBRIDGE_LIB` | 可选 kubridge 导入库 |
| `-BuildDir` | `build/vita` | 构建目录 |
| `-StaticShaderDir` | `ux0:data/mhgu-vita-port/static-shaders-current` | 编译进 SELF 的静态包目录 |
| `-Python` | `$env:MHGU_PYTHON` / PATH | Python 解释器 |
| `-CMake`、`-Ninja` | PATH / 工作区工具 | 工具路径 |
| `-NoBootstrap` | 关闭 | 不构建 bootstrap VPK |
| `-Clean` | 关闭 | 清掉 CMake 缓存后重配 |

## 3. 手动 CMake

```powershell
$env:VITASDK = "C:\tools\vitasdk"
cmake -S . -B build/vita -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:VITASDK/share/vita.toolchain.cmake" `
  -DPython3_EXECUTABLE="C:\Python312\python.exe" `
  -DVITA_PORT_EXEFS="D:\MHGU_vita_port\game-input\exefs" `
  -DVITA_PORT_BUILD_NATIVE_LOADER=ON `
  -DVITA_PORT_NATIVE_SDK_DSO=ON
cmake --build build/vita --target vita_port_native_loader.vpk
```

或使用预设：

```powershell
$env:VITASDK = "C:\tools\vitasdk"
$env:MHGU_EXEFS = "D:\MHGU_vita_port\game-input\exefs"
cmake --preset vita-release
cmake --build --preset vita-release --target vita_port_native_loader.vpk
```

预设要求 Ninja 在 PATH 中；否则请使用脚本或手动传入 `CMAKE_MAKE_PROGRAM`。

## 4. 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `VITA_PORT_BUILD_NATIVE_LOADER` | `ON` | 构建游戏加载器 VPK；需要 `VITA_PORT_EXEFS` |
| `VITA_PORT_BUILD_BOOTSTRAP` | `ON` | 构建无游戏的 bootstrap VPK，用于验证 VitaSDK |
| `VITA_PORT_NATIVE_SDK_DSO` | `ON` | 执行原 SDK DSO；生产路径必须开启 |
| `VITA_PORT_SAVE_SELFTEST` | `OFF` | 服务初始化时运行存档后端自测（诊断） |
| `VITA_PORT_EXEFS` | 空 | 锁定 1.4.0 ExeFS 目录 |
| `VITA_PORT_NATIVE_TITLE_ID` | `MHGUVP018` | 加载器 VPK 的 Title ID |
| `VITA_PORT_STATIC_SHADER_PACKAGE_DIR` | `ux0:data/mhgu-vita-port/static-shaders-current` | 编译进 SELF 的静态包目录 |
| `VITA_PORT_KUBRIDGE_LIBRARY` | 空 | 可选 kubridge 导入库；真机映射需要 |

> `VITA_PORT_NATIVE_SDK_DSO=OFF` 只用于诊断。它不包含完整的平台服务路径，不能作为
> 可玩游戏包发布。

## 5. 构建期校验

Configure 阶段会：

1. 从 `config/target-1.4.0.json` 和补丁计划生成
   `target_identity.h`、`target_tls.h`、`target_startup.h` 到构建目录。
2. 用 `prepare_sdk_dso.py --verify` 校验 SDK DSO 契约头。
3. 用 `prepare_sdk_svc.py`、`prepare_sdk_timer.py`、`prepare_sdk_barrier.py`
   对锁定 ExeFS 重新扫描并校验 `src/generated/sdk_*_contract.h`。
4. 检查 ExeFS 五件套齐全。

任何一项失败都会在编译前停止。不要手改生成头来绕过；应重新确认输入身份或重新生成：

```powershell
python tools/input/prepare_native_loader.py --output build/generated/target_identity.h
python tools/input/prepare_sdk_svc.py --exefs game-input/exefs
python tools/input/prepare_sdk_timer.py --exefs game-input/exefs
python tools/input/prepare_sdk_barrier.py --exefs game-input/exefs
```

后三个非 `--verify` 模式会同时重写 `config/patch-plans/*`，只有确认输入锁变化时才这样做，
并把计划与头一起提交审查。

## 6. VPK 内容

`vita_port_native_loader.vpk` 包含：

- `eboot.bin`：加载器 SELF
- `game/rtld`、`game/main`、`game/subsdk0`、`game/sdk`：锁定游戏模块
- 启动画面与 SFO

因此：

- VPK 是**游戏派生二进制**，不能公开分发，也不能提交 Git。
- VPK 体积约 16.5 MB，取决于模块大小。
- 只把 `game/` 四模块放进 VPK；RomFS 不进入 VPK，运行时从 `ux0:data` 读取。

## 7. 构建产物校验

```powershell
Get-FileHash build/vita/vita_port_native_loader.vpk -Algorithm SHA256
Get-FileHash build/vita/vita_port_native_loader.self -Algorithm SHA256
```

在 PR 或报告中记录：源码提交、VPK/SELF SHA256、VitaSDK 版本、CMake/Ninja/Python 版本、
`config/target-1.4.0.json` 哈希。不要只写“构建通过”。

## 8. 常见构建错误

| 错误 | 原因 | 处理 |
|---|---|---|
| `Set VITASDK or pass...` | 未设置 `VITASDK` | 设置环境变量 |
| `VITA_PORT_EXEFS is missing...` | ExeFS 路径错误或文件不全 | 检查五件套 |
| `No module named lz4` | Python 缺包 | `pip install -r tools/requirements.txt` |
| `stale SDK DSO header` | 生成头与补丁计划不一致 | 按第 5 节重新生成并审查 |
| `Cannot create temporary file` | 系统临时目录不可写 | 脚本已把 `TMP/TEMP` 指到 `build/vita/tmp`；手动构建请自行设置 |
| `pkgRedirects` / `Permission denied` | CMake 4.4.x 在部分卷上的问题 | 改用 CMake 4.3.x，或用 `-BuildDir` 指到系统盘 |
| `undefined reference to vp_bridge_enter` | 未启用 ASM 语言 | 使用仓库 CMake；不要删 `enable_language(ASM)` |
| `ninja: error: WriteFile(.ninja_lock)` | 构建目录被安全策略禁止写入 | 换构建目录到可写卷，见 [故障排查](11-troubleshooting.md) |
