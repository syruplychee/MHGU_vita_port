# tools/shader

离线着色器流水线。Vita 运行时不执行这些脚本；它们在 PC 上生成静态包。

## 核心流程

```text
SPK/捕获
  → audit_static_library.py        建立 catalogue/Cg 候选
  → compile_static_candidates.py   编译 GXP
  → pack_static_shader_package.py  建立 VPSHIX1 索引
  → augment_static_shader_package.py 增补/替换 variant
  → build_resource_layout.py       生成 VPSRES1 sidecar
  → verify_static_package.py       完整性校验
```

辅助脚本：

- `build_frontend.py`、`shader_ir.py`、`maxwell_glsl.cpp`、`translate_captured.py`：
  Maxwell/SPH → GLSL/IR → Cg。
- `compile_shadow_candidates.py`、`shadow_helpers.py`、`shadow_reference.py`：
  软件阴影 stage 生成/校验。
- `setup_shacc.py`、`convert_cache.py`：Shacc 配置与编译缓存。
- `compare_static_shader_packages.py`、`audit_static_library.py`：包/库审计。
- 其余 `audit_*`、`verify_*`、`build_*`：历史批次/专项验证的支撑脚本。

## 依赖

- 游戏 SPK shader 库与 Maxwell/SPH 审计输出（本地）
- Shacc + SceShaccCg（版本/哈希见 `config/graphics/shader-toolchain.json`）
- 既有基础包（当前包是在基础包上增补，不是从零一键生成）
- Python 3.10+（标准库为主）

## 注意

- 这些脚本的输出是**游戏派生数据**，不得提交/公开分发。
- 编译成功、包校验通过不等于 GXM 链接/像素/性能通过。
- 不要手改 GXP/索引/侧车二进制；用工具重建并记录身份。
- 旧工作区有更详细的批次报告；本仓库只保留可复用的工具和格式说明。

格式见 [静态着色器包](../../docs/reference/shader-package.md)。
