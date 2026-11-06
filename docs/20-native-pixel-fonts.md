# 按实际字号选原生字模，并直接对应屏幕像素

2026-11-06。保持已验证的 Alpha8，重做普通 GFD 字体的字模选择、绘制尺寸和显示链路。
用户要求的视觉大小由原字号、字距、行高和文本框维持；字模墨迹会随点阵字体改变，
不声称逐字轮廓等同原字体。

## 已查实的原因与改动

旧方案不只存在字模反复缩放，还把 UI 画布缩小后放大：720p UI 为 960×540，而
1080p 显示缓冲区内的 720p 视口只有 640×360。改成最近邻不能找回已经丢掉的像素。
只改显示服务回复也不足够；原游戏另外硬编码了最大显示缓冲区预设。

新模式直接进入原游戏两套字形发射函数：

1. 用字体 context 的字号和父矩阵决定实际显示字号，选择原生 8/10/12/16px 字模，
   只做整数倍放大。例：16.5px 选 16×1，21px 选 10×2，32px 选 16×2；缺字时只从
   真正存在的字模中选，不生成方框。
2. 保留原逻辑排版记录，在绘制副本上按基线放置字模。静态轴对齐文字直接对齐屏幕
   整数坐标；UV 覆盖完整字模，不再用 15 个纹素撑满 16px。
3. 把原游戏最大显示预设和 VI/Binder 回复统一为 1280×720。实测三张显示缓冲区
   因此变成 720p，UI 到显示缓冲区由 2/3 缩小变成 1:1。
4. 960×540 原样放入 960×544，上下各留 2px，避免最后一次 540→544 插值。

原字、范围、guard、ABI、GFD secondary 目录格式和线程副本寿命见
[ADR 0034](decisions/0034-native-font-screen-grid.md)。Alpha8 的 TEX `0x2c`、原 rTexture
身份登记、真实一字节解码/上传、生命周期和完整 raw 比较继续沿用 ADR 0033。

## 字模与内存

扫描范围仍是 14511 ARC、96 个嵌入 GFD、14 个独立 GFD；95 个普通嵌入字库加 14 个
独立副本使用新目录，另保留两份已有系统字体单通道资产，共 111 个替换文件。
有颜色的 `button_festa` 和 `fontIcon` 保留。系统固定网格 debug 字体没有接入这两个
GFD 发射函数，不能把普通对话的通过当作它们的原生字号验收。

| 常用字库的源字模 | 有效字符覆盖 / 21450 | 缺字 |
|---|---:|---:|
| Fusion Pixel 8px | 15120 | 6330 |
| Fusion Pixel 10px | 11053 | 10397 |
| Fusion Pixel 12px | 19890 | 1560 |
| Unifont 16px | 21450 | 0 |

原 primary 记录总数 21478，另 28 条原本零面积，保持为空。两种 boss 字库的 61/181
个字符在四种源字号均覆盖。所有语言副本都重建并检查；这不等于逐屏验证了七种语言。
清单逐项保存缺字、输入/输出 SHA。常用字和标题的小字号来自固定版本
[Fusion Pixel 2026.10.25](https://github.com/TakWolf/fusion-pixel-font/releases/tag/2026.10.25)，
16px 来自 [Unifont 18.0.01](https://unifoundry.com/unifont/index.html)。包携带来源许可和版权。

| 唯一字库 | 原基础 decoded RGBA8 | 本模式 Alpha8 | 差额 |
|---|---:|---:|---:|
| font_festa | 40 MiB | 11 MiB | 29 MiB |
| 西文 boss_demo | 16 MiB | 1 MiB | 15 MiB |
| 多语言 boss_demo | 24 MiB | 1 MiB | 23 MiB |

常用字库仍是 11 张 1024² Alpha8，多种小字模一起打包，并不为每种字号保留整套空页。
新增 secondary 字模目录约 1.64 MiB/常用 GFD 副本，不能把纹理的 29 MiB 差额说成
整个进程净省。三种字库也不一定同时驻留；固定堆、raw/decoded 池不会自动缩小。
两份系统字体仍合计增加 40 KiB，和上一轮 Alpha8 验证相同。

## 验证

- L0：锁定 main 输入 SHA 和 decoded SHA 不变。四处发射/UV hook 的原字、64 字节
  guard 与直接分支检查通过；两条显示预设指令和 128 字节 guard 检查通过。
- L1：Windows 60 项宿主 CTest 通过。包含字模原始字节、BDF baseline/非整字节宽度、
  缺字与页面边界、GFD 布局保留；还包含 15→16 纹素拉伸和 1080 缓冲区配 720 视口
  的明确失败对照。
- L2：VitaSDK 构建通过，包与 SELF 的实际哈希见对应运行 `process.json`。
- L3：`build/runs/native-font-game-preset-20261106` 的 101 个字形四边形均为 1:1；
  最大坐标误差约 0.000014px。三张 VI 缓冲区的最终 copy 均为 1:1。
  不是只在字形中间阶段测到整数坐标。
- L3 失败对照：`native-font-baseline-lineage-20261106` 的 79 个采样字形未满足原生
  网格；三张显示缓冲区 copy 均为 0.666667，检查器实际返回失败，没有修改通过条件。
- L3：960×544 实际 client 下，同一句村长对话的旧/新截图可比；新图完整显示点阵笔画。
  `font-before-after-3x.png` 只按整数倍放大实际截图，没有重新渲染字或做锐化。

原 Alpha8 1024² 仍实际绑定 1048576 字节；没有扩回 RGBA。
诊断反例与旧包对照保留在忽略的 `build/runs/`。运行使用受保护测试存档，结束后恢复并
逐项核验原存档、marker、eboot 和模拟器配置。诊断过程中每个字符的额外 bridge
开销不作为交付性能计时。

最终正常模式目录为 `build/runs/native-font-final-20261106`，关闭 font trace，实际
只安装六处生命周期和四处绘制 hook，未安装查字/取页诊断。220/280 秒 client 都为
960×544，村庄姓名/正文清楚，字体相关拒绝为 0；正常退出且恢复核验通过。
对应 VPK SHA256 `c2c9dabacb7f143dd7b038a1d3417ae54c61b965790aadca3c2ed7242bec82a3`，
SELF SHA256 `131d65db72affa223a19f338fb40e4c70fc32d4e3c7a0dfc29414af0a00285af`。
此前完整 420 秒的无诊断复测 `native-font-production-20261106` 同样通过。

**没有正式同场景交替 FPS 对照，因此不报提速比例。** 更清楚的整帧和文字不能替代
计时。L4 真机、L5 完整玩法、所有字号的屏幕逐字、boss 演出、旋转/缩放动画、竖排
文字、长时间场景切换和完整播片仍未验证。

## 重建与使用

先校验锁定 ExeFS/NACP；`$tree` 必须是完整合并后的 RomFS，不是 update 增量目录。
工具需要项目已有 Python、lz4 和 Pillow，无额外安装项。生成物都放在忽略目录。

```powershell
$py = $env:MHGU_PYTHON
$tree = 'build/vita3k-pref/ux0/data/mhgu-vita-port/romfs-tree'
$fusion = 'build/fonts/fusion-pixel-2026.10.25'
$unifont = 'build/fonts/unifont-18.0.01/unifont-18.0.01.hex.gz'
& $py tools/assets/fetch_pixel_font.py --output build/fonts/unifont-18.0.01
& $py tools/assets/fetch_native_fonts.py --output $fusion
& $py tools/assets/build_pixel_fonts.py --tree $tree --unifont $unifont `
  --native-font "$fusion/8/fusion-pixel-8px-monospaced-zh_hans.bdf" `
  --native-font "$fusion/10/fusion-pixel-10px-monospaced-zh_hans.bdf" `
  --native-font "$fusion/12/fusion-pixel-12px-monospaced-zh_hans.bdf" `
  --native-font $unifont --native-licenses "$fusion/licenses" `
  --output build/native-font-output
& $py tools/romfs/build_romfs_index.py --tree $tree `
  --overlay build/native-font-output/overlay --base-prefix romfs-tree `
  --overlay-prefix font-native/overlay --output build/native-font-output
```

完整输出目录安装成 `ux0:data/mhgu-vita-port/font-native/`，包含 overlay、index、map、
清单和许可。配套 VPK 启用 `pixel-font.enable`、`pixel-font-native.enable` 和
`gpu-present.enable`，`render-resolution.txt` 为 `540p`，重启生效。
不支持在进程内热换字号目录或显示模式。停用 native marker 后，原 font-pixel 路径仍可
作为对照；停用两个字体 marker 恢复原字体与原显示预设。原资源树不覆盖。

可复核命令：

```powershell
& $py tools/engine/audit_font_hooks.py --exefs $env:MHGU_EXEFS `
  --output build/font-hook-audit.json
& $py tools/assets/check_native_font_draws.py --capture-dir path/to/draws `
  --shader-index path/to/shader-index.json --require-present-copy `
  --output build/font-pixel-proof.json
```

第二条依赖 `pixel-font-trace.enable` 的有界真实顶点/uniform/后续 copy 捕获。正常游玩
关闭该 marker；否则原查字、取页和诊断日志也会进入 bridge，影响性能。
捕获前把上次的 `font-probe-*` 移入旧证据目录，结束后将本次文件一起归档到新目录。
检查器拒绝混入旧尾部数据的长度不符文件。
