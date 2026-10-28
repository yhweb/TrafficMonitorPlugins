# TrafficMonitorPlugins
这是用于[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)的插件。

## 插件下载

请点击以下链接转到插件下载页面：

[TrafficMonitor 插件下载](./download/plugin_download.md)

## 插件使用说明

根据TrafficMonitor的版本（x86为32位，x64为64位）选择对应版本的插件，下载后解压可得到dll文件，下载后将插件dll放到TrafficMonitor程序所在目录下的`plugins`目录下：

![image-20221013203124953](images/image-20221013203124953.png)

重新启动TrafficMonitor后可以在“选项”——“常规设置”——“插件管理”中看到所有的插件：

![image-20221013203353499](images/image-20221013203353499.png)

要使插件项目显示到任务栏中，请在任务栏窗口上点击鼠标右键，选择“显示设置”。

![image-20221013203527593](images/image-20221013203527593.png)

![image-20221013203621714](images/image-20221013203621714.png)

此时，“显示设置”中会显示已加载的插件项目，勾选你希望显示在任务栏上的项目，点击确定即可。

关于更多插件使用的详细说明，请参考以下链接：

[插件功能 · zhongyang219/TrafficMonitor Wiki (github.com)](https://github.com/zhongyang219/TrafficMonitor/wiki/插件功能)

## 如何开发插件

关于如何开发TrafficMonitor，请参考以下链接：

[插件开发指南 · zhongyang219/TrafficMonitor Wiki (github.com)](https://github.com/zhongyang219/TrafficMonitor/wiki/插件开发指南)

## 本次功能改进

> 对应提交 `da0f21c`（2026-09-12）：添加期货买多卖空设置、WebView 判断、显示别名、滚动显示。主要涉及 `Plugins/StockV2` 插件。

### 1. 期货成本价与买卖方向（买多 / 卖空）

- 自选列表新增「成本价」「方向」两列，可逐条为持仓设置成本价与交易方向。
- 「方向」列仅期货品种（`NF` / `HF` / `CFF` 市场类型）可编辑，普通股票置灰禁止设置（股票只能做多）。
- 依据「当前价 − 成本价」实时计算相对盈亏：
  - 盈亏额（价差）：`当前价 - 成本价`，带 `+` / `−` 符号；
  - 盈亏幅度（涨跌幅）：盈亏额相对成本价的百分比；
  - 做空方向下盈亏方向自动反转（价跌盈利、价涨亏损）。
- 新增三个显示开关：`显示成本` / `显示价差` / `显示涨跌幅`，未设置成本价时默认禁用。

### 2. 显示别名

- 新增「显示别名」开关，支持以自定义别名替代股票名称在任务栏显示。

### 3. 滚动显示

- 新增「滚动显示」开关，可设置「每页 N 只、间隔 M 秒」。
- 当自选股票数量超过一页时，按设定间隔自动轮换显示；不超过一页时固定显示全部。
- 默认每页 `2` 只、间隔 `5` 秒（最小间隔 1 秒）。
- 滚动在 `DataRequired()` 中推进起始下标，并实时刷新每个显示 item 的股票映射（宿主加载时只缓存一次 item 指针，故必须在刷新回调里更新映射）。

### 4. WebView 后端可用性判断

- 创建 WebView 前先检测 Edge 后端是否可用（`wxWebView::IsBackendAvailable(wxWebViewBackendEdge)`），不可用时优雅降级，避免创建失败或崩溃。

## 编译环境

### 工具链

- IDE：Visual Studio 2026（`PlatformToolset v145`，即 MSVC 14.5x）
- 工程类型：MFC 动态链接库（`ConfigurationType=DynamicLibrary` + `UseOfMfc=Dynamic`）
- 字符集：Unicode
- 目标平台：`Win32` / `x64` / `ARM64EC`，配置 `Debug` / `Release`（`ARM64EC` 需在 VS Installer 中安装「适用于 ARM64EC 的 MSVC 生成工具」；未安装时把解决方案平台切到 `x64`，否则会报 MSB8020）

### 依赖项

| 依赖 | 说明 |
| --- | --- |
| wxWidgets 3.2.10 | 工程内硬编码为 `F:\wxWidgets-3.2.10`，需自行编译安装；路径不同时修改 `.vcxproj` 的 `IncludePath` / `LibraryPath` |
| Microsoft.Web.WebView2 1.0.3912.50 | NuGet 包，已内嵌于 `packages/` 目录，无需额外下载 |
| utilities 静态库 | 由 `utilities/` 工程生成，输出 `lib/x64/{Debug,Release}/utilities.lib` |
| `include/PluginInterface.h` | TrafficMonitor 插件 API（当前 `v7`） |
| .NET Framework 4.7.2 目标包 | `HardwareMonitor` 是 C++/CLI 工程，需在 VS Installer 中勾选「.NET Framework 4.7.2 目标包」；未安装时该工程会报 MSB3644，其余 12 个工程不受影响 |

### 编译步骤

1. 用 VS2026 打开 `TrafficMonitorPlugins.sln`（默认平台若停在 `ARM64EC`，先切到 `x64`）。
2. 确认 wxWidgets 路径（默认 `F:\wxWidgets-3.2.10`）与本机一致，不一致则修改 `StockV2.vcxproj` / `PluginTester.vcxproj`。
3. 选择 `x64` + `Release`，生成 `StockV2` 工程（会先编译依赖的 `utilities` 静态库）。
4. 产物位于 `bin/x64/Release/StockV2.dll`，运行时需同目录的 `WebView2Loader.dll`。

### 说明

- 插件调试可用 `PluginTester` 工程（基于 wxWidgets 的宿主模拟器，生成 `PluginTester.exe`）。
- 配置通过 `wxFileConfig` 持久化为 `StockV2.ini`（位于插件 DLL 所在目录），键值读写见 `Plugins/StockV2/DataManager.cpp` 的 `InitConfig` / `SaveConfig`。

## 插件来源与上游同步

部分插件合并自独立仓库，后续需要跟进上游更新时，按下列地址拉取。

### WeatherPro（天气插件）

| 项 | 值 |
| --- | --- |
| 上游仓库 | https://github.com/Haojia521/TMP-WeatherPro |
| 发布页 | https://github.com/Haojia521/TMP-WeatherPro/releases |
| 镜像仓库 | https://gitee.com/Haojia521/TMP-WeatherPro |
| 当前合并版本 | v1.4.0（上游 `main` @ `1e29c40`，见 `Plugins/WeatherPro/version.json`） |
| 上游基线分支 | `sync/weatherpro-1.4.0` |
| 本地功能分支 | `feat/dual-line-always` |

本仓库对应结构：

- `Plugins/WeatherPro` —— 插件本体（MFC DLL，源自上游 `WeatherPro/`）
- `Plugins/WPCore` —— 静态库依赖（源自上游 `WPCore/`）
- `Plugins/dependencies` —— 头文件依赖（httplib / jwt-cpp / simpleini / yyjson / spdlog）
- `Plugins/third_party` —— 预编译静态库（OpenSSL / zlib），仅 x64

#### 分支约定

本地改动不压在上游代码里，而是拆成「基线 + 功能」两层，上游发版后只需重放一层：

| 分支 | 内容 | 维护方式 |
| --- | --- | --- |
| `sync/weatherpro-<版本>` | 纯上游快照 + 本仓库结构适配，**不含任何本地功能** | 每版新建一个，只加一个 `sync:` 提交 |
| `feat/dual-line-always` | 只叠「任意位置强制双行」一个提交（8 文件 / +25 −3） | `git rebase` 跟到新基线上 |
| `yh` | 日常开发主分支 | 分支上验证通过后再合入 |

> 当前 `sync/weatherpro-1.4.0` 与上游 `main` 的差异**仅 8 个文件**（清单见「必须保留的结构适配」），其余源码逐字节一致；`feat/dual-line-always` 与基线的差异也只碰那 8 个功能文件，两组文件互不重叠，所以 rebase 不会冲突。

#### 跟随上游：改两行后整段跑

```bash
# ===== 跟新版本时只改这两行 =====
V=v1.5.0
BASE="sync/weatherpro-1.5.0"
# ================================

cd /e/code/TrafficMonitor_Plugins
git remote add wp https://github.com/Haojia521/TMP-WeatherPro.git 2>/dev/null
git fetch wp --tags
REF=wp/main                                      # 想钉某个 tag 就写 refs/tags/$V
git show "$REF":version.json | head -6           # 先确认 version 真的是 $V
OLDBASE=$(git rev-parse feat/dual-line-always^)  # 旧基线 = 功能分支的父提交

# ① 以旧基线为底新建基线分支
git checkout -B "$BASE" "$OLDBASE"

# ② 备份结构适配文件（被上游覆盖后要放回来）
mkdir -p /tmp/_wp_keep
cp Plugins/WeatherPro/PluginInterface.h          /tmp/_wp_keep/
cp Plugins/WeatherPro/WeatherPro.vcxproj         /tmp/_wp_keep/
cp Plugins/WeatherPro/WeatherPro.vcxproj.filters /tmp/_wp_keep/
cp Plugins/WPCore/WPCore.vcxproj                 /tmp/_wp_keep/
cp Plugins/WPCore/WPCore.vcxproj.filters         /tmp/_wp_keep/
cp Plugins/WPCore/utils.cpp                      /tmp/_wp_keep/
cp Plugins/WPCore/utils.h                        /tmp/_wp_keep/

# ③ 解出上游源码整体覆盖（cp 不删除本地独有文件与未跟踪目录）
rm -rf /tmp/_wp_new && mkdir -p /tmp/_wp_new
git archive "$REF" WeatherPro WPCore version.json | tar -x -C /tmp/_wp_new
cp -rf /tmp/_wp_new/WeatherPro/. Plugins/WeatherPro/
cp -rf /tmp/_wp_new/WPCore/.    Plugins/WPCore/
cp -f  /tmp/_wp_new/version.json Plugins/WeatherPro/version.json

# ④ 放回结构适配，提交基线
cp -f /tmp/_wp_keep/PluginInterface.h           Plugins/WeatherPro/
cp -f /tmp/_wp_keep/WeatherPro.vcxproj          Plugins/WeatherPro/
cp -f /tmp/_wp_keep/WeatherPro.vcxproj.filters  Plugins/WeatherPro/
cp -f /tmp/_wp_keep/WPCore.vcxproj              Plugins/WPCore/
cp -f /tmp/_wp_keep/WPCore.vcxproj.filters      Plugins/WPCore/
cp -f /tmp/_wp_keep/utils.cpp                   Plugins/WPCore/
cp -f /tmp/_wp_keep/utils.h                     Plugins/WPCore/
git add Plugins/WeatherPro Plugins/WPCore
git commit -m "sync: 同步 WeatherPro 上游 $V 并适配本仓库结构"

# ⑤ 功能提交重放到新基线
git checkout feat/dual-line-always
git rebase --onto "$BASE" "$OLDBASE" feat/dual-line-always

# ⑥ 校验：应仍是 8 files changed, 25 insertions(+), 3 deletions(-)
git diff --stat "$BASE" feat/dual-line-always

# ⑦ 编译验证（见下方「生成验证」）
```

冲突只在第 ⑤ 步出现，且只会落在那 8 个功能文件上。处理：`git status` 看文件 → 改完 `git add <文件> && git rebase --continue`；放弃用 `git rebase --abort`。

#### 必须保留的结构适配（共 8 个文件）

覆盖上游会带回上游原版，这些**不要跟着上游走**，否则编译失败或丢功能：

| 文件 | 本仓库做法 | 上游原版 |
| --- | --- | --- |
| `Plugins/WeatherPro/PluginInterface.h` | `GetAPIVersion()` 返回 **8** | 返回 7 |
| `Plugins/WeatherPro/WeatherPro.vcxproj` | `PlatformToolset` **v145**；头文件路径指向 `..\dependencies\{simpleini,yyjson,httplib,spdlog,jwt}`、`..\third_party\{openssl,zlib}\include` | v143，按上游 `WeatherPro/` 布局 |
| `Plugins/WeatherPro/WeatherPro.vcxproj.filters` | 与上面配套 | 同上 |
| `Plugins/WPCore/WPCore.vcxproj` | `PlatformToolset` **v145**；额外加 `..\dependencies\spdlog\` | v143，无 spdlog |
| `Plugins/WPCore/WPCore.vcxproj.filters` | 与上面配套 | 同上 |
| `Plugins/WPCore/utils.cpp` | 多 **76 行**：从 Windows 证书存储抓根证书喂给 OpenSSL（`getWindowsRootCaPem`），否则 HTTPS 报 `X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN` | 无 |
| `Plugins/WPCore/utils.h` | 声明 `getWindowsRootCaPem()` | 无 |
| `Plugins/WeatherPro/version.json` | 末尾带换行 | 无换行 |

> 若上游某天把 `GetAPIVersion()` 升到 8，或自己也做了证书存储适配，对应行可去掉。

#### 生成验证

```powershell
# 必须先清代理变量，否则 MSBuild 报 MSB6001
Remove-Item -Path Env:http_proxy,Env:https_proxy,Env:no_proxy,Env:HTTP_PROXY,Env:HTTPS_PROXY,Env:NO_PROXY -ErrorAction SilentlyContinue
& 'D:\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe' `
  TrafficMonitorPlugins.sln -t:WeatherPro -p:Configuration=Release -p:Platform=x64 -m -v:m -nologo
```

- **先关掉 `TrafficMonitor.exe`**，否则 C/C++ 其实 0 错误，但 post-build 拷贝会报 `另一个程序正在使用此文件` + `error MSB3073`（DLL 被宿主占用）。
- 成功标志：`[post-build] copied "WeatherPro.dll" -> "E:\code\TrafficMonitor_bin\plugins"`，退出码 0。
- 无关警告：`LNK4099`（第三方 OpenSSL 缺 `ossl_static.pdb`），可忽略。

#### 常用查询

```bash
git fetch wp --tags
git log --oneline wp/main -10                     # 上游最近提交
git show refs/tags/v1.4.0:version.json | head -6  # 核对 tag 版本（tag 可能滞后）
git diff --stat sync/weatherpro-1.4.0 <新基线>    # 这一版上游改了什么
git diff --text sync/weatherpro-1.4.0 <新基线> -- Plugins/WeatherPro/WeatherPro.rc  # .rc 是 UTF-16LE
git log --oneline --graph --decorate -6           # 当前分支拓扑
```

> `version.json` 在上游仓库根目录，本仓库放在 `Plugins/WeatherPro/version.json`，同步时复制过去。
> `Plugins/WeatherPro/{LICENSE.md,README.md,WeatherPro,docs,images,x64,bin}` 是上游 `WeatherPro/` 里没有的内容，覆盖不受影响（`cp -rf` 不删除）。
> 第 ⑦ 步编译报错时，先确认 `Plugins/dependencies/spdlog` 仍在（它已被 git 跟踪，正常不会丢）。

> 说明：上游仅提供 x64 的 OpenSSL/zlib 静态库，因此本仓库的 WeatherPro/WPCore 只映射 x64 与 ARM64EC，没有 x86 配置。

### StockV2（股票插件）

| 项 | 值 |
| --- | --- |
| 上游仓库 | https://github.com/CListery/TrafficMonitorPlugins |
| 上游分支 | `clistery` |
| 源码目录 | https://github.com/CListery/TrafficMonitorPlugins/tree/clistery/Plugins/StockV2 |
| 本仓库位置 | `Plugins/StockV2` |
| 当前基线 | 上游 `clistery` @ `c44a3f2` |

> 上游仓库就是本仓库的 `upstream` remote（`CListery/TrafficMonitorPlugins`），因此可直接用 git 对比，无需额外 clone。

同步上游的步骤：

```bash
git fetch upstream clistery
git diff upstream/clistery -- Plugins/StockV2        # 看上游与当前的差异
git log upstream/clistery --oneline -- Plugins/StockV2  # 看上游该目录的提交历史
```

1. 用上面的 `git diff` 确认上游改了哪些文件。
2. 逐文件合入 `Plugins/StockV2`，**注意**本仓库已在上游基础上做了较多本地改动（期货买卖方向、显示别名、滚动显示、WebView 判断、工具集升级 v145、输出目录调整），合并时保留这些本地改动。
3. 上游 `StockV2.vcxproj` 用 `PlatformToolset v143`，本仓库统一为 `v145`，不要被覆盖回去。
4. 生成验证：`MSBuild TrafficMonitorPlugins.sln -t:StockV2 -p:Configuration=Release -p:Platform=x64`。
5. 若上游新增了 `res/` 资源或 `packages/` 依赖，一并补入。

> 说明：本仓库的 StockV2 依赖 `utilities` 静态库与 wxWidgets（默认路径 `F:\wxWidgets-3.2.10`），编译前确认路径与 `StockV2.vcxproj` 一致。
