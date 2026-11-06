# tools/assets

| 脚本 | 作用 |
|---|---|
| `tex_format.py` | Switch TEX 头/布局解析 |
| `convert_tex.py` | TEX 转换为目标布局/格式 |
| `build_asset_inventory.py` | 资源清单与来源身份 |
| `pack_static_assets.py` | 打包静态资源 |
| `build_pixel_fonts.py` | 保留 GFD 布局度量，生成点阵字形与真正 R8 字体包；需要 Pillow |
| `fetch_pixel_font.py` | 下载固定 Unifont、校验 SHA 并保存 OFL/copyright |
| `tex_verify.c` | 纹理转换校验（宿主 C） |

纹理/模型是游戏派生数据，输出不得提交 Git。格式和边界以脚本 `--help`、源码和
[运行时资源](../../docs/05-runtime-assets.md) 为准。

点阵字库的覆盖、格式选择、失败对照和生成命令见 [字体结果](../../docs/19-pixel-fonts.md)。
