# 游戏文件、密钥与输入布局

## 1. 合法性与边界

本项目只处理**你合法拥有**的 Monster Hunter Generations Ultimate 1.4.0 副本。
仓库不提供、不下载、不分享：

- 游戏 NSP/XCI/NCA/ROMFS 内容
- `prod.keys`、`title.keys`、ticket/cert
- 固件、系统存档、用户存档
- 由游戏内容派生的 GXP、纹理、模型、静态包

这些文件只能放在本机被 Git 忽略的目录中。不要提交、不要上传、不要附在 PR 里。

## 2. 需要的游戏输入

项目锁定的是**完整 1.4.0 更新输入**，不是单独的基础版或单独更新包：

| 输入 | 内容 | 来源 |
|---|---|---|
| ExeFS | `rtld`、`main`、`subsdk0`、`sdk`、`main.npdm` | 1.4.0 更新 NCA 的 ExeFS |
| RomFS | 完整 `nativeNX/` 树，含 `chS/`、`chT/` | 基础版 RomFS + 1.4.0 更新覆盖后的完整树 |
| NACP | `control.nacp`，显示版本 1.4.0 | 1.4.0 更新内容 |

程序 ID 为 `0100770008DD8000`，更新 ID 为 `0100770008DD8800`。只接受
`config/target-1.4.0.json` 中的四个模块哈希、Build ID 和 NACP/NPDM 哈希。

> 中文资源目录存在不等于中文字形/界面/对话已验证。项目当前已验证简体界面与村长
> 对话，但新输入仍应重新核对。

## 3. 推荐目录布局

```text
D:\MHGU_vita_port\
├─ game-input\                 # Git 忽略
│  ├─ exefs\                   # rtld main subsdk0 sdk main.npdm
│  ├─ romfs\                   # 必须包含 nativeNX\...
│  └─ control.nacp
├─ local\                      # Git 忽略
│  ├─ keys\
│  │  ├─ prod.keys             # 你自己主机的 console keys
│  │  └─ title.keys            # 你自己主机的 title keys
│  ├─ raw\                     # 原始 NSP/XCI，只读保留
│  ├─ extract\                 # 解包工具输出
│  └─ shader-tools\            # Shacc / SceShaccCg（可选）
├─ build\                      # Git 忽略
└─ ...
```

也可以把整个 `game-input` 放在别的盘，只要通过环境变量指向它。

### 3.1 旧工作区布局对应关系

如果你从旧 `vita-port` 工作区迁移：

| 旧路径 | 新布局 |
|---|---|
| `../unpacked/update-mhgu/game/0/` | `game-input/exefs/` |
| `../unpacked/update-mhgu/game/1/`（内含 `nativeNX/`） | `game-input/romfs/` |
| `../unpacked/update-mhgu/content/<hash>/0/control.nacp` | `game-input/control.nacp` |
| `../ProdKeys.NET-*/prod.keys`、`title.keys` | `local/keys/`（只用于解包） |

新仓库不会读取旧工作区；需要时用 junction 或复制到上述位置。

## 4. 密钥放哪里

`prod.keys` 与 `title.keys` **只被解包工具使用**。构建 VPK 和运行时都不读取它们。

```text
local/keys/prod.keys
local/keys/title.keys
```

- 从你自己的主机导出；不要使用来源不明的 key 包。
- 不要放进 `game-input/`，更不要提交。
- 解包工具的 `--keyset` / `-k` 指向 `local/keys/prod.keys`；ticket 对应的 title key
  在 `title.keys` 中。
- 如果只做已解包输入的构建，完全不需要 key 文件。

## 5. 解包与合成

仓库不自动解密或合成游戏内容，也不提供 NSP/XCI。合法持有者可使用
hactool、hactoolnet、nut 等工具，按以下顺序自行处理：

1. 解包基础版 NSP/XCI，得到基础 RomFS 与基础 ExeFS。
2. 解包 1.4.0 更新 NSP，得到更新 ExeFS、更新 RomFS 覆盖文件与 `control.nacp`。
3. 按更新清单把基础 RomFS 与 1.4.0 覆盖合并成完整树，保留 `nativeNX/chS`、
   `nativeNX/chT` 等目录。
4. 将更新 ExeFS 的 `rtld/main/subsdk0/sdk/main.npdm` 放入 `game-input/exefs/`。
5. 将完整 RomFS 根（包含 `nativeNX/` 的那一层）放入 `game-input/romfs/`。
6. 将 `control.nacp` 放入 `game-input/control.nacp`。

不同工具的目录结构略有差异；判断标准不是文件名，而是
`tools/input/verify_input.py` 是否通过以及 RomFS 根是否直接包含 `nativeNX/`。

## 6. 校验输入身份

```powershell
$env:MHGU_EXEFS = "D:\MHGU_vita_port\game-input\exefs"
$env:MHGU_ROMFS = "D:\MHGU_vita_port\game-input\romfs"
$env:MHGU_NACP  = "D:\MHGU_vita_port\game-input\control.nacp"
$env:MHGU_PYTHON = "C:\Python312\python.exe"

.\scripts\check-inputs.ps1
```

输出写入 `build/reports/input-check.json`，包含每项的大小、SHA256、Build ID 和
display version 检查。通过只代表 **ExeFS 与 NACP 身份**，不代表 RomFS 完整、运行成功
或玩法验收。

也可以直接运行：

```powershell
python tools/input/verify_input.py `
  --exefs game-input/exefs `
  --nacp game-input/control.nacp `
  --output build/reports/input-check.json
```

## 7. RomFS 完整性

完整 RomFS 约 35,000 文件、12.3 GB。项目不要求每次启动全盘扫描；运行时使用
`romfs.index` 与 `romfs.map` 映射到 `romfs-tree/`，路径和大小在加载时校验。

如需建立文件清单用于比对：

```powershell
python tools/input/content_manifest.py --romfs game-input/romfs --output build/reports/romfs-manifest.json
```

该清单是本地证据，不进入 Git。

## 8. 输入常见错误

| 错误 | 原因 | 处理 |
|---|---|---|
| `size mismatch` / `SHA256 mismatch` | 版本、地区或更新不匹配 | 重新确认 1.4.0 完整输入 |
| `display version mismatch` | NACP 不是 1.4.0 | 使用 1.4.0 的 `control.nacp` |
| `invalid NSO header` | 文件被压缩/改名/损坏 | 使用原 ExeFS 模块 |
| RomFS 根缺少 `nativeNX` | 指向了 `nativeNX` 内部 | 指向其父目录 |
| 中文缺失 | 合并时漏掉 `chS/chT` | 重新合成完整 RomFS |

身份不符必须停止，不能通过改锁文件或跳过检查来“让它编译”。
