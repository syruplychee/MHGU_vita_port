# src/loader

原生加载与启动链：

- `nso_loader.c`：NSO 头/段/BSS 解码与身份校验。
- `native_link.c`：重定位计划/提交、导入槽绑定。
- `patch_loader.c`：绑定原字节的补丁写入、唯一修正者台账。
- `native_arena.c` / `native_executor.c` / `native_unwind.c`：VM arena、guest 执行器、
  EHABI 展开注册。
- `rtld_connections.c`：rtld 兼容对象与五条 SDK 连接。
- `sdk_dso.c`：执行原 SDK DSO 初始化，桥接 SVC/Timer/Barrier/TLS/锁。
- `native_lifecycle.c`：启动/初始化/析构自检。
- `fixtures/sdk_cpp_fixture.cpp`：C++ 异常展开 fixture（自行编写）。

不允许：跳过身份校验、静默替换模块、用成功 stub 代替未实现入口。
