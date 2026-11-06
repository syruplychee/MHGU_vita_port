# ADR 0034：按最终显示字号选字模，并保住屏幕像素网格

日期：2026-11-06。继承 ADR 0033 的 Alpha8 存储、资源身份和上传契约。

## 问题与证据

旧生成器把 Unifont 16px 缩到 3/4 cell，再放回原 cell；原游戏绘制还会缩放。
原字形发射函数把 UV 两边各收半个纹素，16px 四边形只取到 15 个纹素。
此外还有两个整帧问题：

1. 原游戏 `main+BBCA50` 在启用双显示预设时，把 `+44BAC0/+44BAC4` 初始化为
   1920×1080，`+44BAC8/+44BACC` 为 1280×720。`main+BBBDE0` 经 `869C84` 用第一组
   尺寸创建最大显示缓冲区；当前掌机视口仍是 1280×720。只改 VI 服务回复不足以改变
   这个原游戏分配。
2. 当前分辨率规则使 720p UI 为 960×540，1080p 显示缓冲区也为 960×540；720p 视口
   在后者只占 640×360。实测 token `0x20` 把 UI 缩到 2/3，present 再放大。
   最后还把 540 行拉伸到屏幕 544 行。即使文字顶点正确，也会重新破坏点阵。

字形 shader SHA `97ca92a4e6f4a349c526adecd733c616d742c293354b301d4da0e5fb850eca95`；
最终 UI copy 的 vertex SHA `ed669c11b36dbe538bf2e9d2a76e5b9e6f9b035d68a089fb56fe1705f12e8f8e`，
fragment SHA `342f922d3034de651eadb2b69c8b75f4650d7f5159ddc59ada80f3eb9909cba6`。
只在字形 shader 上通过 1:1 检查，不能作为最终屏幕清晰的证据。

## 决定

`pixel-font-native.enable` 配合 `pixel-font.enable`，使用独立 `font-native/` RomFS 包。
字体与显示设置在进程启动时确定。旧包与默认路径保留，便于有界对照。

### 原生字模与 GFD

读取 Fusion Pixel 2026.10.25 的原生 8/10/12px BDF 和 Unifont 18.0.01 的原生 16px HEX，
不 resize 字模。仅允许运行时整数倍放大。保持原 primary 字符顺序、裁切尺寸、bearing、
advance、行高和字体字号；primary 的页/UV 改为有效默认页，排版仍使用原度量。

已审查的三种普通 GFD 的 secondary count 均为 0。新包用该独立数组承载目录：

- 首条 5 个 u32：`VPF1=0x31465056, version=1, primary_count, strike_count, reserved=0`。
- 后续按 primary 顺序、再按字号递增，每字符固定 `strike_count` 条 20 字节记录。
- word0 是原 Unicode；word1 仍为 page/x/y；word2 为原生位图宽高。
- word3 低 12 位是原生 advance，12–23 位是字模字号；word4 低两字节为 signed bearing，
  高 16 位是原生 ascent。缺少某个小字模时 rect=0，不画替代方块。
- 活跃字符必须全部有 16px 字模。小字号缺字在包清单逐项列出；运行时从实际存在的字模
  中选最接近屏幕字号的整数倍，等距时优先更高分辨率的源字模。

原 GFD loader 分别分配 primary 与 secondary，布局/查字仍读 primary。
有颜色的按钮/图标不进入此目录。全部生成物和字体文件不进入 Git。

### 原游戏字形发射接管

沿用已有 96 字节 island：执行被替换的两条原指令，再进入保存所有通用/VFP/状态寄存器
的 bridge，最后回 `site+8`。8 字节现场替换为绝对跳转；loader 检查原字、64 字节 SHA、
容量和 bridge 分支可达性。未修改原函数签名、栈参数或返回方式。

| 位置 | 原 8 字节（小端） | service | 作用 |
|---|---|---|---|
| A3F8C4 | `f0482de90240a0e1` | 25F | 第一套字形发射，PUSH/MOV 后替换 r4 字形记录 |
| A55524 | `f0482de90240a0e1` | 260 | 第二套字形发射，同上 |
| A3FC10 | `170a81ed060090e8` | 261 | 两条 UV 分支的共同出口，写入完整纹素边界 |
| A55874 | `170a81ed060090e8` | 262 | 第二套发射的共同出口 |

64 字节 guard SHA 逐项见 `src/loader/resource_experiment.h`。
`audit_font_hooks.py` 在锁定 main 上验证原字和 SHA，检查所有直接 A32 分支不进入补丁第二字。
UV hook 不能放在共同出口前一条：另一分支会直接跳进第二字。该错误已由实际顶点检查
发现并修正，不能使用被否决位置 A3FC0C/A55870。

入口根据 font context 的字号和父矩阵计算显示字号。排版记录不改；bridge 给每个原生
线程准备独立的 44 字节字符副本，仅改变绘制用 glyph 指针、位置和宽高。原两个函数
只调用取页和批次查找器（B35C00，只有链表遍历，无下层调用），不递归发射字形，因此
副本在返回前有效。16 个线程槽用完报错。

以原字体 ascent 对齐基线，在原 advance 槽内居中。轴对齐静态文字把最终屏幕坐标
取整，再反算逻辑坐标；位图一像素占整数个屏幕像素。旋转/缩放动画保留其变换，不能
声称动画全过程也保持轴对齐屏幕网格。原缓存字形的父矩阵后续变化仍需专项验证。

UV 共同出口先执行 VSTR/LDM，r1 已变成批次计数。bridge 从 r5 context 和 r0-12 批次头
重建顶点地址，按当前四边形索引写完整 UV。没有改字体格式标志，也不靠扩大过滤阈值修图。

### 原游戏显示预设与平台回复

仅新模式将原游戏双显示预设中的最大尺寸改为 720p，另一组掌机预设和模式判断不变：

| 原 main 位置 | 原指令字 | 新指令字 | 含义 |
|---|---|---|---|
| BBCD34 | E3A00D1E | E3A00C05 | MOV r0,#1920 → #1280 |
| BBCD38 | E3007438 | E30072D0 | MOVW r7,#1080 → #720 |

写入范围共 8 字节。guard 范围 `[BBCD10, BBCD90)`，SHA256
`4920676f70bd71a9d4767e819c167fb06093a3147da29b847c6e3cb782572678`。
没有改变控制流、ABI、断言或失败返回。VI ListDisplays、Binder connect/query/queue
默认尺寸同时报告 1280×720，和已有 Handheld OperationMode=0 一致。

新模式要求 `540p` 和 GPU present，拒绝 bottom-origin。最终 GPU present 用完整
960×540、point、上下各 2px 黑边，保留到 960×544 屏幕的像素对应；黑底通过同场景
GPU clear 绘制，沿用原 scene completion，不增加 CPU 像素回读。

## 证据与边界

锁定输入、原字/guard/直接分支审查 L0；Windows 60 项宿主测试 L1；Vita 构建 L2。
L3 字形与最终 copy 验证、正常模式运行和图像见 [原生点阵结果](../20-native-pixel-fonts.md)。
不得把这次窗口 FPS 当正式性能对照。真机、全部字体用途/语言逐字、boss 演出、任务/
场景切换、完整播片和动画全过程仍未验证。
