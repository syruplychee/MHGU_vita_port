# 真机与 kubridge

> 当前所有设备侧结论都来自 Vita3K。PS Vita 真机**未验证**，本文件的清单不算通过。

## 1. 为什么需要额外方案

Vita 的公开 SDK 内存块接口对大模块、页权限和代码 cache 维护有限制。游戏 `main`
映像约 50 MB，且需要 RX/R/RW/NX 分段。项目提供可选的 kubridge 路径：

- 构建时传入 `-DVITA_PORT_KUBRIDGE_LIBRARY=<libkubridge.a>`；
- 运行时需要安装匹配的 kubridge 内核插件；
- 插件不随项目分发，也不提交源码/二进制。

没有 kubridge 时，加载器仍可在模拟器中运行；真机大模块映射未证明。

## 2. 构建硬件变体

```powershell
.\scripts\build.ps1 `
  -ExeFs "D:\MHGU_vita_port\game-input\exefs" `
  -KubridgeLib "D:\MHGU_vita_port\local\kubridge\libkubridge.a" `
  -BuildDir "build\vita-hw" `
  -StaticShaderDir "ux0:data/mhgu-vita-port/static-shaders-current-hw"
```

构建会定义 `VP_HAVE_KUBRIDGE=1`，并在启动链末尾调用页权限设置。日志中会出现：

```text
HARDWARE_BACKEND kubridge_protect=<rc> measured_on_hardware=0
```

`measured_on_hardware=0` 是编译时常量；只有真机运行证据才能改为已验证。

## 3. 真机待验清单

| 编号 | 断言 | 验证方法 | 通过判据 |
|---|---|---|---|
| HW-01 | 四模块按 RX/R/RW/NX 映射，岛区可达 | 真机运行加载器，读 `VP_PERM_*` / `VP_ISLAND_*` | 四段权限生效，岛区写入后可执行，无内核错误 |
| HW-02 | 代码写入后 I/D cache 一致 | 写代码→清 cache→跳转，与不清 cache 对照 | 维护组稳定执行；对照组可复现失败 |
| HW-03 | 关键场景内存装得下且有余量 | 标题→村庄→任务→战斗，采样 `sceKernelTotalFreeMemSize` + 自建账本 | 峰值低于预算并留 ≥10% |
| HW-04 | 真实 GXM 像素/格式/时序与模拟器一致 | 重放已有关卡 draw 并回读 | 与模拟器差 ≤1 LSB；纹理族逐项过 |
| HW-05 | 代表性离线场景稳定 30 FPS | 固定场景/构建，记录 P50/P95/P99 与 CPU/GPU | 达到性能目标 |
| HW-06 | 音频连续、无欠载、音画同步 | 接 AudioOut，记录欠载与最大执行时间 | 目标场景无欠载，播放位置同步 |
| HW-07 | 线程/锁无丢失唤醒/优先级反转 | 双线程互调、锁交接、超时/取消 | 无重复/丢失唤醒 |
| HW-08 | 休眠/唤醒/前后台/强杀不破坏存档 | 真机执行四类生命周期操作 | 存档可读、无半写；恢复后正常 |
| HW-09 | nvdrv/显示提交语义一致 | 采集 present 与 fence | 呈现/fence 一致，无丢帧累积 |
| HW-10 | 存档后端语义与 `sceIo*` 一致 | 真机跑存档 probe + 只读/满盘/拔卡 | 错误码与参考一致 |

## 4. 推荐顺序

1. HW-01 / HW-02：先证明能装、能执行。
2. HW-09：确认显示/提交模型。
3. HW-03：内存账。
4. HW-04：图形正确性。
5. HW-07 / HW-06 / HW-08：线程、音频、稳定性。
6. HW-05：最后做性能，使用同一成品构建。

## 5. 环境卡

每次真机证据必须记录：

- 机型（PS Vita 1000/2000、PS TV）
- 固件版本
- HENkaku/插件版本，kubridge commit
- 存储介质与剩余空间
- 构建的 VPK/SELF SHA256、静态包 SHA256
- 启动参数、场景、运行时长、退出方式
- 原始日志与截图路径（本地，不入 Git）

## 6. 真机注意

- 12 GB RomFS 必须复制到记忆棒；确保空间和读取速度。
- `ux0:data/mhgu-vita-port/` 下的 marker 与 Vita3K 相同。
- 真机没有 junction，`romfs-tree/` 必须是真实目录。
- 音频、视频和显示路径在真机上与 Vita3K 可能不同；以真机为准。
- 任何“真机通过”必须来自真机日志/截图/存档核验，不能由模拟器推断。
