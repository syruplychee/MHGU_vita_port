# MHGU Vita Port

将 **Monster Hunter Generations Ultimate 1.4.0** 的原始 AArch32 游戏逻辑运行在
PlayStation Vita 上的移植工程。

项目不重写游戏，也不使用解释器、JIT 或整程序静态重编译。PC 端离线完成身份校验、
补丁规划、着色器翻译和静态资源准备；Vita 端加载原始 NSO 模块，保留原生 ARM 执行，
并把文件、存档、输入、音频、视频和图形接口接到 Vita 平台实现。影片呈现另有一个
可选、默认关闭的框架级原生路径（[ADR 0014](docs/decisions/0014-native-movie-player.md)）；
它只替换全屏影片呈现，结束时调用游戏自己的 uMovie 停止命令，不改变游戏状态机。

> 本项目不附带、也不得提交任何游戏文件、密钥、固件、存档或游戏派生二进制。
> 使用者必须自行合法拥有对应游戏与更新。

## 当前状态（2026-11-06 快照）

| 能力 | 状态 |
|---|---|
| 原生加载器 / 四模块重定位 / TLS / SVC / SDK DSO | 在 Vita3K OpenGL 下可用 |
| 真实存档加载、写入、跨进程读回 | 已验证（受保护测试流程） |
| 中文界面与对话 | 已验证 |
| 村庄场景渲染 | 固定村长对话有效对照 7.18→12.34 FPS，帧时间下降 41.8%（Vita3K L3）；全玩法/真机未验证 |
| 任务/住宅场景 | 有修复，仍待完整人工复测 |
| 视频播放（原 guest 管线） | 可解码；约 8 内容事件/秒（Vita3K，单次运行） |
| 视频播放（原生播片，`native-movie.enable`） | 完整片头 29.974 fps（5278 帧/176.085 秒，L3）；视频-only、无音频；真机未验证 |
| 音频 | 服务与混音可用；诊断运行使用 SDL 音频 |
| PS Vita 真机 | **未验证**；映射、缓存、内存、性能均待真机 |

当前推荐组合为 deep 64 MiB、正常日志减负、Alpha8 原生点阵字体；128 MiB 和 idle
回收在新工作集下没有稳定收益，关闭。完整配置、无效基线排除、逐窗口数据与备份见
[性能整理](docs/21-performance-consolidation.md)。

完整范围与证据边界见 [当前状态](docs/09-status.md)。历史研究记录保留在旧工作区，
不作为本仓库的验收依据。

## 5 分钟快速开始

前置条件：VitaSDK、CMake 3.24+、Ninja、Python 3.10+（含 `lz4`）、以及你自己解包的
1.4.0 游戏输入。详细安装见 [依赖](docs/02-dependencies.md) 与
[游戏输入](docs/03-game-inputs.md)。

```powershell
# 1. 环境变量（示例，按你的实际路径修改）
$env:VITASDK = "C:\tools\vitasdk"
$env:MHGU_EXEFS = "D:\MHGU_vita_port\game-input\exefs"
$env:MHGU_ROMFS = "D:\MHGU_vita_port\game-input\romfs"
$env:MHGU_NACP = "D:\MHGU_vita_port\game-input\control.nacp"
$env:MHGU_PYTHON = "C:\Python312\python.exe"

# 2. 校验输入身份（必须匹配 config/target-1.4.0.json）
.\scripts\check-inputs.ps1

# 3. 构建 VPK
.\scripts\build.ps1

# 4. 准备 Vita3K 运行时数据（RomFS 索引、离线系统数据、分辨率配置）
.\scripts\prepare-runtime.ps1 -ShaderPackage <静态着色器包目录>

# 5. 启动（自动备份存档）
.\scripts\play.ps1
```

已有配套 `font-native/` 包时，可以先运行
`scripts/set-performance-profile.ps1 -Profile recommended` 应用受测组合；缺少字体包时
脚本会明确拒绝，准备方法见[原生字模](docs/20-native-pixel-fonts.md)。普通 `play.ps1`
默认开启 batch，有类型的自定义参数用 `-RunnerOptions @{ ... }` 传入。

Linux/macOS 使用 `scripts/build.sh`，环境变量同名。宿主测试使用
`scripts/host-tests.ps1`。

## 目录结构

```text
CMakeLists.txt            生产加载器构建入口
CMakePresets.json         Vita release/debug 预设
cmake/                    构建选项、输入头生成与校验
src/app/                  加载器主入口
src/loader/               NSO 解码、重定位、补丁、SDK DSO、启动生命周期
src/bridge/               guest/host ABI 桥、ARMv7 汇编岛
src/platform/             Vita 平台服务、GXM 渲染、音频、视频、存档
src/generated/            从锁定输入生成的 SDK 契约头（受审查）
tests/                    宿主侧纯 C 回归测试
tools/input/              输入校验与构建期头生成
tools/romfs/              RomFS 索引/离线系统数据生成
tools/shader/             离线着色器翻译、打包与校验
tools/assets/             纹理格式转换与资源清单
tools/runtime/            Vita3K 启动、窗口取证、存档辅助
scripts/                  面向开发者的构建/运行脚本
config/                   输入锁、补丁计划、图形契约、运行配置
docs/                     项目共识文档（从零编写，不是历史流水）
```

## 关键约束

- 只接受 `config/target-1.4.0.json` 锁定的 1.4.0 输入；身份不符必须拒绝。
- 保留原 AArch32 游戏逻辑；补丁绑定模块 build ID、源 SHA256、偏移和原字节。
- 未实现的平台路径必须明确报错，不能用“成功”空实现掩盖。
- 游戏内容、`prod.keys`/`title.keys`、固件、存档、VPK/SELF/GXP 等生成物不得进入 Git。
- 静态审计、编译、Vita3K、真机、用户玩法五种证据不能互相冒充。

## 文档

- [文档索引](docs/README.md)
- [依赖与安装](docs/02-dependencies.md)
- [游戏文件、密钥与目录布局](docs/03-game-inputs.md)
- [构建 VPK](docs/04-building.md)
- [运行时资源](docs/05-runtime-assets.md)
- [在 Vita3K 中运行](docs/06-running-vita3k.md)
- [架构](docs/01-architecture.md)
- [测试与证据规则](docs/08-testing.md)
- [贡献指南](docs/10-contributing.md)

## 许可与声明

本项目代码由贡献者自行编写。VitaSDK、Vita3K、vitaGL、Shacc、SceShaccCg、kubridge
等第三方组件遵循各自许可证；详见 [上游与许可](docs/reference/upstreams.md)。
本项目与 Capcom、Nintendo、Sony 无关联。
