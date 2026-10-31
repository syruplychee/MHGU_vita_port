# 静态着色器包格式

## 1. 为什么需要静态包

游戏的原生 Maxwell 着色器不能直接在 Vita GPU 上执行。项目在 PC 端离线：

1. 从锁定 SPK 库提取精确 stage 内容（Maxwell/SPH）。
2. 通过 pinned frontend 转成 GLSL/IR。
3. 生成 Cg，用 Shacc 编译成 GXP。
4. 建立 stage/variant/program 索引和纹理资源 sidecar。
5. Vita 运行时只加载 GXP、创建 GXM 对象、设置动态 uniform/纹理。

Vita 上不运行 Cg/Maxwell 编译器，也不在逐 draw 路径解析大 JSON。

## 2. 包目录

```text
package/
├─ manifest.json
├─ shader-index.bin
├─ shader-index.json
├─ shader-resources.bin
├─ shader-resources.manifest.json
└─ shaders/
   ├─ <variant-key>.gxp
   └─ ...
```

## 3. `VPSHIX1` 索引

固定小端记录，不直接写 C 结构体。

### 3.1 头（64 字节）

| 偏移 | 类型 | 含义 |
|---:|---|---|
| 0 | 8 字节 | `VPSHIX1\0` |
| 8 | u16 | version = 1 |
| 10 | u16 | header size = 64 |
| 12 | u32 | flags = 1 |
| 16 | u32 | stage count |
| 20 | u32 | variant count |
| 24 | u32 | program count |
| 28 | u32 | stage record size = 48 |
| 32 | u32 | variant record size = 40 |
| 36 | u32 | program record size = 16 |
| 40 | u32 | body bytes |
| 44 | u32 | body CRC32 |
| 48 | u32 | ABI version（1 或 2） |

读取器会校验：magic、版本、记录大小、body 长度、CRC、stage/variant/program 闭包、
token 严格递增、variant 的 viewport mask 不相交（ABI 2 允许不同 input signature 重叠）。

### 3.2 Stage（48 字节）

- u32 stage type：1=vertex，5=fragment
- 32 字节 stage SHA256（精确 Maxwell/SPH 内容）
- u16 variant_start
- u16 variant_count
- u32 source flags：SPK/SDK/observed

### 3.3 Variant（40 字节）

- u32 stage index
- u16 viewport mask：bit0=viewport transform 0，bit1=1
- u16 ABI
- 16 字节 key
- u32 GXP bytes
- u32 flags
- u32 GXP CRC32
- u32 input signature（ABI 2：bit N 表示 `in_attrN_uint`）

运行时选择规则：

1. 精确 stage SHA。
2. 匹配当前 viewport transform。
3. 匹配顶点输入签名（UINT 特化）。
4. 无精确特化时使用 generic（signature 0）。

### 3.4 Program（16 字节）

- u32 token（原 SPK index + 1 的候选）
- u16 vertex stage index / 0xffff
- u16 fragment stage index / 0xffff
- u32 flags：observed/static

## 4. `VPSRES1` 资源 sidecar

用于把 shader 声明的纹理槽映射到运行时 TIC：

- 96 字节头，包含 magic、版本、stage/纹理计数、父 index SHA256/CRC。
- 每个 stage 40 字节：stage SHA256、类型、纹理记录范围。
- 每个纹理 16 字节：slot、offset、type、depth、flags/usage。

Usage flags：

- bit0–3：shader 实际读取 `.r/.g/.b/.a`
- bit7：离线分析完成

深度视图只接受 `USAGE_ANALYZED` 且只读 `.r` 的 stage；原生深度格式没有证明 `.g/.b/.a`
映射。

## 5. `manifest.json`

关键字段：

- `abi_version`：2（生产）
- `index_sha256` / `index_json_sha256`
- `candidate_counts`：programs/stages/variants
- `added`：新增 variant 的来源、GXP/Cg 哈希、编译环境、flags
- `limitations`：明确“不是链接/像素/真机证据”

`tools/shader/verify_static_package.py` 会检查包内全部哈希和 GXP，但**不**证明：

- GXM 链接成功
- 实际 draw 使用该 variant
- 像素正确
- 真机性能

## 6. 运行时加载

`gxm_runtime.c`：

1. 打开 `<dir>/shader-index.bin`。
2. 解析/校验整个索引和 `shader-resources.bin`。
3. 按 draw 的 program token/stage SHA/viewport/input signature 查找 GXP。
4. 创建并缓存 GXM program/pipeline。
5. 用 sidecar 绑定纹理；动态 uniform/骨骼每帧更新。

静态包缺失或损坏时，运行时不静默使用默认 shader；它会记录拒绝并走明确的失败路径
（或 legacy 包，如果显式配置）。

## 7. 重建包

见 [运行时资源](../05-runtime-assets.md) 第 4.2 节。当前流水线需要游戏 shader dump、
既有基础包和 Shacc/SceShaccCg；不是完全一键。重建后必须：

```powershell
python tools/shader/verify_static_package.py <package>
```

并把包安装到 `static-shaders/`，由 runner staging 到编译目录。
