# 0025：GPU 通知等待按触发原因计时

- 日期：2026-11-04
- 状态：已定位主要等待；空闲页回收实验默认关闭，实测结果见下文
- 范围：原生 GXM 后端；不改变提交顺序、完成条件、资源所有权或 guest fence

## 问题

ADR 0024 的最终 deep 路径中，通知阻塞约 21.619 ms/帧，旧路径约 10.892 ms/帧。
`sceGxmFinish` 约 0.06 ms/帧，单纯删除 Finish 没有足够收益依据。通知调用的墙钟耗时
可能包含模拟器线程调度和后端处理，不能叫作 GPU 硬件执行时间。

此前 `VP_GXM_PIPELINE_DONE` 只抽样输出，`flush` / `flush-sync` 还隐藏了触发 flush
的原始原因。不能据这些抽样精确计算每类等待占比。

## 决定与计数契约

`gpu_wait_profile.h` 保存固定原因（本轮 27 类；ADR 0026 增加
`clear-materialize` 后共 28 类），每类累计 calls、real、polls、failures、
block_us、pending_sum、draws、sync 和五桶等待直方图。
桶边界为 `[0,100)`、`[100,1000)`、`[1000,5000)`、`[5000,20000)` 和 ≥20000 µs。

- 同步 `frame_end_scene_timed` 与 deferred `gxm_pending_scene_wait` 全部计入；
  同步呈现 blit 单列 `sync-blit`，其他非 batch 同步场景列 `sync-scene`。
- batch drain 用原始 reason：submit-end、clear、resource、present 等；
  纹理池回收调用内的等待覆盖为 `texture-pool`。arena-reuse 单列。
- block_us 只记录真实 NotificationWait 的前后时间，poll 命中记 0。
  失败的实际阻塞仍计入 failures 与总时间，原错误和 poison 行为保持。
- 使用已有 play-perf 时间戳，不新增逐 draw/wait syscall 或逐次日志；正常无探针时不记数。
- 每 32 个 present 在原有帧统计位置输出完整 `VP_GPU_WAIT` 多行快照与
  `VP_GPU_WAIT_TOTAL`。每行含 snap、t_us、frames、row、rows，均低于日志 sink 上限。
  TOTAL 的 real/block_us 直接来自原有全局计时，可与每类累计和精确对账。
- pending_sum 是等待前尚未退休的场景数累计，draws 只累计被直接等待的 deferred
  场景 draw 数；不是所有被顺带完成的 draw 数，不能用于计算完整 GPU 吞吐。

分析工具：

```powershell
python tools/diagnostics/analyze_gpu_wait.py --log <run>/gxm-runtime.log `
  --start-seconds 250 --end-seconds 370 --output <run>/gpu-wait-report.json
```

工具只选择请求窗口内的完整快照，报告实际端点、帧数和原始日志 SHA256。所有快照必须
行齐全、顺序完整、计数不倒退、calls=real+polls、桶和=real、各类累计和=TOTAL。
残缺、重复、缺段或窗口不足明确失败，不补零。最大耗时/分位数不从累计最大值相减；
本轮只报告粗桶区间分布。

## 源码候选与边界（L0 工程推断）

1. `gxm_batch_flush_worker` EndScene 后立刻等 next arena，发生在下一 draw 的纹理解析、
   解码和状态准备之前。可以研究把等待推迟到真正改写 arena 前，给 CPU 与 GPU 更多重叠。
   但 `gxm_batch_reset_scene_state` 会改写该 arena 的 snapshot 表，不能仅删掉旧等待。
   必须审查 entry、staging、snapshot 和指针绑定的首次写入，保证使用前真实完成。
2. `gxm_texture_pipeline_reclaim_worker` 以 RESOURCE flush 并 drain 所有较早场景。
   若压力占比高，应先尝试回收通知已完成场景、缩短旧 lease 持有，或等最早能释放所需
   资源的场景。不能把真正仍在 GPU 使用的页提前释放，也不能靠增大缓存或少画作结论。
3. `gxm_pending_scene_complete_arena` 等一个 arena 后只退休本身；同 context 通知顺序
   可以证明较早场景也完成。进一步退休较早 pending 可能及时释放 texture lease，
   但需要统计证明是否有这类较早未退休场景，不能把逻辑可行直接写成提速。
4. submit-end 是 guest fence 的完成边界；当前宿主同步提交返回后 guest 发布 syncpoint。
   不能通过直接取消该等待或提前发布完成值来减少计时。

## 空闲页回收实验（默认关闭）

新增 A/B mode 5 `deep-recycle`，与 mode 4 使用相同原游戏 root trampoline、相同
deep 纹理池和缓存内容。唯一变量是 `VPTexturePool.idle_resize`：

- mode 4 保留原规则：空闲 victim 尺寸不同且有 GPU 引用时，先返回 PRESSURE。
- mode 5 允许先淘汰尺寸不同的**空闲** victim。CPU pin 或 GPU ref 非零的槽始终
  不可淘汰；无空闲 victim 时，仍返回 PRESSURE，由真实通知完成后重试一次。
- 小页 provider 的 release/allocate 通常只改 chunk 页表，不做系统 memblock 重建。
  这避免把“有任意 GPU 引用”等同于“必须等待 GPU 才能申请内存”。
- 该变量在原有 A/B 文件轮询中发布，实际 miss 在 GXM worker 内读取；不创建额外
  原始游戏补丁，不改变任何原字节、跳转范围或调用 ABI。4↔5 不重建纹理池。
- 普通启动仍关闭该实验。没有把 layout/shared/native C 优化混入测量。

宿主新增场景：满预算中一个小槽由 GPU 持有、另一个大槽已空闲，新小纹理可通过回收
空闲大槽申请成功；持有中的像素保持逐字节不变。相同大槽被 CPU pin 住时仍拒绝淘汰。
旧“先等待并保留异尺寸槽”测试完整保留，用于 mode 4 对照。

缓存淘汰顺序变化可能提高解码次数，等待减少本身不能证明整帧收益。必须同时比较整帧、
纹理解码、内存系统调用、拒绝绘制与画面，再决定是否采用。

## 验证与实际结果

输入身份检查通过。L1：Windows Release CTest 44/44 通过，含新计数及分析器回归；
生产 pending 函数抽取测试同时检查已完成 poll 计数、失败计时与 GPU lease 保留。

L2 计时构建 VPK `b575a9187bfc660d7929090b55aad55d1b4ebff2359530a13173369d9424ada8`，
SELF `ee248393a76902b044813d749bfed9d07781ccdf4ad379e5e586784da8d9bb73`。
首次构建默认资源目录为 static-shaders-current，运行器因其与已安装包不符而拒绝运行，
该轮没有运行证据。改用已验证的 static-shaders-rg32f 目录重新构建，未放宽身份门禁。

L3 定位运行 `build/runs/gpu-wait-attribution2-20261104`：Vita3K d7edcbe9，OpenGL，
540p，固定测试存档/村长对话。正常配置，不使用 SurfaceReadback；315/355 秒窗口检查
村庄、人物、地面、衣服与 UI 可见。382.853 秒自然退出、exit 0；存档与 markers、原安装
eboot 和模拟器配置恢复核验。此轮启动期间并行编译过候选包，不用其整帧均值给出跨包
加速比；以下选择编译结束后的稳态窗口，只用于定位等待原因。

分析请求 330–370 秒，完整端点为 334.301543–369.425958 秒，共 224 个显示帧间隔：

| 原因 | 通知阻塞 ms/帧 | 占通知阻塞 | 真实等待/帧 |
|---|---:|---:|---:|
| texture-pool | 14.635 | 66.50% | 11.00 |
| clear | 6.636 | 30.15% | 4.00 |
| sync-blit | 0.474 | 2.15% | 3.00 |
| submit-end | 0.155 | 0.70% | 0.98 |
| resource | 0.078 | 0.36% | 0.05 |
| arena-reuse | 0.030 | 0.14% | 0.42 |
| 合计 | 22.009 | 100% | — |

所有快照的每类时间/次数与全局通知计时精确对账，无失败等待。arena-reuse 还有 3715 次
poll 命中，但真实阻塞很小。故暂缓推迟 arena 等待，把空闲页回收列为首个实验；clear
次之。不能把 raw 等待占比理解成可直接获得同等帧率收益。

分析输出为上述运行目录的 `gpu-wait-report.json`。

第一轮候选 `build/runs/gpu-wait-recycle-ab-20261104` 为 643.061 秒、exit 0，恢复回执
通过。VPK `142370ce796fa36650617577b6f265997f2cf161ec5b0e450fd4e25d68958d22`，
SELF `cbe18dce3efa3cd4e25cf3a0097f6f4e1698aa157acf806ba8a2a0c81ecd6fae`。
**该轮整体加速比排除**：315 秒截图仍是加载页，395 秒捕获了 Qt 启动器，游戏窗口还
曾被最小化。恢复游戏窗口后帧时明显改变，不能混为同一受控条件。分析器仅验证数值完整，
不会替代人工场景/窗口身份验收；其汇总不是性能通过结论。

该轮 B1/B2 窗口每帧约 1 次 texture-pool 真实等待，A2 为约 11 次；但 clear 等待约
从 6.5 增至 13–14 ms/帧，说明等待发生了转移。475 秒原策略与 555 秒候选的实际
村庄窗口未见新增明显缺件/黑块，CUA 恢复后也观察到人物、地面、建筑和对话正常；
不把 Qt 启动器截图当游戏画面验收。

### 有效同进程复测（L3）

后续范围校正（ADR 0026）：该轮带 `capture-live inputs=0`，其捕获后 enabled 状态
会让旧 clear fusion 持续关闭。下面 5.25% 的同进程对照仍是该配置下的观察，但不能
当作完全关闭诊断时的生产默认性能；新清屏实验将 GPU 读回与性能测量分开。

目录：`build/runs/gpu-wait-recycle-repeat-20261104`，同一 VPK/SELF、模拟器及资源包，
普通 OpenGL、540p、`disable-surface-sync=true`，没有使用 SurfaceReadback。
该轮期间不编译；测量起点推迟到 360 秒，各段再排除 20 秒。实际模式切换约为
420/480/540 秒，结束分析截到 628 秒之前。375/435/495/555 秒窗口均为实际游戏窗口，
回执非最小化，人工复核为相同村长对话与固定机位；人物、衣服、地面、建筑、阴影和
经过的羊/NPC 未见新增明显缺失或黑块。四次图像采样不是全动画逐帧证明。

| 阶段 | 完整帧间隔 | 整帧平均 ms | 通知阻塞 ms/帧 |
|---|---:|---:|---:|
| A1 deep | 224 | 164.711 | 23.199 |
| B1 deep-recycle | 224 | 156.517 | 21.174 |
| A2 deep | 224 | 167.062 | 23.544 |
| B2 deep-recycle | 384 | 157.574 | 21.232 |

严格整帧合计原策略 **448 帧、165.886 ms**，候选 **608 帧、157.184 ms**；
平均帧时间下降 **5.246%**，约 **6.028 → 6.362 FPS**。两对相邻窗口均改善，
P95 **194–195 → 175–176 ms**，P99 **218–219 → 191–192 ms**。
分位数仍为 1 ms 桶范围，不报伪精确小数。

| 原因 | deep ms/帧 | deep-recycle ms/帧 | 判断 |
|---|---:|---:|---|
| texture-pool | 15.415 | 6.773 | 约 11 → 1 次真实等待/帧 |
| clear | 7.063 | 13.393 | 仍是 4 次/帧，承接了部分未完成工作 |
| sync-blit | 0.545 | 0.520 | 非本次主要收益 |
| arena-reuse | 0.046 | 0.042 | 无需优先优化 |
| 全部通知等待 | 23.371 | 21.211 | 净少约 2.161 ms/帧，不是少 8.6 ms |

独立完整 budget 组的纹理解码约 20.06 → 19.08 ms/帧，本窗口没有出现用更多解码抵消
等待收益的现象。GXM 准备、batch 形状和解码成本也受回收顺序影响，计时项有嵌套，
不能把整帧少 8.702 ms 全归为纯 GPU 等待下降，也不能与 ADR 0024 跨配置相加。

四段稳态中 `direct_fail`、`direct_quarantined` 为 0，staging/upload memcpy 仍为 0；
kernel_allocs/kernel_frees 的稳态增量均为 0，物理纹理 chunk 总额仍为 64 MiB。
stage 9–13（资源/绑定/提交阶段）拒绝增量均为 0；已有 stage 3 拒绝分别为 5/5/5/19，
窗口长度与自然动画工作不同，未证明每个被拒绝 draw 完全等价，不声称全绘制验收。
末次 texture-pool 快照 pressure_fail、nomem_events、direct_stale 均为 0，refs_live 为 0。

632.513 秒自然退出、exit 0，存档和 markers 哈希恢复回执通过；独立复核原安装 eboot
仍为 `d9d28e97…`、原模拟器配置仍为 `5d1d55e0…`。复测使用已开始的自动脚本完成，
用户 Esc 停止桌面操作后没有继续发出人工桌面输入。

原始日志、phases、完整直方图与输入哈希绑定在 `strict-analysis/report.json`；
按原因统计在 `gpu-wait-phases.json`，资源/拒绝计数在 `counter-phases.json`。

**决定：保留 mode 5 为默认关闭的有效候选。** 这轮证明了有限村庄窗口约 5.25% 的收益，
没有证明全场景稳定收益；不替换用户稳定安装包。下一优先级是 clear 边界的等待，需先
区分真正写入附件与仅记录延迟 clear 的场景，保留 CPU 可见性与资源依赖，不能直接删等待。
普通启动直接启用该候选、其他场景、长时间运行及真机验证仍未完成。

L0 布局验证：新增计数结构 2808 字节，没有新增 guest 补丁位置。候选启动的模块落点
为 main `0x8554a000`、SDK `0x8154a000`；既有公共入口 `main+0xbbe9c8` 原字
`0xe92d4ff0`，trampoline 位于 `[0x8774a008,0x8774a014)`，入口分支距离
23336496 字节，仍在既有 ±32 MiB 检查内。SDK 首个 SVC 仍为 `SDK+0xc0668` 的
原字 `0xef000001`，island `0x81f4b250`，原字节/范围/距离门禁均保留并接受本次构建。
控制文件增加 5 只扩展宿主实验选择，不放宽二进制身份或接受其他游戏版本。

真机 L4、任务/住宅、全部动画、长时间运行及完整玩法 L5 未验证。
