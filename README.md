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

- IDE：Visual Studio 2022（`PlatformToolset v143`，即 MSVC 14.3x）
- 工程类型：MFC 动态链接库（`ConfigurationType=DynamicLibrary` + `UseOfMfc=Dynamic`）
- 字符集：Unicode
- 目标平台：`Win32` / `x64`，配置 `Debug` / `Release`

### 依赖项

| 依赖 | 说明 |
| --- | --- |
| wxWidgets 3.2.10 | 工程内硬编码为 `F:\wxWidgets-3.2.10`，需自行编译安装；路径不同时修改 `.vcxproj` 的 `IncludePath` / `LibraryPath` |
| Microsoft.Web.WebView2 1.0.3912.50 | NuGet 包，已内嵌于 `packages/` 目录，无需额外下载 |
| utilities 静态库 | 由 `utilities/` 工程生成，输出 `lib/x64/{Debug,Release}/utilities.lib` |
| `include/PluginInterface.h` | TrafficMonitor 插件 API（当前 `v7`） |

### 编译步骤

1. 用 VS2022 打开 `TrafficMonitorPlugins.sln`。
2. 确认 wxWidgets 路径（默认 `F:\wxWidgets-3.2.10`）与本机一致，不一致则修改 `StockV2.vcxproj` / `PluginTester.vcxproj`。
3. 选择 `x64` + `Release`，生成 `StockV2` 工程（会先编译依赖的 `utilities` 静态库）。
4. 产物位于 `bin/x64/Release/StockV2.dll`，运行时需同目录的 `WebView2Loader.dll`。

### 说明

- 插件调试可用 `PluginTester` 工程（基于 wxWidgets 的宿主模拟器，生成 `PluginTester.exe`）。
- 配置通过 `wxFileConfig` 持久化为 `StockV2.ini`（位于插件 DLL 所在目录），键值读写见 `Plugins/StockV2/DataManager.cpp` 的 `InitConfig` / `SaveConfig`。

