# 架构与执行模型

## 1. 目标

在 PS Vita（Cortex-A9，ARMv7，无 AArch64）上运行 MHGU 的原始 AArch32 游戏代码。
游戏逻辑、AI、任务、战斗、动画和存档语义保持原实现；项目只替换运行环境：

- 原始 NSO 模块由 Vita 端加载器解码、重定位并执行。
- Switch SDK 的 SVC、TLS、线程、时间、文件、存档、输入、音频和视频接口由平台层接管。
  全屏 MP4 另有一条可选的框架级原生呈现路径（ADR 0014）；它只替换影片呈现，不改变
  游戏状态机，并在结束时调用游戏自己的停止命令。
- NVN 图形命令流由 GXM 后端消费；离线阶段把 Maxwell 着色器翻译成 GXP。
  另有显式启用的游戏公共入口补丁，跨绘制复用原 SDK 已准备的顶点布局，在同一命令
  消费顺序恢复状态；网格、动态地址、骨骼、纹理和 draw 保留原流程。该切片两轮对照
  尚未证明整帧提速，默认关闭，见 ADR 0018。

不采用：游戏重写、解释器、JIT、动态二进制翻译、整程序静态重编译、通用 Maxwell
命令模拟器。

## 2. 启动链

```text
Vita 启动 VPK (MHGUVP018)
  └─ src/app/main.c
       ├─ 从 app0:game/ 读取 rtld、main、subsdk0、sdk
       ├─ vp_nso_inspect / vp_nso_decode        解压、校验 SHA256/Build ID、映射段
       ├─ vp_native_plan / vp_native_commit     完整重定位与导入槽绑定
       ├─ patch_tls / patch_rtld                绑定 TLS 岛、rtld 补丁
       ├─ vp_unwind_register                    注册 EHABI 展开信息
       ├─ patch_dso_boundary + vp_sdk_dso_*     执行原 SDK DSO 初始化
       ├─ vp_rtld_connect                       建立 rtld 兼容对象与连接
       ├─ 缓存维护 (VMDomain)                   写入代码后同步 I/D cache
       └─ SDK 启动链 → nnMain → 游戏主循环
```

关键约束：

- 每个补丁都绑定模块 build ID、源 SHA256、模块相对偏移、ARM/Thumb 状态和预期原字节。
  原字节不匹配立即拒绝。
- 模块内部相对布局保持不变；按重定位表整体搬到 Vita 可用地址。
- 代码写入后必须做 cache 一致性维护；真机映射与权限仍待验证。
- 软件 guest 页表不会自动修正原生 load/store 地址，所有地址必须来自真实映射。

## 3. 源码模块

| 目录 | 职责 | 关键文件 |
|---|---|---|
| `src/app/` | VPK 主入口、启动顺序、结果日志 | `main.c` |
| `src/loader/` | NSO 解码、重定位、补丁、SDK DSO、启动生命周期 | `nso_loader.c`、`native_link.c`、`patch_loader.c`、`sdk_dso.c` |
| `src/bridge/` | guest/host ABI 桥、guest TLS、ARMv7 汇编岛 | `host_bridge.c`、`guest_context.c`、`armv7/*.S` |
| `src/platform/` | Vita 平台服务与 GXM 渲染 | `guest_svc.c`、`guest_storage.c`、`guest_save.c`、`gxm_runtime.c` |
| `src/generated/` | 从锁定输入生成的 SDK 契约头 | `sdk_*_contract.h` |
| `tools/` | PC 端离线工具 | 见下节 |
| `tests/` | 宿主侧纯 C 回归 | 见 [测试](08-testing.md) |

### 3.1 `guest_svc.c`

Switch SDK 服务的主要转接层。它把 guest 的 IPC/服务调用映射为：

- 文件与 RomFS 读取（`guest_storage.c`）
- 存档后端（`guest_save.c`）
- 线程、锁、时间、事件、TLS
- 输入（`SceCtrl`）
- 音频命令与视频解码（`audio_*`、`media_*`）
- GXM 图形请求（`gxm_runtime.c`、`gpu_stream.c`、`gpu_texture.c`）

未实现的路径必须返回明确错误并记录日志；禁止用“成功”空实现掩盖缺口。

### 3.2 GXM 后端

`gxm_runtime.c` 消费 Maxwell/MME 命令流，负责：

- 静态着色器包 `VPSHIX1` / 资源 sidecar `VPSRES1` 的加载与查找
- 管线、纹理、uniform、渲染目标、深度/模板状态
- 有界 Scene 批处理、clear 融合、完成通知
- 屏幕缩放（默认 540p）与 present
- 纹理 GOB 解平铺、压缩块转换、上传缓存与快照

设计细节见 [ADR 0004](decisions/0004-gxm-batching.md) 和
[静态着色器包](reference/shader-package.md)。

### 3.3 平台数据路径

Vita 端统一使用 `ux0:data/mhgu-vita-port/`：

| 路径 | 内容 |
|---|---|
| `romfs.index` / `romfs.map` | 原 RomFS 元数据与文件映射 |
| `romfs-tree/` | 解包后的完整 RomFS 树（开发期可用 junction） |
| `offline-ngword2.romfs` | 自生成的空词库系统数据 |
| `static-shaders/` | 静态着色器源包（冻结，不修改） |
| `static-shaders-current/` | 本次构建编译定义选中的运行包 |
| `render-resolution.txt` | `360p` 或 `540p` |
| `savedata/` | 游戏存档 |
| `gxm-runtime.log` / `sdk-svc.log` | 运行日志 |
| `*.enable` marker | 显式诊断/运行开关，见 [marker](reference/markers.md) |

## 4. 离线/运行时边界

| 阶段 | 做什么 | 不做什么 |
|---|---|---|
| PC 离线 | 输入身份校验、NSO 分析、补丁规划、Maxwell→Cg→GXP、静态索引、资源 sidecar | 不执行游戏、不产生“像素通过”结论 |
| Vita 运行时 | 解码、重定位、GXM 对象创建、动态 uniform/骨骼、纹理按需上传、draw/同步 | 不运行 Cg/Maxwell 编译器，不做全量资源常驻 |
| 验证 | 宿主测试、VPK 构建、Vita3K、真机、用户玩法分别记录 | 不把低层级证据冒充完整玩法 |

## 5. 内存与生命周期

- 四个模块使用 Vita VM/普通内存块；真机大模块的 RX/R/RW/NX 映射仍需 kubridge 或等价方案。
- GPU 资源有明确 owner、pin 和 fence 生命周期；Scene 完成前不得复用 staging。
- 纹理缓存、上传缓存和快照都有上限；禁止无界常驻。
- 存档写入使用真实文件语义与提交/恢复流程，测试必须保护用户原始存档。

## 6. 当前架构缺口

- 真机页权限、cache 维护和异常展开未验证。
- 任务/住宅场景仍有未闭环的图形路径。
- 原 guest 视频播放节奏受播放器提交间隔限制；可选的框架级原生播片已在完整片头
  达到 29.974 fps（L3，视频-only、无音频、未上真机）。
- 村庄/任务 540p 下性能距 30 FPS 目标仍远；影片路径见 ADR 0014。
- 音频在真机上的连续性与时钟未验证。

这些缺口在 [当前状态](09-status.md) 中跟踪，不应被构建成功或模拟器单次运行掩盖。
