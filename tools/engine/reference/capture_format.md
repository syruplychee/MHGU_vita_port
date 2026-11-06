# SDK 实际编码采集

仅诊断构建使用 `-DVITA_PORT_ENGINE_CAPTURE=ON`，默认构建不包含采集器。
仍需现有 `engine-full.enable` 才会进入接管路径。第 3000 次公共入口采集一个队列，
结束后返回 `-100`，通过已有 `0x252` 失败桥明确停止 guest 调用。这不是正常玩法验收。
内存分配和文件写入在 host TLS 下进行；guest TLS 下仅复制字节。

重新运行前须将已有三个采集文件移到本轮的备份目录。当前文件服务用 `fopen("wb")`
覆盖旧文件时可能留下更长旧文件的尾部；比较器严格检查文件长度，不会默认忽略尾部。
运行日志必须出现 `ENGINE_SDK_CAPTURE status=ok`；容量耗尽、命令换块和文件写入失败
均不构成有效采集。固定容量为 4096 页、8192 次接口调用和 4 MiB 编码字节。

所有整数为小端，所有地址为采集进程的实际 32 位 guest CPU 地址。GPU 地址及查询
返回值保留 64 位。文件留在忽略目录，不提交资源、快照或程序生成物。

## 三个文件

- `engine-full-pages.bin`：重复的 `u32 page_address + u8 data[4096]`。页对齐、不可重复，
  每页仅保存首次触及的 preimage；上下文在入口复制，其余资源按访问取样。这不是全进程
  同一时刻的原子快照。
- `engine-full-encoded.bin`：按接口顺序追加 SDK 本次新写的 GPU 命令字节，再追加 control
  字节。保留实际地址，不用假的 GPU arena 代替。当前仅支持不换块的队列。
- `engine-full-calls.bin`：44 字节 header，接着 `count` 个 280 字节 record。

header 为 11 个 `u32`，依次为：magic=`0x31435056`、version=1、header_size=44、
record_size=280、count、pages、blob_bytes、main_base、ctx、state、scratch。

| record 字节偏移 | 内容 |
|---|---|
| 0 | `VPEngineNvnCall`，80 字节，含原 main 调用位置、slot、r0–r3、8 个 stack word、stack_words、s0–s3、s_words |
| 80 | 实际 slot 函数指针 u32 |
| 84 | has_result u32；查询必须为 1 |
| 88 | 实际查询返回 u64；非查询不解释 |
| 96 | 调用前 command object 的前 0x50 字节 |
| 176 | 调用后 command object 的前 0x50 字节 |
| 256 | GPU 输出 `{start, file_offset, bytes}`，三个 u32 |
| 268 | control 输出 `{start, file_offset, bytes}`，三个 u32 |

查询调用的 command object / 输出字段为零，不把 r0 查询对象当作 command buffer。
每个零长度非查询输出仍记录真实 cursor 和当前文件 offset。

## 对照方式

`compare_sdk.py` 接受锁定解码后的 main / sdk、与诊断包对应的未剥离 ELF、三个采集文件
及已核对的 `nvn-function-map.json`。它从每个实际函数指针反算 SDK base，所有调用必须
指向同一份 SDK。分别执行原始公共入口和本项目编译出的 ARM C，再执行原 SDK 编码器，
逐接口与运行时真正写出的字节比较；地址查询也执行原 SDK，不替换成固定返回值。

缺页、未返回、指令预算耗尽、外部资源调用或 SDK 分配回调立即失败。相同长度内的编码
差异保留事件位置及首次差异字节，继续检查后续接口，最终仍返回失败；不修补输出让比较通过。
允许创建的空内存仅为已知模块 BSS、执行栈和采集证明存在的输出范围，不为缺失资源
创建零页。数值 `pow` 仍使用宿主数学函数，不扩大为原 SDK 数学库逐位等价证明。
成功只证明采样队列，不能代替正确图像、其他阶段或真机验收。

优化查询可用保留全部实际查询返回的完整旧采集验证：显式指定
`--native-entry vp_engine_full_run_fast --allow-query-elision`。只允许新路径省略纯查询，
仍逐字节检查所有非查询命令与完整状态写回，资源清单也仍检查。默认严格模式不变。
不支持优化 marker 与采集编译同时启用，该组合在加载时明确拒绝。

`resources.json` 给出 SDK 编码时使用的 program header / 命令包、buffer / pool / vtable、
vertex layout、状态对象和 target / view 的地址、长度和 SHA-256。两条路径的清单必须一致。
buffer 同时记录 pool offset、原始大小和 64 位 GPU 地址。这里比较的是编码器读取的
资源描述及 program 命令包；不会把没有采集的顶点、索引和纹理像素内容说成逐字节验证。
