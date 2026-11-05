# 0033：保留排版度量的点阵字体与单通道上传

日期：2026-11-05。状态：工具和运行时已实现；Vita3K 对话验证 L3，真机未验证。

## 决定

使用 GNU Unifont 18.0.01 生成普通文字的二值遮罩。保留原 GFD 的逻辑字号、advance、
行高、字符顺序、重复码点、未知标志和 kerning/index 高 16 位；不把字体改成等宽。
每字按原 cell 的 3/4 尺寸制作点阵，再映射回逻辑 cell；重排 UV，bearing 归零，
完整 cell 作为新矩形。源遮罩只有 0/255。原本零面积的字形保持空白，不凭空补字。

普通 GFD 字库使用原游戏 TEX `0x2c` 创建路径，物理 NVN 格式为 `1`（R8），1024²、
单 mip。固定网格系统字体使用 TEX `0x12`，保留其红通道采样语义。两者不可只因物理
格式相同就互换。格式表 `main+B0AE10` 对 7、18、44 分别写入 `0x25`、1、1；
`B0811C` 会为 18 推导 `0x4000` 附加标志，44 不走这一分支。

L3 对照中，普通字库直接用 `0x12` 导致对话文字不显示；相同点阵/GFD 用 RGBA8 能
显示，改用 `0x2c` 后单通道文字恢复。只改原生对象的格式标记为 7 也没有修复。
这些结果证明必须区分原游戏创建路径；尚未逐条证明后续哪条内部条件造成 `0x12`
文字消失，不把推断写成完整根因。修改格式标记的临时探针已移除。

## 原生资源接管与边界

复用 ADR 0032 的六处原始 rTexture 生命周期 island。`pixel-font.enable` 在启动时
启用它们；普通运行不需要 `resource-lifecycle.enable` 的完整对象 dump。
load-commit 从 r4 的 rTexture `+D0` 取得原生 GPU 对象；从该对象读取 `+28` 的原游戏
格式、`+1E/+20` 的宽高、`+B8` 的完整 64 位 GPU VA。普通字体要求格式 `0x2c`；
系统字体要求 `0x12`。仅接受已审查的完整资源名。load-start、replace、release 和两种
析构均撤销旧登记，地址复用不能沿用原身份。登记满或布局不符明确失败。

这不是内容版本缓存：raw 全字节比较、decoded generation、CPU pin、GPU ref 和通知后
回收保持原契约。不以“字体不变”跳过比较，不释放在途 GPU 资源。

TIC 解析缓存仍只缓存原始描述符。每次解析后的有效视图依据当前资源登记决定：

- 普通字体使用 `(1,1,1,R)`，保持 shader 看到白色 RGB 和遮罩 Alpha。
- 系统字体保留 `(R,0,0,1)`；原 shader 读取红通道。
- 已登记字体使用 point min/mag、禁用 mip 过滤；其他纹理保留自己的过滤状态。
- 原 TIC/TSC 字节保留作证据；只改有效采样状态。视图必须为单层 2D、R8 UNORM，
  不支持的类型、depth、mip、storage 组合明确拒绝。

## Alpha8 / R8 的 CPU 与 GXM 表示

`VP_TEXTURE_UPLOAD_ALPHA8=0x41d` 对应 Maxwell `0x1d`、四 UNORM 字段 `0x492` 和
selector `0x5ff`，GXM 为 `SCE_GXM_TEXTURE_FORMAT_U8_R111`。规范红通道视图
selector `0xe02` 使用 `VP_TEXTURE_UPLOAD_R8=0x51d` 和 `SCE_GXM_TEXTURE_FORMAT_U8_R`。
不把未证明的 Maxwell `0x1c` 当作同义格式。

离线 TEX、CPU raw、decoded、GXM backing 都是单通道，不先建长期 RGBA 副本。
block-linear 和 pitch-linear 解码器先产出紧密遮罩，随后按 GXM linear 的八 texel
行对齐倒序原地补零；实际分配大小为 `align8(width)*height`。1024² 正好 1 MiB。
primary、extra、vertex、decoder-owned 直绑和复制路径共用大小/格式规则。没有合法
decoder-owned 结果时，mask8 显式拒绝，不静默扩回 RGBA。

## 可选原字体内部诊断

`pixel-font-trace.enable` 随字体模式额外安装两处只读观测，每类仅输出前 48 次：

| main 偏移 | 重放原字 | 服务 | 含义 | 64 B guard SHA256 |
|---|---|---|---|---|
| `B82684` | `e92d4010 e1a02000` | `25D` | 字形二分查找，r0 字库、r1 码点 | `5bbbaf50dbe0e63e5ea0e3bb85970b743e269c57f7c0033153d95b80d6b06f48` |
| `B82C78` | `e5d020c4 e3120001` | `25E` | 原纹理页 getter，r0 字库、r1 页号 | `d3633506c2cdf79dcf4c78752e76f340c1bd6cee4f15399a1e67f739c7f466e5` |

每处覆盖 8 B，island 96 B，重放后使用完整 bridge 并回到 site+8；GPR/VFP/FPSCR、
APSR 和栈保持。第二处 TST 的标志不得丢失。正常字体模式仍只安装六处生命周期 hook。
原 main 身份沿用 ADR 0028/0032；所有 guard 和分支范围先检查后写入，缓存维护沿用
原加载流程。诊断看到了 21478 条新记录、11 页和实际查字/取页；不替代画面验证。

## 离线资源与读取

`build_pixel_fonts.py` 扫描全部 ARC 表和独立 GFD。同包按 GFD prefix/页号配对 TEX，
不同语言/版本不能混配。重复内容按 GFD+页 TEX SHA 缓存。ARC 的无关 name/type/flags/
压缩 payload 原样保留，只调整目录数量、偏移和实际字体条目。独立 GFD 必须匹配已
转换的嵌入版本，否则失败。输入 resident.arc 和 Unifont SHA 锁定，原树只读。

`build_romfs_index.py` 支持独立 overlay：虚拟名称不变、文件大小和 offset 按新内容
重建；map 的物理路径指向 `romfs-tree/` 或 `font-pixel/overlay/`。字体模式从
`font-pixel/romfs.index` 和 `romfs.map` 读取，tree root 为数据目录本身。缺失/损坏时
明确失败。关闭 marker 并重启后恢复普通 RomFS 元数据，不覆盖原游戏输入。

字体输入由 `fetch_pixel_font.py` 从官方站点获取并核对固定哈希，附带 OFL 和原字体
copyright。派生遮罩名称为 MHGU Vita Pixel，使用 OFL；这不改变游戏容器和其他内容
的权利。所有转换结果、预览、日志和 VPK 仅留在忽略目录。

## 验证范围

宿主覆盖 U8 两种布局、奇数尺寸/行 padding、长度拒绝、旧 RGBA+swizzle 的逐像素
等价、GXM 格式映射、资源登记失效、GFD 未知字段/secondary/重复记录、缺字失败、
空私用槽、ARC 增删页与无关压缩数据保持、overlay map。Windows 56 项 CTest 通过，
Vita 构建通过。实际包扫描、L3 运行、占用数字和限制见 [字体结果](../19-pixel-fonts.md)。
