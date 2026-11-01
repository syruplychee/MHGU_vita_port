# 0014：框架级原生播片

日期：2026-11-01。状态：实现；L2 构建通过，L3 完整片头 176.085 秒、5278 帧、
29.974 fps；音频、其他影片、真机 L4 未验证。

## 背景与范围

[0001](0001-native-execution.md) 的一般约束是保留原始 AArch32 游戏逻辑，只替换
平台接口。片头在 Vita3K 中的原 SDK 多媒体管线仍约为每秒 8.6 次解码/8.18 次合成；
[0009](0009-native-video-readback.md)–[0013](0013-session-user-gate.md) 已排除平台
读回、锁和 Session 等待是主要成本，[0011](0011-movie-color-conversion-probe.md)
也证明 `NnCodecVideoSink` 的两个颜色转换函数不在当前路径。继续追查 guest
NvMMLite/NVOMX 调度无法在合理范围内产生 30 fps。

本 ADR 记录一次**明确限定的例外**：仅对全屏影片文件采用框架级原生播放。游戏的
状态机、输入、存档、资源加载和其他逻辑不改；原生播放结束时通过游戏自己的
`uMovie` 停止命令恢复原流程。选择该路线是用户在本轮明确要求的；它不改变 0001
对其他路径的约束。

## 输入与启用条件

- 只接受 `config/target-1.4.0.json` 锁定的四个 NSO；加载器仍在身份不符时停止。
- 原生播放不把任何游戏影片、帧、音频或派生数据写入仓库或 VPK。运行时从用户
  自己的 `ux0:data/mhgu-vita-port/romfs-tree` 读取文件。
- 默认关闭。启动前存在 `ux0:data/mhgu-vita-port/native-movie.enable` 才启用。
  删除 marker 并重启即回到原 guest 管线；两种路径不共享解码状态。
- 仅处理视频轨为 H.264 `avc1`、容器为 ISO BMFF/MP4 且尺寸不超过既有
  `media_video.c` 上限的影片。解析、路径、内存或解码任一失败都记录
  `VP_NATIVE_MOVIE_FAIL` 并回到 guest 路径，不伪造成功。

## 实现

新增 `src/platform/native_movie.c`。触发点是原 NVDEC 桥第一次成功解码
`frame=0`：此时 `media_video.c` 已创建 SceAvcdec 解码器，且 ROM 树读取已记录
`nativeNX/movie/*.mp4` 路径。原生线程执行以下步骤：

1. 从 ROM 树路径打开 MP4，解析 `moov`；只选 `vide` 轨，读取 `stsd/avcC`、
   `stsz`、`stco/co64`、`stsc`、`stts` 和 `stss`，生成样本偏移、大小和 Annex B
   输入。每个样本带 `avcC` 的 SPS/PPS。
2. 通过 `vp_media_video_native_decode` 复用 guest 桥已创建的同一个
   `SceAvcdecCtrl` 和输出缓冲，不另开第二个解码器，避免硬件解码器状态冲突。
   返回 NV12 packed raster、coded height 和可见高度。
3. 在独立宿主线程把 NV12 转成 `SCE_DISPLAY_PIXELFORMAT_A8B8G8R8`，按影片时长
   和样本数计算周期（本片 29.97 Hz），用 `sceDisplaySetFrameBuf(NEXTFRAME)`
   双缓冲呈现。显示尺寸固定 960×544；源 960×540 最近邻缩放。
4. 激活期间，`gxm_runtime.c` 的 guest present 仍完成 Scene 提交但跳过拷贝/呈现；
   `audio_sink.c` 把 guest 音频块清零。原生路径当前**没有音频**，因此影片无声，
   避免原慢速音频与原生画面错位。
5. 播放结束后向 `uMovie` 线程对象的命令字写入 8（原停止/清理命令），等待其状态
   回到 9（idle）后再撤下原生呈现。该等待避免旧 NVDEC 线程把 P 帧继续送入已经
   被原生流推进的同一解码器。命令 8 是游戏自己的停止路径，不是伪造的完成标志。
6. 任何中途失败都清除 active/presenting，让 guest 管线继续；不会写停止命令。

`native-movie.limit-ms` 只用于有界诊断运行；正式结果不依赖它。

## 证据

最终完整运行（Vita3K v0.2.1 4106-d7edcbe9，OpenGL，540p）：

- VPK SHA256 `7c78767bf25f2257ccf95b465806b07e932177732bcb1e70dffa09fc895e2699`；
  SELF/eboot `be28aaaf3a1706a5a15dbb3a6b4526d465a36849f9e3f9d8feb885211d577dc2`。
- 影片 `nativeNX/movie/movie06.mp4`，960×540，5280 个视频样本，时长 176.176 秒。
- `VP_NATIVE_MOVIE_DONE decoded=5280 presented=5278 elapsed_us=176085450
  rate_milli=29974 failed=0`。
- 截图 t=60、120、180 秒显示正常影片画面，t=225 秒显示游戏标题界面。
- 此前用同一停止命令做的强制停止对照（不启用原生播放）在 t=50 秒已进入标题
  界面，证明命令 8 是该游戏自己的退出路径。
- 宿主回归：28/28 CTest、17/17 Python 工具测试、SDK 锁/条件变量模拟 1/1。
- 移除 marker 的对照运行在 70 秒内记录 360 次原 guest 解码，回退路径仍工作。
- 失败回退、marker 关闭路径和原 guest 慢速播放路径仍保留。

## 未验证与风险

- **音频未实现**：原生播放期间 guest 音频被静音，影片无声。AAC 解码、音画同步
  和 AudioOut 缓冲是下一阶段。
- **真机未验证（L4 未达成）**：960×544 双缓冲显示内存使用
  `USER_RW_UNCACHE` 内存块；真机是否接受、SceAvcdec 与 guest NVDEC 共享解码器、
  4 MB 常驻显示缓冲的内存预算、CPU NV12→RGBA 成本都未在 Vita 上测过。
- 只对 movie06 做了完整 L3 验证；其他影片、循环、过场和片头后的其他影片没有
  逐一验证。原生路径适用于所有被识别为全屏 MP4 的影片；若影片带游戏 UI 叠加，
  guest present 被抑制会隐藏 UI，这是已知限制。
- 显示缓冲在成功呈现后保留 4 MB 不释放，避免在显示仍扫描时释放；当前接受该
  常驻成本。若显示内存分配失败，原生路径失败并回退 guest。
- 原生视频与 guest 慢速管线同时存在；停止命令后 guest 会启动新的解码器/影片。
  本片完整运行没有崩溃，但长时间循环、快速切换影片和退出竞态仍需覆盖。
- L3 结果不能外推到真机；Vita3K 的 SceAvcdec/显示实现与硬件不同。

## 回滚

移除 `native-movie.enable` 即恢复原路径。`native-movie.limit-ms` 只影响诊断
运行长度，不改变解码/呈现语义。原 `media_video.c` 的 guest NVDEC 解码、合成、
读回和颜色探针均保留。
