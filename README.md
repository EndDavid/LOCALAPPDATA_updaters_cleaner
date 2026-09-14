# LocalAppData 更新残留清理

一个 Windows 桌面小工具，用来清理 `%LOCALAPPDATA%` 下被遗弃的更新程序、安装包和临时文件。

这类文件是很多软件更新后留下的“一次性”产物：`xxx-updater` 目录里的安装程序、下载到一半的
`installer.exe` / `setup.exe` / `.msi` / `.nupkg`，以及 `Temp` 里长期没人管的陈旧条目。
`%LOCALAPPDATA%` 通常还被当作重要数据目录，不好整体清理，所以这里按明确的规则挑出可删的部分，
并由你逐项确认。

界面基于 [EUI-NEO](https://github.com/sudoevolve/EUI-NEO)（C++17 跨平台 UI 框架）。

## 功能

- 扫描 `%LOCALAPPDATA%` 下的更新/安装残留，并单独统计 `%LOCALAPPDATA%\Temp`
- 逐项复选框确认，**扫描结果默认全部不勾选**；实时统计已选数量和预计释放空间
- 按名称 / 修改日期 / 类型 / 大小排序，支持升序与降序
- 支持全选、反选；每一项带一个文件夹图标，可直接在资源管理器里查看
- 清理前弹出确认面板列出待删清单，明确提示不可恢复
- 排除规则可编辑、可持久化（目录 + 关键词），带“恢复默认规则”
- 「浏览…」调用系统文件夹选择对话框（默认定位到 `%LOCALAPPDATA%`）
- 导出清理清单，位置自选（系统另存为对话框）

## 环境要求

- CMake 3.14 或更高
- 支持 C++17 的编译器：MSVC 2019 16.11+ / MinGW-w64 GCC 12+ / Clang 14+
- OpenGL 开发文件（EUI-NEO 默认使用 OpenGL 后端）
- Windows 上还需要 `windres`（MinGW 自带）用于编译图标与版本信息资源

## 获取依赖

程序依赖 EUI-NEO 源码，CMake 会按下面的顺序处理，**不需要手动准备**：

1. `-DEUI_NEO_SOURCE_DIR=<路径>` 显式指定；
2. 在工程目录下自动查找 `EUI-NEO-main/`、`3rd/EUI-NEO/`、`EUI-NEO/`；
3. 都没有时用 `FetchContent` 拉取（优先 GitHub，不通时自动切换 AtomGit 镜像）。

需要离线构建时，先把框架源码放到工程的 `EUI-NEO-main/` 目录即可：

```sh
git clone https://github.com/sudoevolve/EUI-NEO.git EUI-NEO-main
```

EUI-NEO 仓库内自带 GLFW、FreeType、libpng、zlib、glad 等第三方源码，
默认 `-DEUI_DEPS_MODE=bundled` 时全部使用这些内置副本，构建过程不会再联网下载其他东西。

## 构建

### Windows（MinGW-w64）

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target localappdata_cleaner --parallel
```

改过源码后只需要第二条命令增量编译：

```sh
cmake --build build --target localappdata_cleaner --parallel
```

### Windows（Visual Studio）

```sh
cmake -S . -B build
cmake --build build --config Release --target localappdata_cleaner --parallel
```

### Linux / macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target localappdata_cleaner --parallel
```

## 运行

```sh
build\localappdata_cleaner.exe          # Windows MinGW
build\Release\localappdata_cleaner.exe  # Windows Visual Studio
./build/localappdata_cleaner            # Linux / macOS
```

可执行文件旁边必须有 `assets/` 目录（字体、图标等运行资源），构建时会自动部署，
所以请整体移动或打包 `exe` 与 `assets/`，不要只拷贝 exe。

## 扫描规则

时间阈值可选 1 / 3 / 7 / 14 / 30 天，默认 7 天。

- **残留目录**：名字含 `updater` 或 `installer`，或名字正好是 `pending`；位于 `%LOCALAPPDATA%` 下 2 层以内；
  修改时间早于阈值。命中后整目录计入，不再向内重复统计。
- **安装包文件**：`update*/installer*/setup*.exe`，以及所有 `*.msi`、`*.nupkg`；3 层以内；修改时间早于阈值。
- **临时文件**：`%LOCALAPPDATA%\Temp` 下修改时间早于阈值、且未被排除规则命中的顶层条目，按整条目统计。

## 排除规则

规则不区分大小写，作用于任何层级，也作用于 `Temp`。改动后需要重新扫描。

- **排除目录**（目录名精确匹配，命中即整棵子树跳过）
  默认：`Packages`、`Programs`
- **排除关键词**（名称包含即跳过）
  默认：`site-packages`、`node_modules`、`Python`、`pyinstaller`、`Microsoft`、
  `Google`、`Discord`、`Slack`、`GitHubDesktop`、`osu`

两组规则都可以在界面里逐条增删，「排除目录」可以直接用「浏览…」从系统对话框里选；
左下角「恢复默认规则」可一键还原。规则保存在：

```
%LOCALAPPDATA%\LocalAppDataCleaner\settings.json
```

配置文件是普通 JSON（含 `version`、`excludeDirectories`、`excludeKeywords`、`lastExportDirectory`），
文件损坏或字段缺失时会回退到默认规则，不会影响启动。这个配置目录本身不会被扫描规则命中。

## 导出清单

「导出清单」会弹出系统“另存为”对话框，默认导出到上次使用过的目录，一次生成两个文件：

- `<名字>.html`：清理报告，样式内联在文件里，双击即可用浏览器查看（表格、勾选行高亮、完整路径）
- `<名字>.xml`：同一份数据的纯结构化版本，便于脚本或表格软件处理

## 安全性说明

- **删除不可恢复**：不经过回收站。请先确认勾选项，再在确认面板里复核一次。
- 扫描结果默认全部不勾选，避免误删。
- 正在被占用的文件会跳过并计入失败清单，不会强行删除或中断整批操作。
- 默认排除 `Packages`、`Programs` 以及一批系统/应用关键词，但仍建议自行核对后再清理。

## 已知限制

- 打开目录走 Windows `ShellExecuteA`，路径含非 ASCII 字符时可能打不开，此时会给出提示。
- 「排除目录」按目录名匹配，不是按完整路径，因此选中 `...\Local\Foo` 等于排除所有名为 `Foo` 的目录。
- 「排除关键词」是子串匹配，例如填 `cache` 会同时命中 `Cache`、`my-cache-v2`。
- 系统“保存文件”和“选择文件夹”对话框目前只在 Windows 上实现，其他平台会提示不支持。

## 目录结构

```
CMakeLists.txt          构建脚本（含依赖解析、图标与版本资源、素材部署）
src/main.cpp            全部界面与业务逻辑
assets/                 运行素材（窗口图标、列表用文件夹图标）
cmake/app_resources.rc.in  Windows 图标与 VERSIONINFO 的模板
tools/make_icon.py      从方形 PNG 生成多尺寸 .ico 的脚本（仅用 Python 标准库）
```

## 版本号与图标

- 版本号在 `CMakeLists.txt` 的 `project(LocalAppDataCleaner VERSION x.y.z ...)` 里改，
  它会同时写进可执行文件的 VERSIONINFO（产品名、版本、版权）。
- 图标：把方形 PNG 放到 `assets/19icon.png`，然后重新生成多尺寸 ico：

  ```sh
  python tools/make_icon.py assets/19icon.png assets/19icon.ico
  ```

  `.ico` 已登记为 CMake 的 configure 依赖，改完重新构建即会生效。

## 依赖与许可

- 界面框架 EUI-NEO 以源码或在线拉取方式引入，遵循其 Apache License 2.0，本仓库不再分发其源码。
- 其 `assets/` 下的字体与图标字体遵循各自上游许可，构建时会随运行资源一起部署。
- 本仓库自身尚未附带许可证文件，如需正式开源请自行补充（例如 MIT 或 Apache-2.0）。
