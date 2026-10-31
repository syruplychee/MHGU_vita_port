# 术语表

| 术语 | 含义 |
|---|---|
| **NSO** | Switch 可执行模块格式；本项目的四个输入模块是 `rtld/main/subsdk0/sdk` |
| **NPDM** | Switch 程序元数据（权限、内存属性等） |
| **NACP** | Switch 程序信息（名称、版本等）；`control.nacp` |
| **ExeFS** | Switch 可执行文件系统；包含四个 NSO 与 `main.npdm` |
| **RomFS** | Switch 只读资源文件系统；本项目使用解包后的 `nativeNX/` 树 |
| **NVN** | Switch 图形 API；游戏通过它提交图形命令 |
| **Maxwell** | Switch GPU 架构；命令流和着色器需要翻译 |
| **MME** | Maxwell 宏引擎；命令流中的宏/常量更新 |
| **SPK** | 游戏的 shader 程序库容器格式 |
| **GXP** | Vita GXM 着色器程序二进制格式 |
| **GXM** | PS Vita 图形 API |
| **Cg** | NVIDIA 着色语言；项目将 Maxwell 翻译为 Cg 再编译为 GXP |
| **Shader IR** | 离线中间表示，用于类型/资源分析和 Cg 生成 |
| **Guest** | 被移植的游戏代码（AArch32） |
| **Host** | Vita 上的原生运行环境（ARMv7 + VitaOS） |
| **Bridge** | guest/host 之间的 ABI、寄存器、栈、TLS 转换层 |
| **TLS** | 线程本地存储；Switch SDK 使用 CP15/TPIDR 访问 |
| **SVC** | 监督调用；原 Switch SVC 不能直接在 Vita 上执行 |
| **HLE** | 高层模拟/服务替换；本项目只替换平台接口，不模拟整机 |
| **Relocation** | 模块搬家后修正地址的过程 |
| **VPK** | PS Vita 安装包 |
| **SELF** | Sony 签名的 ELF 容器；VPK 中的 `eboot.bin` |
| **eboot.bin** | Vita 应用入口 |
| **ux0:** | Vita 的用户存储挂载点 |
| **Scene** | 一次 BeginScene/EndScene 之间的 draw 集合；GXM 批处理单位 |
| **Staging** | CPU 写入、GPU 读取的临时内存页/缓冲 |
| **Clear fusion** | 把整面颜色 clear 延迟到下一次同目标 draw 的优化 |
| **Marker** | `ux0:data/mhgu-vita-port/*.enable` 一类开关文件 |
| **Static bank** | 离线生成的着色器索引 + GXP + 资源 sidecar |
| **Sidecar** | 与主索引分离的每 stage 资源布局信息（`VPSRES1`） |
| **L0–L5** | 证据级别：静态/宿主/构建/Vita3K/真机/玩法 |
