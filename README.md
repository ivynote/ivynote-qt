# 常青笔记 Ivynote · Qt 桌面客户端

Linux 与 Windows 的原生客户端，基于 Qt 6 Widgets。本地优先：打开即可写，断网照常用；接入自托管的 Ivynote 服务器后在后台可靠同步。

协议与行为依据 `ivynote/docs/client-development-guide.md`，与服务端代码对照实现，并用真实服务端做了集成测试。

## 功能

**笔记库**
- 启动即可用：首次运行自动创建一个本地笔记库。
- 两种笔记库，可并存、随时切换（左上角笔记库菜单，或 文件 → 切换笔记库）：
  - **本地笔记库**：只存在这台电脑上，不需要服务器，其余功能完全一样。
  - **同步笔记库**：绑定“服务器 + 账户”，登录或注册（首个账户成为管理员）。
- 本地笔记库**不能**改为同步，避免两份数据混在一起。迁移方式：接入一个同步笔记库，然后 设置 → 笔记库 → **从其他笔记库导入**。笔记本、标签、附件、置顶、创建时间、笔记间链接都会带过去，导入的笔记像新写的一样同步。
- 同一账户重复接入时复用已有笔记库，不会产生两份。

**同步**（指南第 6 章）
- 所有读写走本地；编辑 300 ms 内落盘，800 ms 防抖后推送。
- outbox 持久化，断网、重启都不丢；按首次入队顺序推送（笔记本先于放进它的笔记）。
- 正文带 `baseRev` 由服务器三方合并；推送途中继续输入不丢字；无法合并时提示并可跳到冲突副本，冲突时正在输入的文字进入副本。
- 字段级 HLC；被服务器拒绝的修改留在本机并在状态栏显示数量，再次编辑或重启后重试；`invalid hlc` 时提示校准时间。
- 首次同步用 snapshot，之后增量 pull；SSE 实时通知 + 断线重连 + 70 秒静默看门狗；回到前台、网络恢复时追平；失败指数退避（2 s 起，上限 60 s）。
- 令牌失效只要求重新登录（只接受原账户），本地数据和 outbox 保留。
- 离线插入的图片立即显示，联网后上传并把 `ivy://res/local-…` 替换成真实 ID；被拒绝的文件不阻塞其他同步。

**加密与安全**
- 服务端数据由服务端加密（指南第 8 章），API 收发明文，客户端不碰传输内容。
- 本机副本可选加密：每条数据库记录、每个附件缓存/待上传文件都用 XChaCha20-Poly1305 封装，关联数据绑定“表+ID”或“文件+分块序号+是否末块”，防替换、防截断。256 位密钥存系统钥匙串（Secret Service / Windows 凭据管理器），另给一把恢复密钥；钥匙串丢失时用恢复密钥解锁。同步笔记库默认开启，本地笔记库可选。
- 登录令牌只存系统钥匙串；钥匙串不可用时退回仅当前用户可读的文件并在设置中警告。
- 不存 Cookie；日志不含令牌和正文；非本机 `http://` 地址明确警告；不关闭证书校验。
- Markdown 渲染禁用原始 HTML，只放行 http/https/mailto 链接；http(s) 图片在后台加载（单张 25 MB 上限，磁盘缓存），不加载其他协议的图片；外部链接用系统浏览器打开。
- 单实例锁，避免两个进程同时写一个笔记库。

**编辑**
- 纯 Markdown，三种模式：即时渲染（光标所在行显示源码，其他行隐藏语法、渲染复选框/列表/引用/分隔线/代码块/行内图片、可编辑表格）、源码、双栏预览。文档内容始终不被改写。
- `[[` 双链补全，点击双链跳转，不存在则在当前笔记本新建；`[[标题#小节]]`、`[[标题|别名]]`、`ivy://note/` 都可跳转。
- 图片：标准 `![]()`、带标题/括号/尖括号地址、引用式图片、图文混排均可显示；兼容旧导入文章中的独立 `[图片URL](图片URL)`（包括标题行），普通文字链接保持链接，正文不被改写。加载失败显示提示，右键可重新加载。
- 顶栏 ＋ 或 `Ctrl+/` 打开插入菜单：标题、格式、列表、引用、代码块、表格、日期时间、网页链接、网络图片和本机附件。
- 即时渲染中的表格保持网格，点击单元格直接编辑；输入立即写回 Markdown 并按常规保存同步，同一单元格连续输入可一次撤销。Tab/Shift+Tab 切换单元格、Enter 向下、末尾自动增行、Esc 离开；右键支持行列增删、列对齐、排序和整理源码。
- `Ctrl+H` 查找替换，支持单项与全部替换；全部替换作为一次操作撤销，回收站笔记不可替换。
- `Ctrl+F` 在当前笔记中查找，支持大小写选项、匹配高亮、Enter/F3 下一项、Shift+F3 上一项、循环查找和 Esc 关闭。
- 顶栏直接切换即时渲染、源码、双栏；切换模式或字号保留正文撤销/重做历史。
- 代码围栏区分标记与长度，多反引号行内代码保持字面内容；代码块内不会误续写列表。预览中的待办按 Markdown 解析顺序勾选，支持引用与嵌套列表。
- 双栏中的图片随预览宽度重新适配；代码和表格跟随字号，表格保留列对齐，主题切换刷新预览。
- 回车续写列表/待办，Tab 缩进；粘贴/拖入图片和文件即上传；粘贴一律为纯文本。
- 快捷键：`Ctrl+K` 搜索、`Ctrl+Shift+P` 命令面板、`Ctrl+N` 新建、`Ctrl+\` 收起书架、`Ctrl+B/I/E`、`Ctrl+Shift+X/K`、`Ctrl+Alt+1/2/3`、`Ctrl+Shift+7/8/9`、`Ctrl+R` 立即同步。

**组织与其他**
- 书架树：置顶、笔记本（嵌套、计数含子笔记本）、根笔记、标签、回收站；排序；拖拽移动笔记和笔记本（悬停 650 ms 展开）；右键菜单；每分支 100 篇分页。
- 标签（颜色、改名、删除）、回收站（撤销、恢复、永久删除、清空）、复制笔记、移动、大纲、反向链接、字数。
- 本地全文搜索（中文、多词 AND，命中高亮）。
- 在线：历史版本（渲染/源码预览、恢复）、公开分享（密码、有效期、访问次数）。
- 导入 Markdown 文件/文件夹（YAML 头、子文件夹成笔记本、本地图片自动上传、笔记间链接重连）；导出为 Markdown 文件夹（含附件与相对链接）。
- 浅色/深色/跟随系统，字号 13–22，中文/English，减少动态效果；字号和编辑模式与网页版跨设备同步。

未做（按指南属 P1/P2，可后补）：媒体库页面、AI 问答、关系图、API 令牌与备份配置（设置中引导到网页版）、视频/音频内嵌播放（目前用系统播放器打开）。

## 安装

在 [Releases](https://github.com/ivynote/ivynote-qt/releases) 下载最新版本：

- **Windows 10/11**：`Ivynote-x.y.z-windows-x64-setup.exe`，双击安装（默认装到当前用户，不需要管理员；向导里可选“为所有用户安装”）。升级时直接安装新版本，数据不受影响。不想安装可以用 `…-portable.zip`，解压后运行 `ivynote.exe`。安装包暂未签名，SmartScreen 提示时点“更多信息 → 仍要运行”。
- **Linux x86_64**：`Ivynote-x.y.z-x86_64.AppImage`，加执行权限后双击或在终端运行：
  ```bash
  chmod +x Ivynote-*.AppImage && ./Ivynote-*.AppImage
  ```
  单文件、自带 Qt，不依赖发行版的 Qt 版本；需要 glibc ≥ 2.35（Ubuntu 22.04、Debian 12、Fedora 36 及更新），不需要 libfuse2。内置 fcitx5 与 ibus 输入法插件。想加入应用菜单、自动更新，用 [Gear Lever](https://flathub.org/apps/it.mijorus.gearlever) 或 AppImageLauncher 打开即可（AppImage 内嵌了更新信息）。

## 构建

依赖：Qt ≥ 6.8（Core、Gui、Widgets、Network、Sql、Svg、Concurrent，测试另需 Test）、[QtKeychain](https://github.com/frankosterfeld/qtkeychain)（Qt 6 版）、libsodium、md4c、CMake ≥ 3.21。

### Linux

```bash
# Arch
sudo pacman -S qt6-base qt6-svg qtkeychain-qt6 libsodium md4c cmake ninja
# Debian/Ubuntu
sudo apt install qt6-base-dev qt6-svg-dev qtkeychain-qt6-dev libsodium-dev libmd4c-dev libmd4c-html0-dev cmake ninja-build
```

```bash
make            # 显示所有目标
make run        # Release 构建并启动
make debug      # Debug 构建到 build-debug/
make test       # 构建并运行全部测试
make install PREFIX=$HOME/.local
```

`make` 只是包装 CMake，等价于 `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`。

### Qt Creator / VS Code

直接打开项目根目录的 `CMakeLists.txt`（Qt Creator：文件 → 打开文件或项目），选 `CMakePresets.json` 里的 Debug 或 Release 预设即可构建、调试、运行测试。VS Code 装 CMake Tools 扩展后同样识别这些预设；构建会生成 `compile_commands.json` 供 clangd 使用。

### Windows（MSVC + vcpkg）

```powershell
vcpkg install qtbase qtsvg qtkeychain-qt6 libsodium md4c --triplet x64-windows
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
windeployqt build\ivynote.exe
```

安装包用 [Inno Setup](https://jrsoftware.org/isinfo.php) 生成，脚本在 `packaging/windows/ivynote.iss`，CI 中的完整步骤见 `.github/workflows/release.yml`。

## 发布

推一个 `vX.Y.Z` 标签即可，其余由 `.github/workflows/release.yml` 完成：

```bash
git tag v0.2.0
git push origin v0.2.0
```

1. 版本号取自标签（`v0.2.0` → 应用内显示 `0.2.0`），不需要先改 `CMakeLists.txt`；带后缀的标签（`v0.2.0-beta.1`）发布为预发行版。
2. 并行构建：Linux（Ubuntu 22.04 + Qt 6.8 LTS，跑全部测试，打 AppImage）、Windows（MSVC + Qt 6.8 LTS，打安装包和便携 zip）。两边都会实际启动打包好的程序截一张图，确认依赖齐全。
3. 全部成功后才创建 Release：上传安装包、AppImage（及 `.zsync` 增量更新文件）、`SHA256SUMS.txt`；说明 = `packaging/release-notes.md` 的下载说明 + 自上个标签以来合并的 PR 列表（按 `.github/release.yml` 里的标签分组）。任何一步失败都不会产生半成品 Release；修好后删掉标签重推，或直接重跑失败的 workflow。

改动打包相关文件的 PR 会跑同样的构建（不发布），产物在 workflow 的 Artifacts 里，可下载试装。

本地打 AppImage（需要 [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy)、linuxdeploy-plugin-qt、[appimagetool](https://github.com/AppImage/appimagetool) 在 `PATH` 上）：

```sh
make appimage-tools # 首次构建时下载打包工具（需要 curl）
make appimage
# 手动覆盖版本号：make appimage VERSION=0.2.0
```

## 测试

```bash
make test
```

- `core`：HLC、ID、双链/摘要/字数/大纲/任务、搜索、书架树、加密与恢复密钥、错密钥检测。
- `engine`：脚本化传输层下的 outbox、推送中继续编辑、合并、冲突副本、拒绝与拆批、pull 保留待推字段、墓碑、snapshot、离线上传回放、本地笔记库。
- `integration`：两台“设备”经**真实服务端**同步（合并、冲突、字段合并、级联删除、墓碑、5 MiB 正文、推送中编辑、拒绝、去重、SSE、401）。设置 `IVY_SERVER_BIN` 指向服务端二进制才运行，否则跳过：
  ```bash
  make test-integration IVY_SERVER_BIN=/path/to/ivynote
  ```
- `ui`：即时渲染不改写源文本、远端更新保持光标、模式/字号切换后撤销重做、列表续写与选区缩进、代码围栏和多反引号、引用/嵌套待办定位、图片随宽度缩放、格式快捷键、纯文本粘贴、渲染安全。
- `transfer`：导出 Markdown、导入 Markdown、从其他笔记库导入。

开发辅助：`IVY_SECRETS_IN_FILE=1` 不使用系统钥匙串；`IVY_SCREENSHOT=out.png` 启动后截图并退出（配合 `QT_QPA_PLATFORM=offscreen` 与独立的 `XDG_DATA_HOME`/`XDG_CONFIG_HOME`）。

## 结构

```text
src/core      模型与 JSON、HLC、ID、Markdown 工具、搜索、书架树、文案、格式化
src/storage   本机加密（libsodium）、钥匙串、SQLite 存储
src/sync      HTTP 客户端、传输层、同步引擎、SSE
src/vault     笔记库登记、会话（打开的笔记库）、文件存储、导入导出
src/ui        主题、图标、书架、编辑器与即时渲染、笔记页、命令面板、各对话框
tests         单元、引擎、真实服务端集成、UI、导入导出
```

数据位置：Linux `~/.local/share/Ivynote/Ivynote/`，Windows `%APPDATA%\Ivynote\Ivynote\`。每个笔记库一个目录：`vault.db`（SQLite，每行一条 JSON，加密时为密文）、`files/`、`thumbs/`、`uploads/`。

`make appimage` 与 Linux CI 共用配置、编译、测试和打包入口，默认使用
`build-appimage/`，产物输出到 `dist/`；省略 `VERSION` 时使用
`git describe --dirty --always --tags --abbrev=7` 自动生成版本号（去掉标签的 `v` 前缀，
有未提交的已跟踪文件修改时附加 `-dirty`）；无版本标签时使用 `<CMake 项目版本>-dev.<Git 描述>`，
无 Git 信息时回退到 CMake 项目版本。
CI 继续使用发布流程传入的 `VERSION`，确保各平台产物和发布说明的版本一致。
环境需先准备好 Qt、QtKeychain、系统依赖及上述打包工具；CI 中的依赖安装、
输入法插件准备和 SQLite 驱动筛选仍属于环境准备。
可通过 `APPIMAGE_BUILD_DIR`、`OUT_DIR`、`APPIMAGE_PREFIX_PATH` 和 `CMAKE_FLAGS`
覆盖构建路径及参数；`DEPS`、`QT_ROOT_DIR`、`TOOLS_DIR`、`QMAKE`、
`LD_LIBRARY_PATH` 和 `UPDATE_INFORMATION` 可与 CI 使用相同的环境变量。
