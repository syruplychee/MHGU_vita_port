# ADR 0020：以原 SDK 实际编码和资源对照检查接管

日期：2026-11-04。

## 背景

ADR 0019 的合成检查和真实队列接口对照通过，但 GPU 输出缺失大量场景。接口比较
遗漏参数也会假绿。因此继续执行锁定原始 SDK 编码器，并以运行时 SDK 真正写出的
字节作为第三方参照，不能只比较两套准备代码自己记录的事件。

## 决定

增加默认关闭的编译选项 `VITA_PORT_ENGINE_CAPTURE`。诊断包在第 3000 次公共入口
采集一个队列：资源首次读取页、完整 ABI 参数、实际 SDK 函数指针、64 位查询返回、
command object 前后状态，以及 SDK 追加的命令字节。结束后进入现有显式失败桥，
`rc=-100` 是诊断终止，不是正常玩法通过。容量或文件错误明确报告失败。分配和文件
操作只在 host TLS 下执行；正常构建不包含采集代码。

SDK `32FAF8` 的 BindProgram 从 `program+8` 读取 CPU 命令包指针，`program+10`
是字数，包本身也必须采集。只抓上层对象不够。离线工具不补缺页，不替换地址查询，
使用原 SDK 的 `BufferGetAddress` / `DeviceGetTextureHandle`。外部资源操作、
换块回调或未返回均失败。Unicorn 仅作宿主离线取证，不加入 VPK，不在运行时解释游戏。

格式和边界见 [采集格式](../../tools/engine/reference/capture_format.md)，工具为
`tools/engine/reference/compare_sdk.py`。它核对锁定解码 main / SDK SHA-256，
检查实际函数指针对应同一 SDK base，记录 ELF、输入和捕获文件身份。

## 找到并修正的实际错误

原 main `BBEA10` 为 r2 设置 `0000FFFF`，`BBEA60` 调用
`nvnCommandBufferSetPrimitiveRestart(cmd, 1, 0xFFFF)`。SDK `3329E0..332A48`
保留 r2，将它写为方法 `0x592` 的值。本项目 `guest_svc.c` 读取该方法并传给 GXM
索引处理，`gxm_runtime.c` 用它识别并拆分 restart。此链条由源码和原指令确认（L0）。

接管代码误传 `(cmd, 1, 0)`，原接口对照只比较该调用前两个参数。第一轮真实采集
第 0 个事件立刻暴露差异：12 字节命令包的最后一个 word，原指令写 `0000FFFF`，
候选实际写 `00000000`。这会把正常索引 0 当作 restart，原本的 FFFF 结束标记则
不能按原意处理，足以破坏模型绘制。现在补齐第三个参数，并加入宿主回归和原指令
接口比较（L1）。没有调整阴影、浮点精度或模拟器来掩盖错误。

## 实际编码和资源结果

第一轮 `engine-sdk-capture-20261104`：3816 次接口、1430 次真实查询、76976 字节
命令。原路径与运行时仅在第 0 个 restart 值不同，其他编码一致；321 项资源描述及
program 命令包一致。较长旧文件的尾部被文件服务保留，长度检查正确拒绝；调查副本
截取 header 声明且写入核验成功的前缀，多出尾部与上一份旧文件逐字节相同。该副本
只用于定位，不用它声明最终通过。

修复后用不存在的目标文件重采 `engine-sdk-capture-fixed-20261104`：1803 页、
7881 次接口、3033 次真实查询、158000 字节 GPU 命令。原公共入口→原 SDK、编译的
ARM C→原 SDK、Vita3K 运行时实际输出三者逐字节一致；提交状态 0x1180 和上下文
0x43BB80 的写回也一致（L1，输入采自 L3）。本样本 control 新增字节为 0，不宣称
验证了 control record 的全部种类。

三者命令 SHA-256：
`89c60ed58ff08518b444d3156d205a3904f5be59890e8e0ca5fc6a25e11ac1be`。

571 项清单一致：92 个 program header、92 段命令包、66 个 buffer header、1 个
pool / 1 个 vtable、92 组 attrib / 92 组 stream、20 个 target / view，以及 115 项
状态对象。记录地址、范围和 hash；buffer 额外记录 pool offset、大小和 64 位 GPU
地址。**这是编码所读的资源描述和 program 数据**，不包括未采集的全部顶点、索引和
纹理像素载荷，不将其说成整个显存逐位等价。

可复跑命令（输出目录须不存在，采集文件与 ELF 留在本机忽略目录）：

```powershell
build/python-env/Scripts/python.exe tools/engine/reference/compare_sdk.py `
  --main build/graphics-binary-audit/main.bin `
  --sdk build/graphics-binary-audit/sdk.bin `
  --elf C:/Users/15196/Desktop/workspace/MONSTERHUNTER/build/mhgu-sdk-capture-20261104/vita_port_native_loader `
  --capture build/engine-full/sdk-capture-fixed `
  --function-map build/graphics-binary-audit/nvn-function-map.json `
  --output build/engine-full/sdk-compare-recheck
```

修复后的默认构建 VPK（采集关闭）：
`a1c057e830423a109846fe4e5e8ea514e0b437497aa3b2027e63a4d6a97223ce`；
SELF：`6afc0ebf51a4391ffe8d5f4a09b703912e6dd089a3c1bafc200fddc2e51f2e08`（L2）。
37 项宿主回归和 30 组可复跑原二进制对照通过（L1）。

反向检查：删除 program packet 所在页后，在第 26 个事件明确报缺失地址
`8F92EB40`、读取 100 字节失败；追加旧尾部后明确报页数／文件长度不符（L1）。
没有为缺页映射零页，也没有跳过差异把结果改成成功。

## GPU 最终图像

`engine-restart-fixed-raw-20261104` 使用上述采集关闭的修复包，开启完整接管及
`-LiveCapture -SurfaceReadback`。230 秒请求实际 GPU present 输出，960×544 RGBA
中村庄地面、建筑、村长、玩家、背景和 UI 恢复正常；修复前大面积场景缺失的现象消失
（该对话窗口 L3）。RGBA SHA-256：
`6a859f7a689c46064b65d71e8e05da090f24a6258d2ad750ea7d22aa9785cb84`。
这是最终表面读回，未用正常退出、draw 数量或窗口标题 FPS 代替画面检查。
没有声称与不同动画时刻的旧截图逐像素相同，也没有据此宣称净提速。

运行 302.176 秒自然退出，退出码 0；原始存档、开关和已安装 eboot 恢复核验通过，
私有 `disable-surface-sync` 读回配置也恢复。末次准备统计 calls=3276、failures=0。
全程 GXM 拒绝绘制 86 次（stage3=12、stage9=74），与此前旧稳定包控制运行的总数
及分类相同；这仍是保留的后端缺口，不把采样画面恢复说成所有效果已验收。

未验证：其他队列输入、control 新增路径、外部资源操作、原 SDK 数学库逐位等价、
真机 L4、完整玩法 L5。不凭有限命令一致或正常退出宣称整个画面与性能通过。
