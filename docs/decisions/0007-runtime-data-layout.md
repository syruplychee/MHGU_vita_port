# 0007 稳定运行时数据目录

- 状态：已采纳
- 日期：2026-10-31
- 决策者：项目维护者

## 背景

旧工作区使用带批次编号的目录名（例如 `static-shaders-b118-scenes`），每次实验都
换名，导致 SELF 编译定义、运行脚本、安装目录和文档互相耦合，难以协作。

## 决定

1. 运行时数据根目录固定为 `ux0:data/mhgu-vita-port/`。
2. 静态包使用两个稳定角色：
   - `static-shaders/`：冻结源包；
   - `static-shaders-current/`：当前 SELF 编译定义选中的运行包。
3. 不再把批次号写进默认路径；批次身份放在 `manifest.json` 和构建记录中。
4. 编译目录名由 `VITA_PORT_STATIC_SHADER_PACKAGE_DIR` 控制，默认
   `ux0:data/mhgu-vita-port/static-shaders-current`。
5. 禁止把编译目录设为 `static-shaders` 或 `shaders`，避免覆盖冻结/legacy 包。

## 后果

- 运行脚本、文档、SELF 编译定义一致。
- 切换包只需替换 `static-shaders/` 内容并重新运行 runner，不改路径。
- 需要包 manifest 记录真实来源/哈希，不能靠目录名判断版本。

## 替代方案

- **每批次换目录名**：历史做法，协作成本高。
- **单目录原地覆盖**：无法区分源包和当前包，容易污染。

## 证据

- `CMakeLists.txt` 默认值、`gxm_runtime.c` fallback、`run_gui_play.ps1` 校验。
