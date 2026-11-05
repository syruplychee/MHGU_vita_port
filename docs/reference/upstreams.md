# 上游项目与许可

本仓库只包含项目自行编写的代码与文档。第三方源码/二进制不进入 Git，除非其许可证
允许且维护者明确决定；当前没有 vendored 第三方源码。

| 项目 | 用途 | 许可证/注意 |
|---|---|---|
| [VitaSDK](https://github.com/vitasdk) | ARMv7 交叉编译、SELF/VPK | MIT（以仓库为准）；工具链二进制由开发者自行安装 |
| [Vita3K](https://github.com/Vita3K/Vita3K) | PC 模拟器 | GPL-2.0；本项目不分发 Vita3K；私有兼容候选单独构建，见 [ADR 0016](../decisions/0016-local-vertex-dot-order.md) |
| [vitaGL](https://github.com/Rinnegatamante/vitaGL) | 只读参考（GPU 组织） | MIT；未复制实现 |
| [libvita2d](https://github.com/xerpi/libvita2d) | 只读参考 | MIT；未复制实现 |
| [kubridge](https://github.com/bythos14/kubridge) | 真机内存映射/权限 | MIT（以仓库为准）；插件不随项目分发 |
| [Vita3K-Shacc](https://github.com/shoui520/Vita3K-Shacc) | Cg → GXP | **GPL-2.0**；版本/哈希锁定 |
| [SceShaccCg](https://github.com/AnimMouse/SceShaccCg) | Shacc 运行时 | Sony OLP；**仅本地使用，不得再分发** |
| [shader-compiler-rs](https://github.com/DCNick3/shader-compiler-rs) | Maxwell/SPH → GLSL 前端 | 仓库 MPL-2.0；vendored 文件含 GPL-2.0-or-later/MPL-2.0，逐文件审阅 |
| [hactool](https://github.com/SciresM/hactool) | 解包自己的游戏 | 上游许可；不随项目分发 |
| [hactoolnet/LibHac](https://github.com/Thealexbarney/LibHac) | 解包/合并工具 | 上游许可；不随项目分发 |
| [nut](https://github.com/blawar/nut) | 游戏文件管理 | 上游许可；不随项目分发 |

## 关键边界

点阵字体使用 [GNU Unifont 18.0.01](https://unifoundry.com/unifont/index.html)。
本项目的派生遮罩采用其 OFL-1.1 选项，附原 copyright 与许可；下载工具固定 HEX、
BDF 和 OFL 身份，字体本体及游戏字库包不进入 Git。实现和证据见 ADR 0033。

- **SceShaccCg / libshacccg.suprx**：公开可下载不等于允许再分发。不要提交、打包或
  上传到 release；只在本地合法获取和使用。
- **Shacc**：GPL-2.0。若分发包含 Shacc 的二进制/源码，必须遵守 GPL；本项目默认不
  分发，只记录版本/哈希。
- **GXP/Cg/shader 内容**：由游戏 shader 派生，属于游戏内容，不得公开分发。
- **Vita3K/kubridge 补丁**：本项目不 vendor 其代码；只读参考/可选的导入库需要遵守
  各自许可证。
- **VitaSDK**：构建工具链，按上游许可使用；生成的 VPK 包含游戏模块，不能公开分发。

## 项目自身许可

本仓库尚未声明统一的开源许可证。发布或接受外部贡献前，维护者需要确定项目许可证，
并确保它不与上述第三方许可证冲突。在此之前，默认按“保留所有权利”处理，除非文件
中另有说明。

## 版本锁定

离线着色器工具链的具体 commit/哈希见
`config/graphics/shader-toolchain.json`；kubridge API 事实见
`config/kubridge-api-lock.json`。版本变化必须更新锁文件并记录证据。
