# 村庄黑块：原始代码、GXM 与模拟器的证据边界

更新：2026-11-03。状态：**未定位全部黑块的根因，未修复验收**。

本页约束后续排查，避免把“实验改变了画面”误写成“找到了原因”。用户反馈包括地面、
建筑黑块，村长袖子和羊身上的黑斑，以及衣服短暂错位后恢复。这些现象未证明具有同一原因。
当前优先处理正确性，暂不继续合入渲染性能实验。

## 1. 已经确认的事实

### 锁定原始 SDK 的行为（L0）

输入身份检查通过。SDK NSO SHA256：
`9a9b7035266637ee693a32da6b0fde54bda00e897bef05b1c8d8cda30f8ac8b0`。
按项目 NSO 解码器校验段哈希后，解码镜像 SHA256：
`ad41e17d4307eade701bb5c7713b988c41ded1993d0a4a2d5381c472d8d99336`。
以下均为模块相对偏移，不是运行时地址；没有修改原始二进制或 ABI。

通过 SDK 自己的 GetProcAddress 路径恢复了 477 个 NVN 函数的对应关系：名称表
`0x7b2a38`、字符串池 `0x7af7c8`、函数表 `0x861c8c`。初始化函数 `0x3184ac`
把该函数表指针写入 `0x9255d0`，GetProcAddress `0x31a2dc` 在检索前调用它。
不能将这张按名称排序的表与 main 中另一张注册表混为一谈。

| 原接口 / 位置 | 实际行为 | 对移植层的约束 |
|---|---|---|
| `SetPolygonOffsetClamp`，`0x332ce4` | slope 写方法 `0x55b`；units 在 `0x332d30` 经过 `vadd.f32 s0,s18,s18`（字节 `090a39ee`），再写 `0x56f`；clamp 写 `0x61f` | 寄存器中的 units 是原 NVN 参数的两倍；除二可恢复原参数，但不能单凭这点证明 GXM 的单位完全相同 |
| `DepthStencilStateSetDepthFunc`，`0x3220a4` | 深度比较函数写入状态字的 4–7 位 | 不能凭截图更换比较函数 |
| 深度状态发射函数，`0x343330` | 关闭深度测试时发 `0x4b3=0`；启用时另发深度写入与比较状态 | 当前 `depth_test && raw_write` 的有效写入规则有源码依据 |
| `SetDepthRanges`，`0x333414` | depth mode 为 1 时，scale 为 far−near，offset 为 near；其他模式使用半范围转换 | 不能把所有路径硬改成 0.5/0.5 |
| `SamplerBuilderSetCompare`，`0x32153c` | enable 写 builder+`0x24`，compare 写 +`0x28` | 只确认入口参数保存位置，尚未完整闭合 sampler 描述符打包链 |

本地 `black-free-20261102` 日志确实出现过 slope=0、寄存器 units=−1024、传 GXM
units=−512。旧报告中“场景从不使用 depth bias”的说法不能继续沿用。但这些有界日志
尚未与村长袖子对应 draw、目标附件和 shader 对齐，不能认定 bias 就是原因。

### 一个可独立复现的 Vita3K OpenGL 同步错误（L0/L1）

核对本地实现和公开上游的
[fence.cpp](https://github.com/Vita3K/Vita3K/blob/master/vita3k/renderer/src/gl/fence.cpp)：

1. 第一代 fence 完成后，`signaled_` 可能保留 true。`insert()` 创建第二代 GLsync 时没有
   清除此状态；后续等待可能直接删除新 GLsync、返回成功，不查询新任务是否完成。
2. `glClientWaitSync` 先返回 TIMEOUT，随后 `glGetSynciv` 查到 SIGNALED 时，局部返回
   状态仍是 TIMEOUT，因此函数错误返回 false。

这与 [Khronos 的 fence 定义](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glFenceSync.xhtml)
不符：每个新同步对象跟踪本次命令流中的完成条件，上一代完成不能代替新一代完成。
OpenGL ring buffer 在回到缓冲区头部复用空间前调用该等待函数，故该错误有实际数据
生命周期风险。不过“存在风险”和“此次黑块由它造成”是两个不同结论。

可重复测试读取外部真实源码，注入 fake GL 后编译原函数，不在项目中复制第三方实现：

```powershell
python tools/diagnostics/check_vita3k_gl_fence.py <Vita3K源码目录> `
    --out build/fence-check-new `
    --vcvars "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"
```

输出目录必须是新目录。测试源码身份为 SHA256
`45a0f1d24ee2216bcc5739957589b81c6fe6487ef9de878a361780c0f57a2975`。

| 检查 | 原实现 | 最小修正 |
|---|---|---|
| 连续两代都实际等待 | 失败 | 通过 |
| TIMEOUT 后轮询完成返回成功 | 失败 | 通过 |
| 立即完成 | 通过 | 通过 |
| GL_WAIT_FAILED 返回失败 | 通过 | 通过 |
| insert 等待旧任务后，新任务仍须重新等待 | 失败 | 通过 |

最小修正仅为新建同步对象前清除缓存完成状态，以及轮询到 SIGNALED 后记录成功状态。
这是 L1 状态机测试，不模拟真实 GPU 的执行时间，不证明图像改善。诊断模拟器只在
忽略目录构建，未选为默认版本；本仓库不 vendor 或分发修改后的 Vita3K。
主代理另行复核并执行了本仓库的精简测试脚本，得到同样的 2/5 与 5/5 结果；本轮
既有宿主回归 29/29 通过，Python 语法检查通过。没有修改游戏运行时代码，没有重建
正式 VPK；既有构建结果不能记作本轮黑块修复验收。

## 2. 不能再当作修复依据的实验

- **DF32 改用桌面 GL_DEPTH32F_STENCIL8**：候选运行中画面整体变暗、羊和人物黑斑
  加重，用户明确指出回归，已撤回。上游 GL 当前使用 D24，并不能反过来证明它完全
  等同于 Vita 的 DF32；也不能因名称不同就直接改格式。单次改动还没有建立足够的
  同场景重建基线，不能把所有变化精确归于某一个格式细节。
- **把 shadow compare 改成 ALWAYS**：它是全遮蔽结果，不等于关闭阴影。即使强制
  无遮蔽后某处黑块减少，也只说明它受该路径影响，不证明采样、投影或目标内容哪项错。
- **跳过深度预绘制**：改变整个遮挡关系，只能用于定位，不能作为保留画面语义的修复。
- **给位置结果加 precise / invariant**：35 个旧共享顶点的 XY 差异没有消除；把 GLSL
  点积分别按正序、逆序拆成明确乘加也没有改善。均未进入生产实现。
- **原顶点色为零，所以黑块正常**：没有证明后续补光、材质合成和呈现 pass 完整，
  也没有原平台同帧对照，这不足以关闭问题。

旧样本中，原始 GLSL 两遍的 XYZW 全部相同；当前 GXP 经模拟器翻译后，X 有 8/35、
Y 有 2/35 个点不同，最大 clip 差分别为 `0.0001220703125`、`0.00006103515625`，
Z/W 均相同。这里只验证了 35 个位置和这一对 shader；不能将“顶点深度相同”扩展为
“光栅化后每个像素都相同”，也未证明这些顶点就是本次村长袖子或羊的网格。
上述离线桌面 GL 检查记为 L1，不冒充实际 Vita3K 场景或真机结果。

## 3. 本轮运行的限制（L3 尝试，未通过画面验收）

2026-11-03 运行了保留原同步行为的检测版 302.383 秒，以及修正版 122.197 秒。
进程均正常退出，但窗口采集为整片白色，运行器明确拒绝 `no-usable-capture`。
随后对完全恢复的稳定原版运行 67.162 秒，也得到同样的白色采集。因此不能将白屏
归因于 fence 修改，亦不能用这些采集评价黑块修复。原行为检测版未取得
`VP_FENCE_UNSAFE` 样本，修正版未取得预期的 `VP_FENCE_*` 样本；探针覆盖尚未闭合。

三次运行均完成原始存档逐文件哈希恢复和 marker 恢复校验。默认模拟器已恢复为
SHA256 `d21795e11ecfa4ec6f9efee4e727cfcd55ef27b0a0c27ea44aa98b4a9cbc9b6f`；
正式 VPK 保持 SHA256 `72f516f3322dbee0356b5682953ef8a41a93f30749a6ad63ca9c93aa2fbb6249`。
本地证据入口为 `build/runs/fence-baseline-20261103/`、`fence-fixed-smoke-20261103/`、
`fence-stable-smoke-20261103/`，不进入 Git。

## 4. 下一步取证顺序

1. 先恢复有效窗口观测，区分实际显示白屏和采集异常；不能降低运行器的验收门槛。
2. 抓到同一帧的问题部位，确认它在角色着色结束时已错，还是后续阴影/补光/合成后变错。
   区分不同颜色附件，保留呈现转换关系；不能把中间全黑附件当最终黑块。
3. 对实际问题网格同时保存两遍的顶点、索引、骨骼纹理、constant buffer、viewport、
   depth bias 和比较状态，按原始输入和原始 shader 对照。不要只比较几个顶点的 Z/W。
4. 同步错误只有在当前后端实际调用、并抓到新对象未完成而跳过等待后，才提升为当前
   问题的强候选。必要时用独立小型 GXM 程序对照真机与模拟器，无需改游戏逻辑。

默认路径中 uniform 在 `sceGxmDraw` 前拷入已预留的缓冲；批次保存请求时会清空 guest
顶点、索引、纹理和 constant buffer 指针，后续完成处理仅使用元数据。这排除了“批次
完成时才重新读取这些 guest 指针”这一个具体假设；不代表所有并发写入问题均已排除。

## 5. 公开资料能回答到哪里

- [VitaSDK GXM 接口](https://docs.vitasdk.org/group__SceGxmUser.html) 确认 viewport、
  depth bias 参数和深度格式的公开声明；不能据此断言 SGX 的舍入、深度偏移单位与
  桌面 NVIDIA 完全一样。没有找到足以证明本问题相关细节的真机测量资料，L4 仍空缺。
- [Vita3K PR 4248](https://github.com/Vita3K/Vita3K/pull/4248) 报告 Vulkan ring buffer
  过早覆盖导致 3D 顶点/纹理错误，是检查资源复用的线索；它处理的是 Vulkan，不能
  直接套用到当前 OpenGL。
- [Vita3K PR 4175](https://github.com/Vita3K/Vita3K/pull/4175) 涉及 mask bit 与 tile
  裁剪，并说明未做真机测试。不能把其推断当成当前村庄的硬件对照证据。

结案仍需：实际问题网格的因果证据、保持原始语义的修复、有效窗口对照和明确的真机
未验证范围。单个模拟器 bug、一次未复现或宿主测试通过，均不满足黑块任务完成定义。
