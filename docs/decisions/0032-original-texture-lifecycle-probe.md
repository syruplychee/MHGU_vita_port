# 0032：原游戏 rTexture 生命周期取证

日期：2026-11-05。状态：默认关闭观测；不启用按地址跳过 raw 验证。

## 入口和证据

从锁定 main 的 `rTexture` 字符串 `1579C57` 追到 `B51C4C` 注册函数。该程序用 PC 相对
literal 计算字符串地址，直接查绝对指针没有引用不表示它没被使用。已核对
`B51CC0` 的 LDR、literal `B51CF8` 和 `B51CCC` 的 ADD PC，实际计算得到该字符串。
注册对象大小 `F0`，工厂 `B512F8` 将实际对象虚表设置为 main 相对 `17936DC`。

`B51464` 读 TEX 魔数 `00584554`、版本/尺寸/mip，经过原 stream 方法读像素，创建
原生 GPU 对象，最后在 `B51AC8` 写入 `rTexture+ D0`。`B51B00` 会替换该字段，
`B51BFC` 释放旧 GPU 对象。两个析构函数也有内联释放，不能仅靠显式 release 入口
判断对象还活着。

静态分析使用只读 Ghidra，会话中修正旧 PLT no-return 元数据，数据库不保存。
关键指令另由锁定 NSO 解码核对。没有把反编译伪代码当原始源码或完整调用图。

## 六处观测点

`resource-lifecycle.enable` 在启动时决定是否安装。生产位置/原字/64 B guard SHA256
均在 `src/loader/resource_experiment.h`，原 main 身份沿用 ADR 0028。

| main 偏移 | 名称 | 重放的两条原指令 | 观测对象 |
|---|---|---|---|
| `B51464` | load-start | `e92d4ff0 e24dd09c` | r0 |
| `B51AC4` | load-commit | `e3560000 e58460d0` | r4，已写入新 GPU 对象 |
| `B51BFC` | release | `e92d4010 e1a04000` | r0，释放之前 |
| `B51B00` | replace | `e92d4830 e1a05000` | r0，替换之前；r1 为新对象 |
| `B513B4` | destroy | `e92d4010 e1a04000` | r0，析构之前 |
| `B513FC` | destroy-delete | `e92d4010 e1a04000` | r0，析构/删除之前 |

全部 guard 和到 host bridge 的 A32 B 距离先核对，再开始改写。每处覆盖 8 B，放
`LDR pc,[pc,#-4]` 和 SDK island 地址。island 先重放两条原指令，再进入已有完整 bridge，
最终回到 `site+8`。每处 island 96 B，六处共 576 B；GPR、VFP、FPSCR、APSR 和
重放之后的 SP 保留。`load-commit` 的 CMP 标志必须保留给后续条件指令，不能漏掉。
不通过 C wrapper 调原函数，不挪动原函数的栈参数；不改写观测现场的寄存器。

服务号 `257–25C`，按执行顺序记录事件、对象、原资源名、尺寸、GPU 指针和调用现场。
映射可读时再记录原生 GPU 对象的 `210` B。输出单条 GPU 快照使用单次 fwrite，避免
同一日志的其他线程插入半行。记录只写忽略的运行目录，不提交像素或游戏资源。

## 如何关联以及如何使用

同一次运行中，GPU 对象 `+B8` 的 64 位值与有界 `VP_FRAME_RESOURCE` 的完整 key
相等，并且宽高相等，才能建立观测关联。这个字段位置目前是运行数据关联证据，
不声明为所有 SDK/对象版本的公共 ABI。

`analyze_resource_lifecycle.py` 按日志顺序维护已观察的 load，load-start/replace/release/
两个 destroy 失效旧对象。不把后面重新使用同一 VA 的新名字倒填到前面的请求。
同名但多对象只证明资源名字一致，不宣称唯一活对象。截断/混行单列；未知或有冲突则
保留 UNKNOWN，不用最近一个地址硬猜。

这仍不是完整内容版本：还没有覆盖所有 CPU 直接写、SDK upload、DMA/copy、GPU 生产者、
独立原生纹理对象和全部别名。没有在这些路径闭合前关闭逐字节 raw 比较，亦不以
资源名、地址、对象计数或哈希代替内容验证。十张大纹理的真实资源名、实验结果和剩余
工作统一见 [三项调查](../18-resource-lifecycle-and-capacity.md)。
