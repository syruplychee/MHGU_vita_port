# 0013：Session 使用原子状态与信号量门锁

日期：2026-11-01。状态：实现；L1/L2 通过，L3 单次运行使用 backend=user-gate，Session 等待中位 1 µs、p95 612 µs；内核互斥对照与真机待验证。

## 背景

SDK 轻量入口仍需要和 dispatch 共用 Session 保护。实测只尝试获取 Session 会使
播放时每秒约三万次调用退回完整桥；改为等待后，轻量入口才真正命中。每次进入和
退出 Session 都调用内核互斥体，仍为大量短操作增加了内核接口成本。

改用一个有真实互斥、递归和睡眠语义的用户态门锁。只替换加载器自己的 Session
保护，不改变 guest SDK 锁的对象、游戏播放器、队列、时钟或跳帧行为。

## 协议

生产实现为 `src/platform/session_gate.h`。三个原子状态：0 空闲、1 已持有、2 已持有
且可能有等待者。CAS 从 0 取得所有权；争用者用原子 exchange 发布 2，无法取得时
在计数信号量上真正睡眠。退出最外层先清 owner，再以 release 顺序发布 0；原状态
为 2 时发一个信号。取得所有权使用 acquire 顺序，保护区内的数据因此可见。

信号量可以保存“发布空闲后、等待者进入内核前”到来的唤醒，避免丢失唤醒。它可能
保留过期 token，但醒来的线程必须重新检查原子状态，不能仅凭 token 进入保护区。
这与 [条件变量](0012-condition-variable-signal.md) 不同：信号量的计数本来就有保存
唤醒的语义，条件变量只通知当前等待者。

owner 使用实际宿主线程的硬件 TLS 地址，递归只由同一 owner 更新 depth。原 SDK
的 TLS 指令已通过软件 guest 栈目录替换，guest TLS 不会覆盖该硬件身份。不能使用
共享的 ambient guest handle，因为它会在别的 dispatch 和暂停期间变化。

所有 Session 进入/退出，包括轻量锁入口、dispatch、park 和设备等待回调，统一经过
`session_enter/leave`。取得 Session 后只尝试取得 guest 主机互斥体；需要阻塞时先
释放 Session，再按原路径等待。调用者可能已持有别的 guest 锁，这与原 dispatch
顺序相同；本次没有声称调用者从不持锁。

## 错误、生命周期与对照

- 创建信号量失败时清理已创建的 Session 互斥体并返回真实错误。
- 轻量入口返回真实错误，继续由 0x24b 桥终止本次调用。原 void Session 包装无法
  把错误返回给所有回调，故记录 `SDK_SESSION_LOCK_FATAL` 后结束进程；结束意外
  返回则 trap，不能无锁继续执行。
- 共享信号量和线程栈仅在线程确实 dormant、删除成功后回收。停止检查有界等待；
  仍活跃或检查失败记录 `SDK_THREAD_POOL_FATAL` 并结束整个进程，不把失败删除
  当成已停止。这里没有新增强制删除活跃线程的路径。
- 启动前存在 `sdk-session-kernel.enable` 时，共享保护全部使用原内核互斥体；启动
  日志记录后端。同一 SELF 可作对照，不在运行中切换，也不伪造加锁成功。

## 验证范围

Windows 的 `tests/session_gate.c` 使用真实 Win32 信号量和八个线程，覆盖递归、错误
owner、先释放后 Wait、过期 token 不得越过所有权检查、八万次递归保护更新及真实
错误返回。Windows 原子操作使用 Interlocked，顺序不弱于生产的 acquire/release。
这些是 L1，不能替代 Vita 内核调度或硬件 TLS 的 L4 验证。

生产代码提取 harness 的 27 组检查覆盖 guest 锁、条件变量和两种 Session 后端。
VPK 构建为 L2；最终本地 ARM 调用链需检查无 VFP/NEON/FPSCR 操作，内核导入记录
不用于证明固件内部实现。L3 播片速率、可见画面及村庄回归应另行报告；本 ADR 不
以锁代码或编译通过宣称已经达到 30 FPS。
