# web

WebView2 前端。构建时由 CMake 复制到 `cmcl-ui.exe` 同目录的 `web/`，通过虚拟主机 `https://app.craftlauncher.local/` 加载，入口固定为 `index.html#/launch`。

## 结构

- `index.html`：应用外壳（左侧导航、页面插槽、面包屑、顶部操作、状态栏）
- `app.css`：全部样式
- `icon.png` / `favicon.png`：程序图标与网页图标
- `js/app.js`：ES 模块入口，初始化 store、game-store 和路由；处理整包拖入事件
- `js/rpc.js`：JSON-RPC 客户端与事件订阅
- `js/store.js`：启动设置、游戏目录、实例列表、上次选择（localStorage）
- `js/game-store.js`：运行状态、退出码、总结和日志缓冲
- `js/download-store.js`：下载任务状态、阶段加权进度、速度/剩余时间、取消（原版安装 / 修复 / JDK / 加载器 / Modrinth 资源 / 整合包导入）
- `js/ui.js`：面包屑/顶部按钮/侧栏插槽、确认框、输入框、实例名校验
- `js/router.js`：hash 路由
- `js/components/instance-list.js`：侧栏实例列表与搜索
- `js/components/log-view.js`：实时日志、崩溃分析卡片与未知特征提示
- `js/components/crash-report.js`：崩溃报告导出窗口（复制 / 保存 / 打开 crash-reports）
- `js/pages/launch.js`：启动页
- `js/pages/instances.js`：版本管理页
- `js/pages/instance-detail.js`：实例详情与 M2 操作
- `js/pages/resources.js`：实例资源管理（Mod / 资源包 / 光影包 / 数据包）
- `js/pages/download.js`：PCL 风格下载页（原版安装 / 加载器 / Modrinth 的 Mod / 整合包 / 数据包 / 资源包 / 光影包）
- `js/pages/settings.js`：设置页（常规 / Microsoft 账号 / Java 管理与 OpenJDK 下载 / 日志 / 关于）

## 路由

| 路由 | 页面 |
| --- | --- |
| `#/launch` | 启动 |
| `#/instances` | 版本管理 |
| `#/instances/<id>` | 实例详情（id 会做 URL 编码） |
| `#/instances/<id>/resources` | 实例资源管理（Mod / 资源包 / 光影包 / 数据包） |
| `#/download`、`#/download/install` | 原版游戏安装 |
| `#/download/mod`、`#/download/modpack`、`#/download/datapack`、`#/download/resourcepack`、`#/download/shader` | 社区资源搜索 |
| `#/settings`、`#/settings/general`、`#/settings/account`、`#/settings/java`、`#/settings/logs`、`#/settings/about` | 设置 |

页面模块约定：

```js
export function mount(ctx, params) { /* 向 ctx.page / 侧栏插槽挂载 DOM */ }
export function unmount() { /* 取消订阅、递增异步请求序号 */ }
```

`ctx` 提供 `page`、`navigate(hash)` 和 `setStatus(text)`。新增页面时在 `router.js` 的 `ROUTES` 里加一条匹配规则即可。

## JSON-RPC 接口

C++ 侧实现在 `src/ui/host.cpp`：

- `app.info` → 版本、默认游戏目录、日志路径
- `app.open_url` → 用系统浏览器打开 HTTP/HTTPS 链接
- `auth.status` → 本地 Microsoft 账号状态（只返回名字 / UUID / 过期时间，不返回令牌）
- `auth.device_start` → 申请设备码（device_code 只保存在宿主端）
- `auth.device_poll` → 轮询登录结果；成功时由宿主加密保存账号
- `auth.logout` → 删除本地账号文件
- `instance.list` → 扫描 `versions/`，返回类型、加载器、主类、继承关系、隔离状态
- `instance.inspect` → 解析版本、选 Java、拼参数（不启动）
- `instance.rename` → 重命名版本目录/JSON/JAR，并修正子版本引用
- `instance.delete` → 移到 Windows 回收站
- `instance.open_folder` → 打开实例目录
- `instance.open_saves` → 打开当前实例的存档目录（完整隔离 / 单独隔离 / 共享）
- `instance.settings.get` / `instance.settings.set` → 读写 `versions/<id>/craftlauncher.json`
- `instance.resources.list` → 列出实例的 Mod / 资源包 / 光影包 / 数据包，返回实际目录、是否共享、文件状态
- `instance.resources.toggle` → 通过追加/移除 `.disabled` 启用或禁用资源
- `instance.resources.delete` → 把单个资源移到 Windows 回收站
- `instance.resources.open` → 打开对应资源目录
- `java.list` → 检测 Java（含启动器下载的 JDK，标记 `managed`）
- `java.available` → 获取 Adoptium Temurin 版本列表（可强制刷新）
- `java.download` → 下载并安装 OpenJDK（SHA-256 校验，镜像优先）
- `java.delete` → 删除启动器下载的 JDK（移到回收站）
- `java.uninstall` → 打开第三方 Java 自带的卸载程序，或 Windows“应用和功能”
- `download.versions` → 获取版本清单（支持镜像和缓存）
- `download.install` → 安装原版游戏（库 / assets / client jar）
- `download.repair` → 校验并修复实例文件
- `download.cancel` → 取消当前下载
- `download.mirrors` / `download.mirrors.refresh` → 查看/刷新镜像列表
- `loader.versions` → 获取 Forge / NeoForge / Fabric / Quilt 在某游戏版本下的可用版本
- `loader.install` → 安装加载器（必要时先装原版，Forge/NeoForge 调用官方安装器）
- `modrinth.search` → 搜索 Mod / 整合包 / 数据包 / 资源包 / 光影包，支持游戏版本、加载器、排序、分页
- `modrinth.versions` → 获取项目版本列表（按游戏版本和加载器过滤，返回主文件信息）
- `modrinth.download` → 下载指定版本到实例有效游戏目录（`mods` / `resourcepacks` / `shaderpacks` / `datapacks` / `modpacks`），返回文件路径
- `modpack.install` → 导入整合包：支持 Modrinth `version_id`（先下载 `.mrpack`），或本地 `.mrpack` / `.zip` 路径；返回新实例 ID
- `dialog.open_modpack` → 打开本地整合包选择框，取消时返回空路径
- 事件：`files.dropped` → 页面拖入文件后由 WebView2 `postMessageWithAdditionalObjects` 传回真实路径
- `game.launch` → 准备并启动游戏，返回 PID
- `game.stop` → 结束当前游戏
- `game.status` → 当前是否运行、PID
- `crash.save_report` → 把前端报告文本保存到 `crash-reports/CMCL-崩溃分析-*.txt`
- `crash.open_folder` → 打开 `crash-reports` 目录
- 事件：`game.log`（逐行日志）、`game.exit`（退出码、是否手动停止）、`game.summary`（崩溃分析：`findings`、`unknown`、`context`、`log_tail`、`crash_report_tail`）、`download.progress`（下载阶段/文件/字节进度）

前端只通过 JSON-RPC 与核心交互，不直接访问文件系统或进程。
