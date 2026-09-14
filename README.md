_项目基于 [EUI-NEO](https://github.com/sudoevolve/EUI-NEO)_

# LocalAppData 更新残留清理

一个 Windows 桌面小工具，用来清理 `%LOCALAPPDATA%` 下被遗弃的更新程序、安装包和临时文件。

这类文件是很多软件更新后留下的“一次性”产物：`xxx-updater` 目录里的安装程序、下载到一半的
`installer.exe` / `setup.exe` / `.msi` / `.nupkg`，以及 `Temp` 里遗留的陈旧条目。

`%LOCALAPPDATA%` 通常被当作重要数据目录，不好整体清理，软件按明确的规则挑出可删的部分，
并由你逐项确认。

## 功能

- 扫描 `%LOCALAPPDATA%` 下的更新/安装残留和 `%LOCALAPPDATA%\Temp`
- 逐项复选框确认，扫描结果默认不勾选；实时统计预计释放空间
- 按名称 / 修改日期 / 类型 / 大小排序，支持升序与降序
- 清理前弹出确认面板列出待删清单，**不可恢复**
- 可自定义排除规则
- 导出清理清单，位置自选


## 扫描规则

时间阈值可选 1 / 3 / 7 / 14 / 30 天，默认 7 天。

- **残留目录**：名字含 `updater` 或 `installer`，或名字正好是 `pending`；位于 `%LOCALAPPDATA%` 下 2 层以内；
  修改时间早于阈值。命中后整目录计入，不再向内重复统计。
- **安装包文件**：`update*/installer*/setup*.exe`，以及所有 `*.msi`、`*.nupkg`；3 层以内；修改时间早于阈值。
- **临时文件**：`%LOCALAPPDATA%\Temp` 下修改时间早于阈值、且未被排除规则命中的顶层条目，按整条目统计。

## 排除规则

规则不区分大小写，作用于任何层级，改动后需要重新扫描。

默认排除了一些目录和关键词，两组规则都可以在界面里逐条增删。
左下角「恢复默认规则」可一键还原。规则保存在：
```
%LOCALAPPDATA%\LocalAppDataCleaner\settings.json
```

## 导出清单

「导出清单」导出两份清单：
- `<名字>.html`：清理报告，可用浏览器查看
- `<名字>.xml`：同一份数据的纯结构化版本，便于脚本或表格软件处理

## 目录结构

```
CMakeLists.txt          构建脚本（含依赖解析、图标与版本资源、素材部署）
src/main.cpp            全部界面与业务逻辑
assets/                 运行素材（窗口图标、列表用文件夹图标）
cmake/app_resources.rc.in  Windows 图标与 VERSIONINFO 的模板
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

### 环境要求

- CMake 3.14+
- 支持 C++17 的编译器：MSVC 2019 16.11+ / MinGW-w64 GCC 12+ / Clang 14+
- 默认渲染器所需的 OpenGL 开发文件

### 获取依赖

程序依赖 EUI-NEO 源码，CMake 会按下面的顺序处理

1. `-DEUI_NEO_SOURCE_DIR=<路径>` 显式指定；
2. 在工程目录下自动查找 `EUI-NEO-main/`、`3rd/EUI-NEO/`、`EUI-NEO/`；
3. 都没有时用 `FetchContent` 拉取（优先 GitHub，不通时自动切换 AtomGit 镜像）。

需要离线构建时，将框架源码放到工程的 `EUI-NEO-main/` 目录：

```sh
git clone https://github.com/sudoevolve/EUI-NEO.git EUI-NEO-main
```

EUI-NEO 仓库内自带 GLFW、FreeType、libpng、zlib、glad 等第三方源码，
默认 `-DEUI_DEPS_MODE=bundled` 时全部使用这些内置副本，构建过程不会再联网下载其他内容。

## 构建

### Windows（MinGW-w64）

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target localappdata_cleaner --parallel
```

### Windows（Visual Studio）

```sh
cmake -S . -B build
cmake --build build --config Release --target localappdata_cleaner --parallel
```

## 运行

```sh
build\localappdata_cleaner.exe          # Windows MinGW
build\Release\localappdata_cleaner.exe  # Windows Visual Studio
```
并包含 `assets/` 。
