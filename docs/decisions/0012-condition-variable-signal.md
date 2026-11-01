# 0012：条件变量只唤醒当前等待者

日期：2026-11-01。状态：实现；L1/L2 通过；L3 单次运行未见 fatal，完整并发与真机待验证。

## 背景与证据

原 `key_pending` 表会在没有等待者时保存信号次数，未来的 Wait 消耗一次并立即
成功。这把条件变量变成了可累计的信号量。播放过程有大量条件变量调用，但这个
语义错误是不是当前播片慢的原因，尚未由运行对照证明。

锁定 SDK 的 `InternalConditionVariableImplByHorizon` 直接进入 SVC，没有用户态
信号次数表。以下偏移相对 SDK 模块起点，均为 A32：

| 范围/位置 | 原始行为 |
|---|---|
| `0xa3074`、`0xa3080` | 构造/初始化把条件变量 word 清零 |
| `0xa308c` | Signal 传 count=1，尾调 `0xc07e8` |
| `0xa3094` | Broadcast 传 count=-1，尾调 `0xc07e8` |
| `0xa309c` | Wait 把条件变量地址作为 key，进入 `0xc07d0`；非零返回触发原断言 |
| `0xa3108` | TimedWait 调同一 SVC；超时结果 `0xea01` 后在 `0xa3180` 重入临界区 |
| `0xc07dc`、`0xc07e8` | 原字分别为 `0xef00001c`、`0xef00001d` |

身份来自 `config/target-1.4.0.json`：SDK NSO SHA256
`9a9b7035266637ee693a32da6b0fde54bda00e897bef05b1c8d8cda30f8ac8b0`，Build ID
`cb8e7f9a74a4a854ff013489ecc1e8b0def23a75000000000000000000000000`。
归档 ELF 的整个 `.text` 已与锁定 NSO 解压结果逐字节核对，长度 4,548,468，SHA256
`cad06f3b74b2c1de5828d5dbc1d7cb7332ca9a9d54d8a1b942580c68ac92dc35`。
`[0xa3074,0xa31c4)` 的 336 字节 SHA256 是
`c2a0e83a0cc3c107b2f6202225b25491a8ed02440ad3c6fa1f5a980c4176b60e`；
`[0xc07d0,0xc07f0)` 的 32 字节 SHA256 是
`ff16a2e6381bd22e0f5f55cd0feda433591a00816ad3a683aa076763c028ab64`。
本次不新增或改变二进制补丁位置、客体 ABI、客体栈布局。

研究对照为 [Atmosphere 的条件变量实现](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_condition_variable.cpp)
（2026-11-01 阅读）。它的 Signal 只处理当前等待树，树中该 key 已无等待者时清
has-waiter word；Wait 在调度锁内置该 word、释放 mutex 并登记等待者。它是第三方
内核实现，不能当作任天堂官方内核的同等证明。本项目只据此交叉核对平台语义，
没有复制该实现代码。

## 决定

- 删除 `KeyPending` 表、增加/消耗 token 的函数及所有字段。不提供旧错误语义的
  marker；运行对照使用保留的旧 SELF，避免生产代码继续维护错误分支。
- Wait 在 Session 锁内登记当前等待者，再完整释放本线程的临界区持有。释放成功
  后把已校验可写的 key word 置 1，再释放 Session 并进入主机事件等待。
- Signal 同样在 Session 内，只处理该 key 当前 `used && !signaled` 的等待者。
  正数 count 限制本次唤醒数；非正数唤醒当前全部等待者。没有等待者时没有未来
  唤醒次数，key word 置 0。
- 主机 SetEventFlag 成功后才置 `signaled`，失败原样报错，未成功唤醒的等待者
  保持可被之后的信号选中。Signal 后尚有未唤等待者则 key 为 1，否则为 0。
- Wait 和 Signal 写 key 前都检查当前内存权限；Wait 的检查和创建事件失败发生在
  释放 mutex 之前。拒绝时不写 key、不改变已有递归持有。
- 一个已登记等待者的事件可以保存“登记后、内核等待前”到来的唤醒。登记和释放
  由 Session 串行化，Signal 进入时只能看见整个登记状态，不需要补记更早的信号。

保留原宿主等待模型的超时处理：成功后重新取得一次主机 mutex 并恢复完整客体
递归深度；超时保持释放状态，由原 SDK 重入。超时后的 key word 可以暂时为 1，
直到 Signal 核对当前等待表后清零；它是保守的“可能有等待者”提示，绝非成功
凭证。等待表仍为 32 槽，不引入无界分配，也不改等待超时值或电影时钟。

## 验证与边界

`tests/sdk_lock_regression.py` 从当前生产源文件提取 Wait、Signal、锁辅助函数和
锁服务分支，再配主机 mutex/event 模拟，生成 Windows/MSVC 工程。原 15 组检查
保留，新增 6 组覆盖旧信号无 credit、登记到内核等待的唤醒间隙、count/Broadcast
隔离、key flag、主机事件真实错误、权限和创建事件失败。该模拟检查控制流程与
记账，不能证明真实内核调度、等待优先级或完整 Horizon 锁 owner 交接。

```powershell
uv run --python build/python-env/Scripts/python.exe python tests/sdk_lock_regression.py --output build/sdk-key-check
cmake -S build/sdk-key-check -B build/sdk-key-check/build
cmake --build build/sdk-key-check/build --config Release
ctest --test-dir build/sdk-key-check/build -C Release --output-on-failure
```

L0：锁定 SDK 反汇编、Session 原子登记顺序与研究对照。
L1：生成工程 CTest 1/1，通过 21 组内部断言检查。
L2：完整 VitaSDK 构建、链接与 VPK 包装通过。
L3：本次修正的 Vita3K 独立对照待主代理运行；不能宣称本修正已提高播片速率。
L4/L5：真机与完整玩法未验证。
