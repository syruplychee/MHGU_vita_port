# src/bridge

guest（AArch32）与 host（Vita ARMv7）之间的边界：

- `guest_context.c`：按 SP 查找 guest 上下文、RO/RW TLS、注册表 seal。
- `host_bridge.c`：完整宿主调用桥、FPSCR/寄存器/栈切换、重入拒绝。
- `armv7/host_bridge.S`、`guest_tls.S`、`guest_tls_island.S`、`udiv_island.S`：
  固定 A32/Thumb 入口、TLS 岛、整数除法岛。
- `armv7/loader_boundary.S`、`sdk_dso.S`、`sdk_svc_site.S`、`sdk_timer_site.S`：
  加载器/SDK DSO 的边界与站点模板。

约束：

- 保存/恢复调用者可见寄存器、flags、VFP/NEON、FPSCR、栈对齐和返回状态。
- 不把 Switch SVC 直接交给 Vita 内核。
- 岛区必须保持原字节绑定；代码写入后做 cache 维护。
- 每个上下文单宿主线程，嵌套进入明确拒绝。
