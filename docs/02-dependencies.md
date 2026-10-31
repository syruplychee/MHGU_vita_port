# 依赖、版本与安装

## 1. 必需组件

| 组件 | 版本要求 | 用途 | 获取方式 |
|---|---|---|---|
| Git | 任意较新版本 | 源码管理 | https://git-scm.com/ |
| CMake | **3.24+**（推荐 3.24–4.3.x） | 生成 Vita 构建 | https://cmake.org/download/ |
| Ninja | 1.10+（测试 1.13.2） | 构建执行器 | https://github.com/ninja-build/ninja/releases |
| Python | **3.10+**（测试 3.12.10） | 输入校验、头生成、离线工具 | https://www.python.org/downloads/ |
| Python `lz4` | 4.x（测试 4.4.5） | 解压锁定 NSO | `python -m pip install -r tools/requirements.txt` |
| VitaSDK | GCC 15.2.0 测试通过 | ARMv7 交叉编译、SELF/VPK 打包 | https://vitasdk.org/ 或 https://github.com/vitasdk/vdpm |
| 游戏输入 | MHGU 完整 1.4.0 | 构建 VPK 时打包四模块 | 见 [游戏输入](03-game-inputs.md) |

### 1.1 安装 VitaSDK

使用官方安装器或 vdpm 构建。安装后必须设置 `VITASDK` 指向 SDK 根目录：

```powershell
$env:VITASDK = "C:\tools\vitasdk"
Test-Path "$env:VITASDK\share\vita.toolchain.cmake"   # 必须为 True
```

```bash
export VITASDK=/opt/vitasdk
test -f "$VITASDK/share/vita.toolchain.cmake"
```

`arm-vita-eabi-gcc --version` 应显示 15.2.0 或兼容版本。项目使用：
`-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard -Wall -Wextra -Werror`。

### 1.2 Python 依赖

```powershell
python -m pip install -r tools/requirements.txt
python -c "import lz4.block; print(lz4.__version__)"
```

构建脚本会检查 `lz4`。如果系统 `python` 是 Microsoft Store 占位符，请显式设置：

```powershell
$env:MHGU_PYTHON = "C:\Python312\python.exe"
```

## 2. 可选组件

| 组件 | 用途 | 说明 |
|---|---|---|
| Vita3K | 在 PC 上运行 VPK | 必须使用 **OpenGL** 后端；不同版本对 VM 分配限制不同，见 [运行](06-running-vita3k.md) |
| kubridge 导入库 + 内核插件 | 真机大模块映射/权限 | 本项目不附带插件；见 [真机](07-hardware.md) |
| Shacc (`vita3k-shacc`) | 离线编译 Cg → GXP | GPL-2.0；版本与哈希锁定在 `config/graphics/shader-toolchain.json` |
| SceShaccCg (`libshacccg.suprx`) | Shacc 运行时依赖 | 本地合法获取，**不得再分发**；Sony OLP |
| shader-compiler-rs | Maxwell/SPH → GLSL 前端 | MPL-2.0/GPL-2.0 混合，见上游许可 |
| hactool / hactoolnet / nut | 解包自己的游戏与更新 | 只用于处理你合法拥有的副本 |

## 3. 环境变量

| 变量 | 必需 | 含义 |
|---|---|---|
| `VITASDK` | 是 | VitaSDK 根目录 |
| `MHGU_EXEFS` | 是 | 锁定 1.4.0 ExeFS 目录（含 `rtld/main/subsdk0/sdk/main.npdm`） |
| `MHGU_ROMFS` | 运行/资源 | 含 `nativeNX/` 的 RomFS 根目录 |
| `MHGU_NACP` | 输入校验 | `control.nacp` |
| `MHGU_PYTHON` | 建议 | 可用的 Python 3.10+ 解释器 |
| `MHGU_NINJA` | 可选 | Ninja 路径 |
| `MHGU_VITA3K` | 可选 | Vita3K 可执行文件路径 |
| `MHGU_VITA3K_SOURCE` | 可选 | Vita3K 安装目录，供私有运行时硬链接 |
| `MHGU_KUBRIDGE_LIB` | 可选 | kubridge 导入库路径 |
| `MHGU_STATIC_SHADER_DIR` | 可选 | 编译进 SELF 的静态包运行目录，默认 `ux0:data/mhgu-vita-port/static-shaders-current` |

## 4. 版本锁定

输入身份锁在 `config/target-1.4.0.json`，包含四个模块的 SHA256、Build ID、大小，
以及 NPDM/NACP 哈希。任何构建都会在 configure 阶段校验 SDK 契约头；运行时还会再次
核对模块身份。

Shader 工具链锁在 `config/graphics/shader-toolchain.json`：

- `shader-compiler-rs` commit `f9f0fbc998279a663d0bc98937a0bde880efc8db`
- Shacc 审阅 commit `cec711d4bcb1cb77279d186e5919f1b88e2fe1ab`
- Shacc Windows 归档 SHA256 `b6dc166a...`
- Shacc 可执行文件 SHA256 `8f13b52a...`
- SceShaccCg commit `f9511fc2...`，二进制哈希 `188adb8c...`（仅本地）

这些第三方二进制不进入 Git。需要重建着色器包时，按 [静态着色器包](reference/shader-package.md)
准备。

## 5. CMake 版本说明

项目最低要求 3.24。实测：

- CMake 4.3.1：Windows + VitaSDK 正常。
- CMake 4.4.3：在部分非系统卷上创建构建目录时可能报
  `Unable to (re)create the private pkgRedirects directory` 或临时文件权限错误。

遇到此类错误时，优先把构建目录放到系统盘（`-BuildDir` 参数）或改用 4.3.x。
这不是项目源码问题，也不影响 Linux CI。

## 6. 磁盘与内存

- 完整 RomFS 约 12.3 GB，35,000+ 文件；开发期用目录 junction，不要复制多份。
- 源码与构建产物分开：`build/`、`local/`、`game-input/` 均被 Git 忽略。
- 静态着色器包约数 MB；纹理/模型资源包按需生成，不常驻内存。
- 运行 Vita3K 时建议至少 16 GB 可用内存和足够的页面文件。
