# 0011：对原视频颜色转换做定向计时

日期：2026-11-01。状态：已实现；L1/L2 通过，L3 已启用运行：4 处补丁接受，0 条完成记录，证明这两个函数不在当前片头路径。

## 背景

原生表面读回已命中，SDK 锁已减少桥接开销，但最近一次受控窗口仍约为每秒
6.8 次解码、6.49 次合成。计数不等于显示帧率。此前逐级条件等待追踪表明，长等待
主要发生在上游发送信号之前，不能据此认定内核唤醒慢，也不能直接认定颜色转换慢。

需要先确认原平台颜色转换是否实际执行，以及一次执行花了多少时间。本次只加计时，
不替换算法，不修改播放器时钟、跳帧、队列、返回值或断言。

## 输入身份与函数范围（L0）

模块是 `subsdk0`，不是游戏 `main`。输入绑定 `config/target-1.4.0.json`：

- NSO SHA256：`75fb812705612ec68cd0760640183a140a32d4d07181a40355cc002c52096245`。
- Build ID：`c2774fa4cace9ecc6a712bfc839214c19515f36e000000000000000000000000`。
- 用于反汇编的旧 ELF 整个 `.text` 与锁定 NSO 解压 text 逐字节一致，共 3542512 字节；
  text SHA256 为 `1b97455942726ee28e611ead1aea1298f67de5cc85fba2f2d47959dc0f7f7c05`。

| 原平台函数 | 模块相对范围 | 完整范围 SHA256 |
|---|---|---|
| `NnCodecVideoSink::ConvertYuvToRgb` | `[0x33c4e8, 0x33c694)`，428 字节 | `fd83cf277feb8d26b9990d17bdb75cc0e6d91831045cce145cc05427cf8a98cf` |
| `NnCodecVideoSink::ConvertYuvToNv12` | `[0x33c694, 0x33c880)`，492 字节 | `5f85c2e781d5add839d2cde732dae770c7b62b99f98c32582edb7cbf595cc65c` |

反汇编来自本机 `arm-vita-eabi-objdump -d -m armv7`，没有反编译或重编译原函数。
契约见 [movie-color-probe.json](../../config/patch-plans/movie-color-probe.json)。

## 开关、补丁与重放

默认不打这四处补丁。启动时只检查一次
`ux0:data/mhgu-vita-port/media-color-probe.enable`；文件存在才启用。启动日志记录
`MOVIE_COLOR_PROBE_MODE enabled=0/1`。移除 marker 并重启即回到未修改的原函数。

启用前检查模块文件 SHA256、Build ID、两段完整原函数 SHA256 和四条原指令；任何不符
立即拒绝启动，不带着不完整的探针执行。只在 `subsdk0` 自身的 island 末尾追加四个
8 字节绝对跳转 veneer，共 32 字节，不改变 SDK 的岛偏移。原位置写一条 A32 `B`。

四个 veneer 分别进入完整的 336 字节保存桥，保留全部整数寄存器、APSR、FPSCR 和
32 个 D 寄存器。原函数的 guest 栈不改位置。处理器只重放下表的整数访存指令，然后
把恢复 PC 指向原位置后 4 字节；不会返回到当时可能已经用于临时计算的 guest LR。

| 位置 | 原字节（小端） | service | 必须重放的操作 | 恢复位置 |
|---|---|---|---|---|
| RGB `+0x33c4f4` | `40e090e5` | `0x24c` | `LDR lr,[r0,#0x40]`，写入保存的 guest LR | `+0x33c4f8` |
| RGB `+0x33c678` | `000081e5` | `0x24d` | `STR r0,[r1]`，先检查当前写权限 | `+0x33c67c` |
| NV12 `+0x33c6a0` | `0c108de5` | `0x24e` | `STR r1,[sp,#12]`，使用原 guest SP 并检查写权限 | `+0x33c6a4` |
| NV12 `+0x33c868` | `000089e5` | `0x24f` | `STR r0,[r9]`，先检查当前写权限 | `+0x33c86c` |

入口时，`r0` 是 this，`r1` 是目的指针，`r2/r3` 是原调用的源参数；宽、高来自
`this+0x3c`、`this+0x40`。不修改 RGB 的逐像素浮点运算，也不修改 NV12 的原始复制、
对齐配置或输出长度计算。出口写回原函数算出的 `r0`，继续执行原 epilogue。

其中 RGB 原函数实际读取 `r2` 指向的 packed NV12：Y 行跨度为 `align16(width)`，
UV 起点偏移为 `align16(height) * align16(width)`。原函数会覆盖传入的 `r3`，不把它
作为独立 UV 地址使用。因此日志字段 `source_y/source_uv` 分别只是入口 `r2/r3`
快照；RGB 的 `source_uv` 不能解释为实际 UV 来源。NV12 路径同样只记录参数，不由
探针推断或改写真实像素布局。

## 计时口径与日志

Session 为主 executor 和 63 个 worker executor 各保留一项计时状态。入口记下模式、
宽高、源目的参数和进程单调时间；出口核对同一 executor 的模式并输出
`VP_MOVIE_COLOR_TIME n=... mode=rgb/nv12 thread=... width=... height=...`
`destination=... source_y=... source_uv=... bytes=... elapsed_us=... entry_us=... t_us=...`。
仅记录前 2000 次完成的转换，之后每 32 次记录一次。

同一 marker 生效且命中 CPU 视频表面原生读回时，探针在前四次还输出有界的
`VP_MOVIE_READBACK_STACK`、`VP_MOVIE_READBACK_REGS` 和
`VP_MOVIE_READBACK_WORDS` 栈快照，用于核对调用方；这些行默认关闭，不属于正式性能
日志，也不能用来推断未观测到的读回成本。

`elapsed_us` 是两处探针之间的墙钟时间，包含 guest 调度、桥接和可能的等待，不是纯
CPU 执行时间。日志写入和探针自身也会影响运行速度，因此用于定位，不单独用来宣称
正式包达到目标帧率。没有命中或样本不足时必须报告“未观测到/不可用”，不能报转换
成本为零；出现 RGB/NV12 之外的实际平台路径，也不能用本探针覆盖它。

## 验证与未验证范围

- L0：锁定 NSO 身份、旧 ELF 整段 text、两段函数 hash 和四条原指令已核对。
- 构建结果由对应运行记录标注，只有编译打包通过才能记为 L2。
- 尚未证明这两个转换函数是当前片头的执行路径，也未证明它们是性能瓶颈。
- 2026-11-01 探针运行：4 处补丁全部接受，`VP_MOVIE_COLOR_TIME` 为 0 条；在本片头中
  这两个函数不是执行路径（L3，单次 Vita3K 运行）。剩余成本另见当前状态第 7 节。
- 本次 L3 只证明这两个函数未被调用；如果未来路径变化导致命中，仍需单独验证计时和画面。
- 真机寄存器恢复、缓存一致性、性能和显示仍未验证（L4 未达成）。
