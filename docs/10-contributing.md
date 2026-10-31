# 贡献指南

## 1. 基本约定

- 回复与项目文档使用中文；代码、注释、提交信息使用英文。
- 一次提交只做一件可解释的事；避免把格式化、重构和行为修改混在一起。
- 不提交游戏文件、密钥、固件、存档、日志、截图或生成二进制。
- 不修改 `config/target-1.4.0.json` 来“适配”其他版本。
- 不绕过身份校验、断言、失败路径或证据要求来让测试变绿。

## 2. 分支与提交

```text
main                 保护分支，始终可构建、宿主测试通过
feat/<topic>         新功能
fix/<topic>          缺陷修复
perf/<topic>         性能优化（必须带测量方案）
docs/<topic>         文档
```

提交信息用英文，建议格式：

```text
<area>: <imperative summary>

Why:
- ...

What:
- ...

Evidence:
- cmd/result/scope
```

示例：

```text
gxm: raise fragment sampler limit to five

Why:
- quest stages declare tex0..tex4.

What:
- extend the static sampler slot table and runtime limit.

Evidence:
- host sampler tests pass; VPK builds; runtime quest verification pending.
```

不要写“优化性能”“修 bug”这类无法审查的提交。

## 3. 代码风格

- C11，`-Wall -Wextra -Werror`（Vita 目标）。
- 4 空格缩进，LF，文件末尾换行。
- 函数/变量使用现有命名风格（`vp_` 前缀，snake_case）。
- 新增纯逻辑优先放头文件/独立单元，便于宿主测试。
- 不引入异常、RTTI、动态分配热路径，除非有明确理由和预算。
- 不复制第三方代码；确需复用时先审阅许可证和依赖，并在 ADR 记录。

## 4. 证据要求

每个行为修改至少给出：

- 修改前问题与复现条件
- 修改点（文件/函数）
- 验证命令与结果
- 证据级别（L0–L5，见 [测试](08-testing.md)）
- 未验证范围

示例：

```text
Evidence:
- host: `scripts/host-tests.ps1` 26/26 pass (Windows, MSVC 19.51).
- build: `scripts/build.ps1` VPK ca...; SELF d6...
- Vita3K: not run; visual result unverified.
```

不要把“编译通过”写成“运行正常”。

## 5. PR 检查清单

- [ ] 只改相关文件，没有顺手重构
- [ ] `scripts/host-tests.ps1` 通过
- [ ] `scripts/build.ps1` 通过（涉及 Vita 代码时）
- [ ] 输入身份检查通过（涉及锁/补丁/SDK 契约时）
- [ ] 新逻辑有宿主测试或有明确理由说明为何不能
- [ ] 没有提交游戏数据/密钥/存档/生成物
- [ ] 文档/ADR 已更新
- [ ] 证据范围写清楚，未夸大

## 6. ADR 流程

影响架构、契约、数据格式、证据规则的决策写 ADR：

```text
docs/decisions/NNNN-short-title.md
```

内容：

1. 背景/问题
2. 决定
3. 后果（正面/负面）
4. 替代方案
5. 证据/验证范围

编号递增，不修改已生效 ADR 的结论；如需推翻，写新 ADR 并在旧文件加“Superseded by”。

## 7. 目录职责

| 目录 | 可以改 | 注意 |
|---|---|---|
| `src/app/` | 启动链 | 保持日志可诊断 |
| `src/loader/` | NSO/重定位/补丁 | 任何补丁必须绑定原字节 |
| `src/bridge/` | ABI/汇编 | 寄存器/flags/cache 语义必须写清 |
| `src/platform/` | 服务/GXM | 未实现路径明确报错 |
| `src/generated/` | 生成头 | 由工具生成，不手改 |
| `tools/` | 离线/运行工具 | 不改游戏输入 |
| `tests/` | 宿主回归 | 不依赖游戏数据 |
| `docs/` | 共识文档 | 中文；不写流水账 |
| `config/` | 锁/契约 | 变更需 ADR 和身份核对 |

## 8. 安全与合规

- 不提供、不索要、不分享游戏/密钥/固件。
- 不添加绕过 DRM、联网验证或反作弊的功能。
- 不在日志中打印密钥、存档内容或个人信息。
- 第三方工具按其许可证使用；GPL/MPL/OLP 组件不随意再分发。
- 发现安全或合规问题时，先停止并联系维护者，不要公开利用细节。

## 9. 提问与报告

报告问题时附：

- 复现步骤
- 构建/运行环境
- VPK/SELF/eboot/静态包哈希
- 日志关键行与截图
- 期望行为/实际行为
- 已尝试的排查

不要只贴“黑屏”“很卡”，也不要附加整个 1 GB 日志。
