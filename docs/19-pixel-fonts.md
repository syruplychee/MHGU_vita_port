# 点阵字体与 Alpha8 结果

2026-11-05。代码/离线验证 L0–L2；Vita3K 中文村长对话 L3；真机 L4 和完整玩法 L5
未验证。用户选择“保持原来的视觉大小，只改成清晰点阵”。

## 已交付范围

- 扫描 14511 个 ARC、96 处嵌入 GFD 和 14 个独立 GFD。
- 重制 95 处普通嵌入字库、14 个独立字库及两份固定网格系统字体，共 111 个改动文件。
- 普通常用字库 21478 条记录；两种登场标题字库分别 61、181 条。覆盖全部七种语言
  的对应资源副本；保留字符顺序、原 advance/行高/逻辑字号/未知标志。
- 字形来自固定版本 GNU Unifont 18.0.01，以 540p 的 3/4 cell 网格生成二值遮罩。
  显示墨迹会随点阵字形改变，布局度量不变，不声称每个字的墨迹轮廓与原字体一致。
- 系统字体 `sysfont_AM_NOMIP` 的 8×16 网格和 `detail_sysfont_AM_NOMIP` 的 16×32
  网格，替换 ASCII U+0020–007E；额外游戏符号保留原遮罩并二值化。
- `button_festa` 是含颜色的按钮字体/图标，`fontIcon` 是 III 图标容器；保留颜色，
  不把它们错误转成 Alpha8。原字库五个 E810–E814 私用槽原本零面积，继续为空，
  不用方块或无关 Unicode 图形补齐。

## 实际存储

以下是三种唯一普通字库的基础 decoded 表示，不是整个游戏的内存峰值：

| 字库 | 旧页数 / 基础像素 | 新页数 / 基础像素 | 减少 |
|---|---:|---:|---:|
| 常用 font_festa | 10 / 40 MiB RGBA8 | 11 / 11 MiB Alpha8 | 72.5% |
| 西文 boss_demo | 1 / 16 MiB RGBA8 | 2 / 2 MiB Alpha8 | 87.5% |
| 多语言 boss_demo | 6 / 24 MiB RGBA8 | 8 / 8 MiB Alpha8 | 66.7% |

重排使用完整 cell 和至少一像素 padding，因此页数增加，但每像素从四字节变一字节。
常用字库的完整源 mip payload 从 55941120 B 降到单 mip 11534336 B。两份系统字体
原来是 BC4，新 R8 合计 81920 B，相比原 BC4 的 40960 B 增加 40 KiB，换取重制后的
无损二值字形；没有把它们算作内存节省。111 个改动文件包含未改动的其他 ARC 条目，
合计文件体积 73173943 → 29057202 B，不能把该差额当作驻留内存收益。

## 运行证据和失败对照

普通字体用 TEX `0x12` 的首次尝试使对话文字消失。相同新 GFD/字形的 RGBA8 对照
能显示，证明不能把问题直接归因于字形重排。随后选用原游戏 `0x2c` 创建路径，实际
单通道文字恢复。只改对象格式标记的探针无效，已移除；没有把它当作实现。
两个格式在原映射表中都得到物理 R8，但原游戏推导的附加标志不同。最终实现采用
已验证的 `0x2c` 契约，并在真正绑定时检查 R8 UNORM 和二维布局；没有关闭失败路径。
具体格式/ABI/guard 与剩余根因边界见 [ADR 0033](decisions/0033-pixel-fonts-and-alpha8.md)。

实际运行目录（忽略的本地证据）：

- `build/runs/pixel-font-rgba-20261105`：RGBA8 定位对照，中文字可见。
- `build/runs/pixel-font-r8-alt-20261105`：`0x2c` 单通道路径，中文字恢复。
- `build/runs/pixel-font-final-20261105`：最终 Alpha8/R8 和两份系统字体加载验证。
  280/380 秒窗口图可见姓名、中文对话和原按钮。系统字体两份均以正确尺寸和 `0x12`
  成功创建；未单独触发其 debug 字符绘制，不能冒充系统字体逐字 L3 验收。

最终运行记录 `PIXEL_FONT_BIND`：原 TIC `7012491d`，有效 `2ffa491d`，upload `0x41d`，
每页 `1048576` B。`VP_GXM_MASK8_BIND` 证明 GXM primary 实际使用同一一字节大小，
不是只压缩磁盘后又扩回 RGBA。也观察到规范 R8 `0x51d` 的 960×540 backing 为
518400 B。字体资源/视图拒绝和 mask8 layout 拒绝均为 0；原有非字体深度/资源拒绝仍
有记录，不宣称所有渲染路径零错误。存档、marker、原 eboot 和模拟器配置均恢复核验。

L2 构建 `a97d076aea1a84a69497e93af32c34a98e41bdccfc0217bde1afaa9801fedb5f`（VPK），
SELF `325ada791782f07b0a08447cb92a0ac150119ea67b509f6625ef022eb1333c66`。
Windows 56 项宿主 CTest 通过，字体工具另有 5 组内部检查；C Release 测试保留 assert。

本轮窗口大小/显示过滤曾变化，且新增了规范 R8 上传路径；没有按正式同场景交替窗口
测 FPS，不报加速百分比。字体贴图为 point，模拟器最终窗口仍可能再缩放整帧；判断
笔画应优先看原生显示大小。尚未做全部字形的屏幕逐字核对、boss 演出/任务全流程、
长时间场景切换、真机映射/缓存/带宽验证。

## 重新生成与启用

安装 `tools/requirements.txt` 后，在仓库根目录执行；`$tree` 必须是完整合并的 1.4.0
RomFS 树，不能直接使用只含增量文件的 update 目录。先按项目流程校验锁定 ExeFS/NACP。

```powershell
$py = $env:MHGU_PYTHON
$tree = 'build/vita3k-pref/ux0/data/mhgu-vita-port/romfs-tree'
& $py tools/assets/fetch_pixel_font.py --output build/fonts/unifont-18.0.01
& $py tools/assets/build_pixel_fonts.py --tree $tree `
  --unifont build/fonts/unifont-18.0.01/unifont-18.0.01.hex.gz `
  --output build/pixel-font-output
& $py tools/romfs/build_romfs_index.py --tree $tree `
  --overlay build/pixel-font-output/overlay --base-prefix romfs-tree `
  --overlay-prefix font-pixel/overlay --output build/pixel-font-output
```

生成器拒绝复用已有输出目录；清单含每个输入/输出 SHA、字库与页面 SHA、覆盖统计、
保留的空槽、排除的图标和许可。固定 Unifont HEX SHA256 为
`e66385c79a0b8b24a466f3129930e08a966a935b4bf3b28c6bb17a9df9bf791d`。
来源与许可见 [官方 Unifont](https://unifoundry.com/unifont/index.html)。

把完整输出目录作为 `ux0:data/mhgu-vita-port/font-pixel/`，保留其 `overlay/`、index、map、
清单和许可文件，配套使用包含本改动的 VPK。在数据根目录创建 `pixel-font.enable`
并重启即可启用；删除该 marker 并重启恢复原字体。不要覆盖原 `romfs-tree/` 或复用
旧 index/map。缺包会明确报错。稳定安装没有被测试流程永久覆盖；本地已留好可试用包。
