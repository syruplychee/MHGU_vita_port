# tools/assets

| 脚本 | 作用 |
|---|---|
| `tex_format.py` | Switch TEX 头/布局解析 |
| `convert_tex.py` | TEX 转换为目标布局/格式 |
| `build_asset_inventory.py` | 资源清单与来源身份 |
| `pack_static_assets.py` | 打包静态资源 |
| `tex_verify.c` | 纹理转换校验（宿主 C） |

纹理/模型是游戏派生数据，输出不得提交 Git。格式和边界以脚本 `--help`、源码和
[运行时资源](../../docs/05-runtime-assets.md) 为准。
