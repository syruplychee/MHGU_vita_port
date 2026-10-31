# 输入身份锁

权威文件：`config/target-1.4.0.json`。任何构建/校验都必须以它为准。

## 1. 程序标识

| 项 | 值 |
|---|---|
| Profile | `update-1.4.0` |
| Program ID | `0100770008DD8000` |
| Patch ID | `0100770008DD8800` |
| Display version | `1.4.0` |
| Architecture | AArch32 |
| CNMT version | `262144` |

## 2. 四模块

| 模块 | 大小（字节） | SHA256 | Build ID |
|---|---:|---|---|
| `rtld` | 7,004 | `5352efcb0c4d0e023fe76b78155aa12ca093087096bbc1a1e6db14efbbb6204f` | `e74a8d1013372a3a1088ab5458807fb7f87925d1000000000000000000000000` |
| `main` | 14,291,042 | `b3935415499549adaf4c0e89822831daa175cfd456a7990791c79be3997645ef` | `fb08f1d20fd1204fe7070672777f869b00000000000000000000000000000000` |
| `subsdk0` | 3,276,847 | `75fb812705612ec68cd0760640183a140a32d4d07181a40355cc002c52096245` | `c2774fa4cace9ecc6a712bfc839214c19515f36e000000000000000000000000` |
| `sdk` | 4,139,430 | `9a9b7035266637ee693a32da6b0fde54bda00e897bef05b1c8d8cda30f8ac8b0` | `cb8e7f9a74a4a854ff013489ecc1e8b0def23a75000000000000000000000000` |

## 3. 元数据

| 文件 | 大小 | SHA256 |
|---|---:|---|
| `main.npdm` | 1,476 | `bbd9ecdfc0d9787d4b9c9500f88c43eb5176ec357ac3ff59e140f18b8ae09d93` |
| `control.nacp` | 16,384 | `256e00a473af6df8f628bdce943bfdf975184b48b3f2456c36ce9f9c24737083` |

## 4. 相关锁定文件

| 文件 | 作用 |
|---|---|
| `config/patch-plans/vp022-sdk-dso.json` | SDK DSO 初始化边界 |
| `config/patch-plans/vp022-sdk-svc.json` | 56 个 SVC 站点 |
| `config/patch-plans/vp022-sdk-timer.json` | 32 个计时站点 |
| `config/patch-plans/vp022-sdk-barrier.json` | 12 个 legacy barrier |
| `config/patch-plans/vp020-1.4.0-archive.json` | TLS 站点与模块档案 |
| `config/graphics/shader-toolchain.json` | 离线着色器工具链 |
| `config/kubridge-api-lock.json` | kubridge API 事实（可选） |

## 5. 校验范围

`tools/input/verify_input.py` 检查：

- 四模块大小与 SHA256
- NSO 魔数
- Build ID
- NPDM 魔数与 AArch64 标志
- NACP 长度与 display version
- ExeFS 中没有额外文件

它**不**检查：

- RomFS 完整树
- 游戏可运行性
- 画面、音频、存档
- 真机行为

这些必须由后续层级验证。
