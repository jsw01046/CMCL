# CMCL（C++ Minecraft Launcher）

从零实现的 C++20 Minecraft 启动器核心，架构上参考 PCL2 的经验，但不是 PCL 的移植或衍生版本。

当前阶段：**M5 微软正版登录 + M4 模组加载器安装 + Modrinth 资源接入 + 整合包导入**。在 M3（原版下载与修复、镜像自动切换、OpenJDK 管理）基础上，新增 Forge / NeoForge / Fabric / Quilt 加载器的一键安装：自动获取版本清单、必要时先装原版、Forge/NeoForge 调用官方安装器（NeoForge 依赖走 BMCLAPI 预取+离线注入，不受 maven.neoforged.net 连通性影响），完成后可启动、可修复。资源页已接入 Modrinth v2 API，可搜索 Mod / 整合包 / 数据包 / 资源包 / 光影包；整合包支持 Modrinth `.mrpack` 和 PCL/普通 `.minecraft` 导出 zip 的导入，也可以直接拖进窗口。M5 新增 Microsoft 设备码登录、Xbox Live / XSTS / Minecraft Services 认证、DPAPI 加密持久化和启动参数接入。

## 已实现

- 读取 `versions/<id>/<id>.json`，解析 `inheritsFrom` 继承链
  - libraries 合并规则与 PCL 一致：子版本在前、父版本在后
  - 其余字段子版本覆盖父版本
- `rules` 判定（Windows / x64 / `os.version` 正则 / features）
- Maven 坐标 → 库文件路径，支持 `downloads.artifact` 与 `natives` 分类器
- classpath 拼装、缺失库检测
- natives 释放（临时使用系统 `tar.exe`）
- Java 探测（JAVA_HOME / PATH / `Program Files` / `Program Files (x86)` / `.minecraft/runtime` / `.jdks`）
- 按版本 JSON 的 `javaVersion.majorVersion` 选择 Java，优先 64 位
- **最低 Java 版本校验**：低于版本要求时拒绝启动并提示所需版本，自动选择也不会回退到旧 Java
- Forge 1.6.1 - 1.7.2 按 PCL 的规则自动选择 Java 7
- **OpenJDK 管理**：设置页下载 Adoptium Temurin 8/11/17/21/25，优先清华镜像、失败回退官方，校验 SHA-256 后解压到 `%LOCALAPPDATA%\CraftLauncher\jdk`；CMCL 下载的 JDK 可直接删除，本地 Java 只能打开自带卸载程序
- **目录自检与初始化**：启动时自动补齐 `cache` / `jdk` / `natives`；游戏目录不存在时自动创建 `versions` / `libraries` / `assets` / `logs`，不会清理已有文件
- JVM / 游戏参数模板替换、`--width`、`--height`、`--server`、`--demo`
- Win32 `CreateProcessW` 启动、输出流读取、Job Object 进程树管理
- 离线 UUID（与 Java `UUID.nameUUIDFromBytes` 一致）
- **WebView2 图形界面**：多级页面（启动 / 版本管理 / 实例详情 / 下载 / 设置），版本列表、参数详情、启动设置、实时日志、启动/停止
  - 无系统标题栏，最小化 / 最大化 / 关闭按钮在页面右上角，支持拖动标题栏和四边缩放
  - 工作台图标同时用于窗口、任务栏和 exe；图标源文件与生成脚本为 `tools/make_icon.py`
- **M2 实例管理**
  - 版本重命名：同时修改文件夹、JSON 和 JAR，并修正其它实例里的 `inheritsFrom` / `jar` 引用
  - 删除到 Windows 回收站（仅本地固定磁盘），避免误删无法恢复
  - 打开实例文件夹
  - 隔离目录：`versions/<id>/craftlauncher.json` 保存 `{"isolate":true,"isolateSaves":true}`，游戏目录切换到 `versions/<id>/game`
  - 存档单独隔离（默认开启）：只把 `saves` 放到 `versions/<id>/game/saves`，mods、config、资源包等通过目录联接继续与全局共享；切换完整隔离时存档路径不变。实例详情和启动页都有“存档文件夹”按钮
  - 加载器识别与分类：原版 / Forge / NeoForge / Fabric / Quilt / OptiFine，版本管理页可按加载器筛选
- **M3 下载与修复**
  - 安装原版游戏：版本 JSON、client jar、libraries、asset index、assets（含旧版 `virtual/legacy` 资源）
  - 校验 SHA-1 和文件大小，缺失或损坏时重新下载
  - `.part` 临时文件 + 原子替换，取消后不留下半截文件
  - 内置 BMCLAPI、FastMinecraftMirror 等多个镜像；官方优先，速度过低或失败时自动切换
  - 镜像列表可从远程 JSON 刷新（`refresh_mirror_list` 已预留 GitHub 镜像目录接口）
  - 最多 8 个并发下载任务，按阶段显示文件数、字节数、速度和剩余时间
  - 实例详情页可“修复”，只补缺失/哈希不符的文件
- **PCL 风格的下载界面**：左侧“原版游戏 + 社区资源”分类，安装页的版本列表、搜索、安装名称/来源、实时进度和底部进度条；资源页的搜索条件、结果区、分页和下载进度
- **M4 模组加载器安装**
  - Forge / NeoForge / Fabric / Quilt 版本清单获取（Fabric/Quilt 官方 meta，Forge/NeoForge BMCLAPI + 官方 maven 回退），按稳定性排序、默认选最新稳定版
  - 安装页加载器卡片：选择后自动按当前 Minecraft 版本加载可选版本，默认实例名 `<版本>-<加载器名>`
  - Fabric / Quilt：下载 profile JSON 写入实例，继承原版并补全加载器库
  - Forge / NeoForge：缓存官方安装器 → 调用 `--installClient` → 自动重命名为目标实例名 → 校验全部库文件
  - NeoForge 特化：maven.neoforged.net 不可达时，先从 BMCLAPI 预取安装器依赖并 SHA-1 校验，再以 `--offline` 完成安装（实测 26.3 可用）
  - 安装完成后实例可启动、可修复；加载器识别/分类沿用 M2
- **Modrinth 资源接入**
  - 搜索 Mod / 整合包 / 数据包 / 资源包 / 光影包，支持关键词、游戏版本、Mod 加载器、排序和分页（`offset` / `total_hits` 由 API 返回）
  - 结果卡片显示图标、作者、描述、下载量、关注数、更新时间和版本 / 分类标签
  - 内联版本选择：按游戏版本和加载器过滤，列出最近 30 个版本及文件大小；下载前走 `version/{id}` 确认主文件
  - 下载目标按实例有效游戏目录计算（完整隔离 / 存档单独隔离时为 `versions/<id>/game`），分别写入 `mods` / `resourcepacks` / `shaderpacks` / `datapacks`；整合包写入 `.minecraft/modpacks`
  - 支持进度、速度、剩余时间、取消和 SHA-1 校验；目标文件已存在且哈希一致时直接跳过
  - 所有请求携带 `User-Agent`，搜索条件和排序参数均做 URL 编码
- **整合包导入**
  - Modrinth `.mrpack`：解析 `modrinth.index.json`，自动安装 Minecraft 与依赖加载器，下载并校验清单文件，解压 `overrides` / `client-overrides`，默认创建完全隔离实例
  - PCL / 普通 `.minecraft` 导出 zip：识别压缩包里的 `.minecraft`（或 `minecraft`）目录，导入自带版本 JSON / JAR / libraries / assets，并把 mods、config、saves、resourcepacks、光影包等复制到实例目录
  - 入口：整合包版本旁的“安装”、搜索页的“导入本地整合包”，以及把 `.mrpack` / `.zip` 直接拖进窗口
  - CurseForge 官方 `manifest.json` 的模组需要 CurseForge API Key 才能解析下载地址，当前会给出明确提示，暂不自动下载
- **Microsoft 正版登录（M5）**
  - 设备码流程：`login.microsoftonline.com` → Xbox Live 用户认证 → XSTS 授权 → `login_with_xbox` → Minecraft 档案
  - 设置页“账号”分页：显示设备码、复制代码、打开官方登录页、自动轮询；登录后显示账号名 / UUID，可退出登录
  - 账号数据用 Windows DPAPI（`CryptProtectData`）加密保存到 `%LOCALAPPDATA%\CraftLauncher\account.dat`，只有当前 Windows 用户能解密
  - 启动时检查令牌有效期，过期前 5 分钟用 refresh_token 自动续期，并把轮换后的 refresh_token 重新加密保存
  - 启动游戏时自动带入 `--username` / `--uuid` / `--accessToken` / `--userType msa`
  - 客户端 ID 不写入仓库，通过环境变量 `CMCL_MS_CLIENT_ID` 或 `%LOCALAPPDATA%\CraftLauncher\auth.json` 配置
  - ⚠️ Mojang 自 2022 年起对新 Azure 应用实行 API 白名单，未审核的应用会在最后一步返回 `403 Invalid app registration`；申请地址 <https://aka.ms/mce-reviewappid>
- **JSON-RPC 桥**：`app.info`、`instance.list/inspect/rename/delete/open_folder/settings`、`instance.resources.list/delete/toggle/open`、`java.list/available/download/delete/uninstall`、`download.versions/install/repair/cancel/mirrors`、`loader.versions/install`、`modrinth.search/versions/download`、`modpack.install`、`dialog.open_modpack`、`crash.save_report/open_folder`、`auth.status/device_start/device_poll/logout`、`app.open_url`、`game.launch/stop/status`，以及 `game.log` / `game.exit` / `game.summary`（含 findings）/ `download.progress` / `files.dropped` 事件
- 单元测试：继承合并、规则判定、Maven 路径、参数展开、离线 UUID、GBK 日志解码、实例重命名、加载器识别

## 构建

依赖：Visual Studio 2026（或 2022）、CMake ≥ 3.25、vcpkg、Windows 10 1803+。

```powershell
# vcpkg 的根目录（本仓库不写死本机路径）
$env:VCPKG_ROOT = "C:\src\vcpkg"

cd CMCL
cmake --preset vs2026-x64
cmake --build --preset debug
ctest --preset debug
```

`CMakePresets.json` 通过 `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake` 查找 vcpkg；没有设置 `VCPKG_ROOT` 时配置会失败。使用 Visual Studio 2022 时，把 preset 的 `generator` 改成 `Visual Studio 17 2022`，并把 preset 名改成 `vs2022-x64`（或直接在命令行传入自己的 generator）。

## 在 VS Code 里构建/运行

**不要用 C/C++ 扩展默认的 “gcc.exe 构建活动文件”**。那个任务只编译当前 `.cpp`，没有 `-Isrc`，也不会链接 `cl_core`、vcpkg 的 nlohmann-json、WebView2 和 `bcrypt`，必然报 `core/encoding.h: No such file or directory`。本项目是 CMake 多目标工程，必须用 MSVC + CMake。

正确步骤：

1. 安装扩展：**C/C++**（`ms-vscode.cpptools`）和 **CMake Tools**（`ms-vscode.cmake-tools`）
2. 用 VS Code **打开文件夹** `CMCL`（不是打开单个 `main.cpp`，也不是打开它的父目录）
3. 首次先运行一次配置任务：`终端 → 运行任务 → CMake: configure (vs2026-x64)`
4. 按 `Ctrl+Shift+B` 执行默认任务 `CMake: build Debug`
5. 按 `F5` 选择 `CMCL UI (Debug)`（或 `CMCL CLI: list`）

仓库里已经放好 `.vscode/tasks.json`、`launch.json`、`settings.json`。如果 CMake Tools 提示选择 Kit，选 **Visual Studio 2026 – amd64**（或 2022）。

最省事的方式：直接双击项目根目录的 **`run.bat`**，它会自动配置（首次）、编译并启动 `cmcl-ui.exe`。

> 编译 `cmcl-ui.exe` 时如果启动器正在运行，链接会失败；先关掉窗口再编译。

### 常见 CMake / 链接问题

- **`LNK1104: 无法打开 WebView2Loader.dll.lib` 或 `WebView2LoaderStatic.lib`**
  `build/vs2026-x64/vcpkg_installed` 里的 vcpkg 包不完整（常见于清理工具/杀软误删）。关闭 VS 和 `cmcl-ui.exe`，把整个 `vcpkg_installed` 重命名备份，再重新运行 `cmake --preset vs2026-x64` 即可。
- **Release 构建目录提示缺少 `CMakeCache.txt` / vcpkg 依赖**
  重新配置该目录：
  ```powershell
  cmake -S . -B "$env:LOCALAPPDATA\Temp\cmcl-release-build" -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCMCL_BUILD_TESTS=OFF
  ```
- 如果 `web/` 下的文件被外部清理工具删除，可先检查 `build/vs2026-x64/Debug/web` 里是否有构建时复制的同类文件；`web/` 与它应保持一致。
- **F5 报 `Failed to load localizable resource from vsdebugeng.impl.resources.dll`**
  分两种情况：
  - **VS Code**：C/C++ 扩展自带的调试器资源缺失。检查
    `%USERPROFILE%\.vscode\extensions\ms-vscode.cpptools-*\debugAdapters\vsdbg\bin\1033\vsdebugeng.impl.resources.dll`；
    缺失时重装扩展 `ms-vscode.cpptools`（Extensions 里卸载后重新安装），或从同版本的 `Remote Debugger\x86\1033` 复制回去。
  - **Visual Studio**：对比 `Common7/IDE/devenv.exe` 和 `Common7/IDE/vsdbg/vsdebugeng.impl.dll` 的文件版本；不一致时用 Visual Studio Installer 的“修改 → 修复”更新 C++ 调试器组件。
  - 临时可以先按 `Ctrl+F5`（不调试启动）或直接运行 `run.bat`。

产物：

```
build/vs2026-x64/Debug/cmcl-ui.exe   # 图形界面（推荐）
build/vs2026-x64/Debug/cmcl.exe      # CLI
build/vs2026-x64/Debug/cl_tests.exe
```

## 安装包

发布版使用独立的 Release 目录，只包含 GUI、4 个 VC++ 运行库 DLL 和 `web/` 资源：

```powershell
# 1. 配置并编译 Release（不编译测试）
cmake -S . -B build-release -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCMCL_BUILD_TESTS=OFF
cmake --build build-release --config Release --target cmcl-ui --parallel

# 2. 打包 Inno Setup 安装程序
python tools/build_installer.py --release-dir build-release/Release
```

输出：`dist/CMCL-Setup-0.5.1.exe`（约 2.6 MB，安装后约 5.8 MB）。
Inno 脚本位于 `installer/CMCL.iss`，默认按当前用户安装到
`%LOCALAPPDATA%\Programs\CMCL`，不需要管理员权限。

> `build/` 是开发目录，包含 Debug PDB、`.lib`、`.obj`、vcpkg 开发文件和测试程序，
> 所以有几百 MB；这些都不会进入安装包。

## Microsoft 登录

仓库**不包含任何 Azure 客户端 ID**。启用正版登录前，自行注册一个公共客户端应用：

1. Azure 门户 → Microsoft Entra ID → 应用注册 → 新注册
2. 支持的账户类型：**仅个人 Microsoft 账户**
3. 身份验证 → 高级设置 → **允许公共客户端流**：是
4. 复制“应用程序(客户端) ID”

配置方式（任选其一）：

- 环境变量 `CMCL_MS_CLIENT_ID`
- 配置文件 `%LOCALAPPDATA%\CraftLauncher\auth.json`：

```json
{
  "client_id": "你的客户端 ID",
  "tenant": "consumers"
}
```

`tenant` 默认 `consumers`（个人账户）；要支持工作/学校账户可改成 `common`。

登录流程：设备码 → Xbox Live → XSTS → Minecraft Services → Minecraft 档案。账号数据用 Windows DPAPI 加密保存在 `%LOCALAPPDATA%\CraftLauncher\account.dat`，只有当前 Windows 用户能解密；启动时检查有效期并自动续期。设置 → 账号页可以登录、查看账号和退出登录。

> **Mojang API 白名单**
>
> 自 2022 年起，新注册的 Azure 应用必须由 Mojang 加入 Java 版 API 白名单，否则 `login_with_xbox` 会返回 `403 Invalid app registration`。申请地址：<https://aka.ms/mce-reviewappid>。审核通过后使用同一个客户端 ID 即可，无需修改代码或重新打包。

`account.dat`、`auth.json`、日志和构建产物都已加入 `.gitignore`，不会被提交。

## 图形界面

直接运行 `cmcl-ui.exe`。需要系统已安装 **Microsoft Edge WebView2 Runtime**（Windows 10/11 一般自带）。

界面采用多级页面结构，左侧导航固定为“启动 / 版本管理 / 下载 / 设置”，页面模块通过 hash 路由切换：

- **启动**：扫描并选择实例，显示加载器、原版版本、主类、Java、classpath、命令行和 natives；设置玩家名、内存、窗口、全屏、自动连接服务器；实时日志、启动/停止
- **版本管理**：版本总数/可用/隔离统计，按原版 / Forge / NeoForge / Fabric / Quilt / OptiFine 分类筛选；表格里可以打开详情、打开目录、删除到回收站
- **实例详情**：完整启动参数、完整隔离 / 存档单独隔离开关、重命名、删除、打开实例文件夹和存档文件夹、直接启动；可进入“资源管理”二级页面
- **实例资源管理**（`#/instances/<id>/resources`）
  - 按 Mod / 资源包 / 光影包 / 数据包分页签，读取实例实际生效的资源目录（完整隔离用 `versions/<id>/game`，存档单独隔离优先用覆盖层，未建立时回退全局目录）
  - 显示文件名、启用状态、大小和修改时间，支持文件名搜索和刷新
  - 禁用 / 启用：给文件追加或移除 `.disabled` 后缀；删除：移到 Windows 回收站
  - 打开资源文件夹，或直接跳到 Modrinth 对应分类（默认目标仍是当前实例）
  - 自动忽略 `.part` / `.tmp` 等未完成下载文件
- **下载**：参照 PCL 的下载页结构
  - 左侧：“原版游戏”，以及“社区资源”下的 Mod / 整合包 / 数据包 / 资源包 / 光影包
  - 安装页：安装名称、Minecraft 版本（正式版/快照/旧版，支持搜索）、安装来源，实时进度、速度、剩余时间和取消
  - 加载器卡片：选择 Forge / NeoForge / Fabric / Quilt 后自动加载版本列表并开始安装；Fabric API / OptiFine 标记“后续”
  - 资源页：关键词、游戏版本、Mod 加载器、排序、安装目标实例；结果区显示项目信息和内联版本列表，支持分页、下载进度与取消；数据源为 Modrinth（CurseForge 待接入）
- **设置**：常规、Microsoft 账号、Java 管理（OpenJDK 下载 + 已检测 Java 列表）、日志位置与保留策略、关于
  - 账号页：设备码登录、显示账号名 / UUID、退出登录；登录后启动游戏自动使用正版身份
  - 启动器下载的 JDK 标记为“CMCL 下载”，可直接删除（移到回收站）
  - 磁盘本地加载的 Java 标记为“磁盘本地加载”，只能打开它自带的卸载程序；找不到时打开 Windows“应用和功能”

游戏退出后自动生成**崩溃分析**：结果、原因、多条问题发现、日志时间、涉及模组、原始事件、上下文证据和可执行建议；支持复制总结，以及导出完整崩溃报告。手动点击“停止”会显示“游戏已停止”，不会被误判成异常退出。

游戏目录可以直接编辑并“应用”；上次选择的实例也会保存在本地。

窗口尺寸：

- 默认 **1280×800** 逻辑像素，最小 **1024×640**
- 按 DPI 缩放，并自动限制在显示器工作区内
- 关闭时把当前尺寸保存到 `%LOCALAPPDATA%\CraftLauncher\window.json`，下次启动恢复（仍受最小值和工作区限制）

## 崩溃分析

游戏退出后，启动器会分析控制台输出、`logs/latest.log`，以及最近 30 分钟内生成的 `crash-reports/*.txt`，给出面向玩家的多条分析结果，不需要懂 Java 或看堆栈。

每条发现包含：

- 特征 ID（如 `conflict.optifine_render`、`dependency.missing`、`java.class_version`）
- 分类（内存 / Java / 加载器 / 依赖 / 模组冲突 / Mixin / 模组崩溃 / 版本 / 原生库 / JVM / 显卡 / 崩溃）
- 日志时间（`[12:34:56]` 或 Forge 的日期时间戳）
- 涉及模组：冲突双方、缺失的前置与请求者
- 命中的原始日志行和附近上下文（可展开查看）
- 针对该问题的建议

当前覆盖：

- **正常退出**：包括离线模式下 401 / Realms 报错的解释
- **内存不足 / 元空间 / 直接内存 / 内存回收压力 / 内存参数错误**
- **Java 版本不匹配**：解析 `class file version`（52→Java 8、61→17、65→21 等），结合本次启动的 Java 版本
- **缺少模组依赖**：解析 Forge 的 `Mod ID ... Requested by ...`、Fabric 的 `Mod 'x' requires ... which is missing`、`Missing dependency`
- **加载器 / 游戏版本不兼容**
- **已知冲突组合**：OptiFine ↔ Sodium / Embeddium / Rubidium / Magnesium，OptiFine ↔ Iris
- **重复模组、Mixin 注入失败、显式“不兼容”描述**
- **模组加载失败**：自动提取 `Caught exception from <模组名>`
- **模组版本不匹配 / 缺少运行库**
- **原生库（LWJGL）加载失败、显卡 / OpenGL 初始化失败、JVM 崩溃**
- **启动器参数冲突**（如 bootstraplauncher module path 问题）
- 没有匹配到特征时标记为“未知特征”，按退出码给出通用建议

未知崩溃或任意崩溃都可以点“导出报告”：

- 报告中包含运行环境、分析结论、全部发现、日志末尾和崩溃报告末尾
- 可以一键复制（直接发给开发者）、保存到 `crash-reports/CMCL-崩溃分析-*.txt`，或打开报告文件夹
- 不会自动上传任何内容；报告可能包含玩家名、路径和模组列表，发送前请确认

CLI 的 `launch` 也会在退出后打印同样的总结。分析逻辑在 `src/mc/diagnostics.cpp`，可以单独运行 `diagnostics_tests` 验证。

## 日志压缩与清理

启动器日志（`logs/cmcl.log`、`logs/cmcl-ui.log`）在每次启动时自动轮转：

1. 上一次的日志压缩成 `<名字>-YYYYMMDD-HHMMSS.log.gz`
2. 只保留最近一份压缩包，更早的归档删除
3. 归档超过 7 天也会删除（即使只有一份）

新日志开头会记录一行 `[rotation]`，说明压缩和清理结果。

游戏日志在界面扫描实例时顺带清理：

- `.minecraft/logs/` 下超过 7 天的 `*.log.gz` 和旧 `*.log`（`latest.log` 始终保留）
- `.minecraft/crash-reports/` 下超过 7 天的 `crash-*.txt`

清理数量会写进启动器日志，不会动 7 天内的文件。

## CLI 使用

```powershell
# 列出实例
cmcl.exe list --mc-dir "$env:APPDATA\.minecraft"

# 只看解析结果，不启动（排查参数问题非常有用）
cmcl.exe inspect --version 1.20.1 --mc-dir "$env:APPDATA\.minecraft"

# 启动
cmcl.exe launch --version 1.20.1 --mc-dir "$env:APPDATA\.minecraft" --username Steve

# 指定 Java、内存、分辨率
cmcl.exe launch --version 1.20.1 --java "C:\Program Files\Java\jdk-17\bin\java.exe" --max-mem 8192 --width 1280 --height 720

# 检测 Java
cmcl.exe java

# 查看可下载的版本 / 安装原版游戏
cmcl.exe versions --type release
cmcl.exe download --version 1.20.1 --name 1.20.1

# 校验并修复实例文件
cmcl.exe repair --version 1.20.1

# 查看 / 下载 OpenJDK
cmcl.exe jdk
cmcl.exe jdk --install 21

# 查看 / 安装模组加载器
cmcl.exe loader --loader fabric --game 1.20.1
cmcl.exe loader --loader neoforge --game 1.21.1 --version 21.1.264 --name 1.21.1-NeoForge
```

日志写在 `cmcl.exe` 同目录的 `logs/cmcl.log`。

## 目录结构

```
src/core/      与 Minecraft 无关的基础设施（Result、日志、编码、路径、进程、解压、加密）
src/mc/        版本 JSON、实例扫描与管理、Java/JDK、启动参数、下载与修复、运行诊断、微软登录
src/ui/        WebView2 宿主、JSON-RPC 桥
src/app/       CLI
web/index.html 页面外壳（导航 / 面包屑 / 状态栏）
web/app.css    全部样式
web/js/        ES 模块前端
  app.js         启动入口
  rpc.js         JSON-RPC 客户端与事件订阅
  store.js       启动设置、版本列表、上次选择
  game-store.js  运行状态与日志缓冲
  router.js      hash 路由与页面生命周期
  components/    实例列表、日志/运行总结
  pages/         启动、版本管理、实例详情、下载、设置
tests/         单元测试
```

`core` 和 `mc` 不依赖任何 UI 类型；前端只通过 JSON-RPC 调用核心，不直接碰文件系统或进程。每个页面模块导出 `mount(ctx, params)` / `unmount()`，后续新增页面只需要加一条路由。

## 兼容性处理

这几条是实际跑 1.6.4 / 1.21.10 时踩出来的，已经固化在代码里：

- **系统开启 UTF-8 beta（ACP=65001）时，Java 8 解析不了中文 classpath**：进程工作目录设为 `.minecraft`，classpath 使用相对路径，命令行里不出现中文。
- **Java 8 的 native 加载器同样处理不了中文路径**：natives 释放到 `%LOCALAPPDATA%\CraftLauncher\natives\<hash>\`（纯 ASCII），再把绝对路径传给 `-Djava.library.path`。
- **OptiFine 等 jar mod 会让 FML 报 binary discrepancy**：默认带上 `-Dfml.ignoreInvalidMinecraftCertificates=true -Dfml.ignorePatchDiscrepancies=true`。
- **Java 7/8 的中文日志是 GBK**：子进程输出按行先校验 UTF-8，失败再用 GBK/系统代码页解码，日志和 UI 统一为 UTF-8。
- **Forge 1.6.1 - 1.7.2 需要 Java 7**：自动选择 Java 7，没有时回退 Java 8 并告警。
- **最低 Java 版本校验**：显式 `--java` 和自动选择都会检查 `javaVersion.majorVersion`；低于要求直接拒绝启动，避免用 Java 8 打开 1.20.5+ 后崩溃。
- **JDK 自带的 `jre\bin\java.exe` 会去重**：只保留 JDK 根目录下的 `bin\java.exe`，避免 Java 列表出现同一套运行时的两个条目。
- **现代 Forge/NeoForge 的 module path 必须和 classpath 使用同一组绝对路径**：Java 9+ 使用绝对路径；Java 7/8 仍使用相对路径以规避 UTF-8 ACP 问题。绝对路径修复了 `bootstraplauncher was already on the JVMs module path`。
- **不能按 token 去重 JVM 参数**：`--add-opens` / `--add-exports` 会重复出现，去掉重复 flag 会把参数值孤立成主类。

## 已知限制（当前阶段）

- 只支持 Windows x64
- natives 解压调用系统 `tar.exe`
- 命令行超过约 30000 字符会警告；超长 classpath 需要后续加 JavaWrapper / `@argfile`
- 界面还没有文件夹选择器，游戏目录需要手动输入
- 资源搜索目前只接入 Modrinth，CurseForge 尚未接入
- 整合包已支持 Modrinth `.mrpack` 和带 `.minecraft/versions` 的 PCL/普通导出 zip；CurseForge 官方 `manifest.json` 需要 API Key，暂不自动下载模组
- MultiMC / Prism 的 `mmc-pack.json`（不含版本 JSON）暂时按普通 `.minecraft` 导出处理，缺少版本文件时会提示
- Fabric API / OptiFine 加载器安装尚未接入（M4 已覆盖 Forge / NeoForge / Fabric / Quilt）
- 暂不支持手动指定默认 Java；第三方 Java 必须通过它自己的卸载程序移除
- 旧的配置、缓存和已下载 JDK 仍保存在 `%LOCALAPPDATA%\CraftLauncher\`，改名后不会丢失
- 正版登录依赖 Mojang API 白名单；未审核的 Azure 应用会在最后一步返回 403（见「Microsoft 登录」）
- 暂不支持外置登录（authlib-injector）
- 实例隔离只切换游戏目录；资源管理会按需创建 `mods` / `resourcepacks` / `shaderpacks` / `datapacks`，其余子目录仍由游戏或用户创建
- 没有做 PCL 的 `McLaunchPrecheck`；将来如果基于本项目分发，需要单独处理

## 下一步

- ~~**M3**：下载器（库 / assets / client jar、BMCLAPI 镜像、哈希校验、修复）~~ 已完成
- ~~**M4**：加载器安装（Forge / NeoForge / Fabric / Quilt）~~ 已完成；Fabric API / OptiFine 待接入
- ~~**M5**：微软 device code 登录~~ 已完成；authlib-injector 外置登录待接入
- **M6**：Fabric API / OptiFine 安装、CurseForge 资源源、Mod 更新检查
  - Modrinth 搜索 / 版本 / 下载、实例资源管理、Modrinth `.mrpack` 与 PCL/.minecraft 整合包导入均已完成
- **M7**：CurseForge 整合包导入与整合包导出

## 许可证

本项目以 **GNU General Public License v3.0** 发布，完整文本见 [LICENSE](LICENSE)。

Copyright (C) 2026 CMCL contributors

你可以自由使用、修改和分发本项目；如果分发修改后的版本，需要继续以 GPL-3.0 公开源代码，并保留版权声明。本项目不提供任何担保。

第三方组件遵循各自的许可证：

- nlohmann/json — MIT
- zlib — zlib License
- Microsoft Edge WebView2 SDK — Microsoft 软件许可条款
- Adoptium Temurin JDK、Minecraft 版本清单 / 资源 — 由各自提供方授权，不包含在本仓库中

Minecraft 是 Mojang Studios 的商标。本项目与 Mojang / Microsoft 没有隶属关系。
