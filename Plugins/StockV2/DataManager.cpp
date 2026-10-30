#include "pch.h"
#include "DataManager.h"
#include <wx/filename.h>
#include <wx/config.h>
#include "TaskManager.h"
#include <iomanip>
#include <wx/webrequest.h>
#include <wx/wx.h>
#include <random>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <wx/hashmap.h>
#include "StockV2.h"

// 链接库（获取模块路径需要）
#pragma comment(lib, "shlwapi.lib")

LDataManager LDataManager::m_instance;

constexpr auto WEB_USERAGENT = _T("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36 Edg/135.0.0.0");

// 5 日分时接口（走势图数据源）：股票走 5 分钟 K 线，期货走四日分时。
// 期货的 symbol 是运行期拼的（要去掉 nf_ / CFF_RE_ 前缀），所以只固定前后缀。
static const wxString HISTORY_STOCK_KLINE_URL = wxT("https://quotes.sina.cn/cn/api/jsonp.php/var%20t1=/CN_MarketDataService.getKLineData?symbol=");
static const wxString HISTORY_FUTURES_URL_PREFIX = wxT("https://stock2.finance.sina.com.cn/futures/api/jsonp.php/var%20t5");
static const wxString HISTORY_FUTURES_URL_MID = wxT("=/InnerFuturesNewService.getFourDaysLine?symbol=");

namespace
{
    static const auto KEY_CFG_ALL_STOCK_DATA = "/stock/all_data";
    static const auto KEY_CFG_REALTIME_REFRESH_FREQ = "/stock/realtime_refresh_freq";
    static const auto KEY_CFG_DISPLAY_COLOR = "/stock/is_display_color";
    static const auto KEY_CFG_DISPLAY_NAME = "/stock/is_display_name";
    static const auto KEY_CFG_PRIORITY_DISPLAY_CHANGE = "/stock/is_priority_display_changed";
    static const auto KEY_CFG_DISPLAY_COST = "/stock/is_display_cost";
    static const auto KEY_CFG_DISPLAY_COST_PROFIT_PRICE = "/stock/is_display_cost_profit_price";
    static const auto KEY_CFG_DISPLAY_COST_PROFIT_PERCENT = "/stock/is_display_cost_profit_percent";
    static const auto KEY_CFG_DISPLAY_ALIAS = "/stock/is_display_alias";
    static const auto KEY_CFG_DRAW_HISTORY_GRAPH = "/stock/is_draw_history_graph";
    static const auto KEY_CFG_HISTORY_DAYS = "/stock/history_days";
    static const auto KEY_CFG_HISTORY_GRAPH_MODE = "/stock/history_graph_mode";
    static const auto KEY_CFG_HISTORY_GRAPH_STYLE = "/stock/history_graph_style";
    static const auto KEY_CFG_GRAPH_COLOR_DARK = "/stock/history_graph_color_dark";
    static const auto KEY_CFG_GRAPH_COLOR_LIGHT = "/stock/history_graph_color_light";
    static const auto KEY_CFG_GRAPH_DASHED_BOX = "/stock/history_graph_dashed_box";
static const auto KEY_CFG_GRAPH_SHOW_SEP_LINE = "/stock/history_graph_show_sep_line";
static const auto KEY_CFG_GRAPH_SEP_COLOR_DARK = "/stock/history_graph_sep_color_dark";
static const auto KEY_CFG_GRAPH_SEP_COLOR_LIGHT = "/stock/history_graph_sep_color_light";
    static const auto KEY_CFG_SCROLL_ENABLE = "/stock/is_scroll_enable";
    static const auto KEY_CFG_SCROLL_PAGE_SIZE = "/stock/scroll_page_size";
    static const auto KEY_CFG_SCROLL_INTERVAL = "/stock/scroll_interval";
    static const auto KEY_CFG_KLINE_SIZE_W = "/stock/kline/size/w";
    static const auto KEY_CFG_KLINE_SIZE_H = "/stock/kline/size/h";

    long long generateRandomDouble()
    {
        // 静态随机引擎，只初始化一次，性能更好
        static std::mt19937_64 rng(std::random_device{}());

        // 第一个随机数范围 [1, 1234567890]
        std::uniform_int_distribution<long long> distA(1, 1234567890LL);
        long long a = distA(rng);

        // 第二个随机数范围 [1, 9876543210]
        std::uniform_int_distribution<long long> distB(1, 9876543210LL);
        long long b = distB(rng);

        return a + b;
    }

    // ==================== 闭市退避 + 开盘前探针 ====================
    //
    // 动机：交易时段之外行情不再变化，仍按用户频率（默认 10 秒）持续抓取纯属空转。
    //       只持 A股时一周 168 小时里约 148 小时是闭市（≈88%）。
    //
    // 做法：不维护交易日历，改用「行情内容是否变化」当判据 —— 连续多轮响应文本完全一致
    //       就判定闭市并拉长刷新间隔。好处是周末/法定节假日/临时休市/混市场全自动正确，
    //       也不会因为日历写错而把行情饿死。
    //
    // 唯一副作用是开盘后要等一个退避周期才发现，所以再用「开盘前探针」兜住：
    //       在这些时刻的前几分钟把手表拨回用户频率，开盘即可秒级刷新。
    //       全部按北京时间判断。

    // 连续多少轮行情完全不变才升一档退避。
    // 取 6 而非 1 是为了躲开盘中偶发的短暂停顿（冷门品种一两分钟没成交），
    // 万一误判，代价也只是间隔从 10s 变成 30s。
    constexpr int REFRESH_STILL_ROUNDS = 6;

    // 退避档位（秒）。0 = 保持用户设定频率；实际间隔 = max(用户频率, 该档位)。
    // 用 max 而不是直接赋值，是为了兼容用户把频率设得比档位还大（如 300 秒）的情形。
    static const unsigned int REFRESH_BACKOFF_SECS[] = {0, 30, 60, 120, 300};
    constexpr size_t REFRESH_BACKOFF_LEVELS = sizeof(REFRESH_BACKOFF_SECS) / sizeof(REFRESH_BACKOFF_SECS[0]);

    // 探针按市场分组，只在该组市场出现在自选列表里时才点亮
    enum class ProbeGroup
    {
        AShare = 0,
        CNFutures,
        HK,
        US,
        Count,
    };

    struct RefreshProbe
    {
        ProbeGroup group;
        int startMinute; // 北京时间当日分钟数（触发起点）
        int holdMinutes; // 命中后保持「用户频率」的时长
        const char *label;
    };

    // 各市场的开盘前探针。holdMinutes 取 65 分钟（覆盖开盘后那一段，之后行情自己在动，退避自然不再触发）。
    // 美股：夏令时 21:30、冬令时 22:30（北京时间）开盘，故 hold 给到 75 分钟覆盖两种情形，
    //       这样不必自己去判断美国夏令时。
    static const RefreshProbe REFRESH_PROBES[] = {
        {ProbeGroup::AShare, 9 * 60 + 25, 65, "A股早盘"},
        {ProbeGroup::AShare, 12 * 60 + 55, 65, "A股午盘"},
        {ProbeGroup::CNFutures, 8 * 60 + 55, 65, "国内期货日盘"},
        {ProbeGroup::CNFutures, 20 * 60 + 55, 65, "国内期货夜盘"},
        {ProbeGroup::HK, 9 * 60 + 25, 65, "港股早盘"},
        {ProbeGroup::HK, 12 * 60 + 55, 65, "港股午盘"},
        {ProbeGroup::US, 21 * 60 + 25, 75, "美股"},
    };

    // 市场类型 -> 探针分组；不属于任何分组的返回 Count
    ProbeGroup ToProbeGroup(MarketType type)
    {
        switch (type)
        {
        case MarketType::MarketType_SZ_SH:
        case MarketType::MarketType_BJ:
        case MarketType::MarketType_GN:
        case MarketType::MarketType_HY:
        case MarketType::MarketType_DY:
        case MarketType::MarketType_SI:
        case MarketType::MarketType_CNI:
        case MarketType::MarketType_REPO:
            return ProbeGroup::AShare;
        case MarketType::MarketType_NF:
        case MarketType::MarketType_CFF:
            return ProbeGroup::CNFutures;
        case MarketType::MarketType_HK:
        case MarketType::MarketType_HKAP:
            return ProbeGroup::HK;
        case MarketType::MarketType_US:
            return ProbeGroup::US;
        default:
            break;
        }
        return ProbeGroup::Count;
    }

    // 自选列表里实际出现过的分组（位图）。没持有的市场不开探针，免得白白恢复频率。
    unsigned int WatchlistGroupMask()
    {
        unsigned int mask = 0;
        const wxArrayString codes = g_data.GetAllCodes();
        for (size_t i = 0; i < codes.GetCount(); i++)
        {
            const ProbeGroup group = ToProbeGroup(market(codes[i]));
            if (group != ProbeGroup::Count)
            {
                mask |= (1u << static_cast<int>(group));
            }
        }
        return mask;
    }

    // 取北京时间。用 UTC 加 8 小时推算，不依赖宿主机的时区设置
    //（中国自 1991 年起无夏令时，UTC+8 是常数）。
    void GetBeijingTime(SYSTEMTIME &out)
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);

        ULARGE_INTEGER ul;
        ul.LowPart = ft.dwLowDateTime;
        ul.HighPart = ft.dwHighDateTime;
        ul.QuadPart += 8ULL * 60ULL * 60ULL * 10000000ULL;

        ft.dwLowDateTime = ul.LowPart;
        ft.dwHighDateTime = ul.HighPart;
        FileTimeToSystemTime(&ft, &out);
    }

    // 当前是否落在某个探针窗口内；命中则返回该探针，否则 nullptr。
    // 周末不点亮：四个市场全部休市，点亮只是白白恢复频率。
    // （按周一到周五的 09:25/12:55/08:55/20:55/21:25 计时；不区分法定节假日 —— 一旦未能点亮，
    //   退避档位最长也就 300 秒，只是开盘慢几分钟，不会漏数据。）
    const RefreshProbe *ActiveProbe(const unsigned int groupMask, const SYSTEMTIME &bj)
    {
        if (bj.wDayOfWeek == 0 || bj.wDayOfWeek == 6)
        {
            return nullptr;
        }

        const int nowSec = bj.wHour * 3600 + bj.wMinute * 60 + bj.wSecond;
        for (const RefreshProbe &probe : REFRESH_PROBES)
        {
            if ((groupMask & (1u << static_cast<int>(probe.group))) == 0)
            {
                continue;
            }
            const int startSec = probe.startMinute * 60;
            if (nowSec >= startSec && nowSec < startSec + probe.holdMinutes * 60)
            {
                return &probe;
            }
        }
        return nullptr;
    }

    void OnRefreshStockRealtimeData(LPVOID pParam)
    {
        wxArrayString codes = g_data.GetAllCodes();

        LLOG_INFO_F("OnRefreshStockRealtimeData: %d", codes.size());

        if (codes.empty())
        {
            return;
        }

        // https://w.sinajs.cn/rn=5769211975&list=sh601011,hf_BTC2606

        static const wxString url_prefix = "https://w.sinajs.cn/";

        wxArrayString params;
        params.push_back(L"rn=" + std::to_wstring(generateRandomDouble()));
        params.push_back(L"list=" + CommonUtils::StringHelper::vectorJoinString(codes, ","));

        wxString url = url_prefix + CommonUtils::StringHelper::vectorJoinString(params, "&");

        CString strHeaders = _T("Referer: https://finance.sina.com.cn");

        std::string jsonp;
        if (UtilNetHlp::GetURL(url.ToStdWstring(), jsonp, false, WEB_USERAGENT, strHeaders, strHeaders.GetLength()))
        {
            g_data.HandleStockRealtimeData(jsonp);

            // 闭市退避：拿本轮响应原文当「行情有没有在动」的判据。
            // 只比较不解析，所以不需要区分 GBK/UTF-8，也不需要碰任何 MarketType 的字段布局。
            // 注意：只有真正拿到响应才参与判断；网络失败时不喂签名，
            // 免得把网络故障当成「闭市」而把刷新间隔越拉越长。
            g_data.UpdateRefreshThrottle(jsonp);
        }

        // m_currentRequest = wxWebSession::GetDefault().CreateRequest(wxApp::GetInstance(), url);
        //
        // if (!m_currentRequest.IsOk())
        //{
        //     return;
        // }

        // m_currentRequest.SetHeader("User-Agent", WEB_USERAGENT);
        // m_currentRequest.SetHeader("Referer", "https://finance.sina.com.cn");

        //// Start the request (events will be sent on success or failure)
        // m_currentRequest.Start();

        //// 工作线程轮询等待，不影响界面
        // while (m_currentRequest.GetState() == wxWebRequest::State_Active)
        //{
        //     wxMilliSleep(10); // 避免 CPU 空转
        // }

        // if (m_currentRequest.GetState() == wxWebRequest::State_Completed)
        //{
        //     wxString jsonp = m_currentRequest.GetResponse().AsString();
        //     LLOG_DEBUG_F(wxT("resp: %s"), jsonp);
        // }
    }
}

// ==================== 5 日分时（走势图数据源） ====================
//
// 实时行情是一次请求拿全部自选（w.sinajs.cn），分时接口却是一只票一个请求，
// 所以独立成一个任务：TASK_KEY_STOCK_HISTORY_INIT 首拉一次，TASK_KEY_STOCK_HISTORY_REFRESH 每 60 秒刷新。
//
// 数据源（都是 JSONP，剥壳后交给 yyjson）：
//   股票：https://quotes.sina.cn/cn/api/jsonp.php/var%20t1=/CN_MarketDataService.getKLineData
//         ?symbol=sz002086&scale=5&ma=no&datalen=240
//         返回 /*<script>...</script>*/var t1=([{"day":"...","close":"0.491",...},...]); 取 close
//   期货：https://stock2.finance.sina.com.cn/futures/api/jsonp.php/var%20t5nf_M2701=
//         /InnerFuturesNewService.getFourDaysLine?symbol=M2701
//         返回 var t5nf_M2701=([[点,点],[点,点],...]); 外层是交易日，内层每个点是
//         [时间, 价格, 均价, 成交量, 持仓量, 昨收(仅首点), 日期(仅首点)]，取索引 1
//         每日 21:00 起算（含夜盘），组顺序即时间顺序，按组直接拼接即可

// 从 JSONP 文本里截出 JSON 数组：第一个 [ 到最后一个 ]。
// 两个接口前后缀不同（/*<script>*/var t1=( 、var t5nf_M2701=( 、结尾 );），
// 但「第一个 [ 到最后一个 ]」对它们都成立，注释里也没有方括号。
static bool ExtractJsonArray(const std::string &raw, std::string &json)
{
    const size_t begin = raw.find('[');
    if (begin == std::string::npos)
    {
        return false;
    }
    const size_t end = raw.rfind(']');
    if (end == std::string::npos || end <= begin)
    {
        return false;
    }
    json.assign(raw, begin, end - begin + 1);
    return true;
}

// 字符串转正数。返回 false 表示该字段不可用（空串、非数字、0）。
// 价格 0 一律当坏点丢掉：走势图对 0 值极其敏感，一个 0 就能把整条曲线压平。
static bool ParsePositivePrice(const char *text, double &out)
{
    if (text == nullptr || *text == 0)
    {
        return false;
    }
    char *endptr = nullptr;
    const double v = std::strtod(text, &endptr);
    if (endptr == text || !std::isfinite(v) || v <= 0.0)
    {
        return false;
    }
    out = v;
    return true;
}

// 该市场是否走「股票分钟K线」这套接口（A股/ETF/指数/债券逆回购）。
// 注意：这里不调用 StockDef.h 里的 isCN()。它在头文件里是非 inline 的命名空间级定义，
// 在本 TU 里 odr-use 会让编译器和 pch.obj 里的副本撞成 LNK2005 多重定义。市场判断就地展开。
static bool IsStockKLineMarket(MarketType type)
{
    return type == MarketType::MarketType_SZ_SH ||
           type == MarketType::MarketType_BJ ||
           type == MarketType::MarketType_GN ||
           type == MarketType::MarketType_HY ||
           type == MarketType::MarketType_SI ||
           type == MarketType::MarketType_CNI ||
           type == MarketType::MarketType_DY ||
           type == MarketType::MarketType_REPO;
}
// 价格字段统一是字符串（股票 close、期货索引 1 都是 "0.491" 这种带引号的形式）
static bool ReadPriceField(yyjson_val *val, double &out)
{
    if (val == nullptr)
    {
        return false;
    }
    if (yyjson_is_str(val))
    {
        return ParsePositivePrice(yyjson_get_str(val), out);
    }
    // 不用 yyjson_get_num：本工程带的 yyjson 版本没有该 API（编译期报找不到标识符）。
    // 两个接口的价格都是带引号的字符串，非字符串一律当坏点丢弃。
    return false;
}

// getKLineData：数组元素是对象，取 close 字段（时间顺序即最早在前）
static StockHistorySeries ParseStockKLinePrices(const std::string &json)
{
    StockHistorySeries series;
    // 股票分钟K线每天固定 STOCK_HISTORY_KLINE_POINTS_PER_DAY 根，据此给出每日满槽数；
    // 最后一天若还没走完，实际点数会少，渲染侧据此在右侧留白。
    series.slotsPerDay = STOCK_HISTORY_KLINE_POINTS_PER_DAY;
    yyjson_doc *doc = yyjson_read(json.c_str(), json.size(), 0);
    if (doc == nullptr)
    {
        return series;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root != nullptr && yyjson_is_arr(root))
    {
        wxString lastDay;
        std::vector<double> curDay;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(root, &iter);
        yyjson_val *item = nullptr;
        while ((item = yyjson_arr_iter_next(&iter)) != nullptr)
        {
            if (!yyjson_is_obj(item))
            {
                continue;
            }
            // day 字段形如 "2026-10-29 09:35:00"，取日期部分判断是否换天
            const char *dayText = yyjson_get_str(yyjson_obj_get(item, "day"));
            wxString day = dayText ? wxString(dayText).Left(10) : wxString();
            double price = 0.0;
            const bool ok = ReadPriceField(yyjson_obj_get(item, "close"), price);
            if (!day.IsEmpty() && !lastDay.IsEmpty() && day != lastDay)
            {
                if (!curDay.empty())
                {
                    series.days.push_back(std::move(curDay));
                    curDay.clear();
                }
            }
            if (!day.IsEmpty())
            {
                lastDay = day;
            }
            if (ok)
            {
                curDay.push_back(price);
            }
        }
        if (!curDay.empty())
        {
            series.days.push_back(std::move(curDay));
        }
    }
    yyjson_doc_free(doc);
    return series;
}

// getFourDaysLine：外层数组是交易日，内层是该日时间点，每个时间点取索引 1 的价格。
// maxDays 只保留最近的若干个交易日（接口固定回 4 天，取尾部即可）。
static StockHistorySeries ParseFuturesFourDaysPrices(const std::string &json, int maxDays)
{
    StockHistorySeries series;
    std::vector<std::vector<double>> days;
    yyjson_doc *doc = yyjson_read(json.c_str(), json.size(), 0);
    if (doc == nullptr)
    {
        return series;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root != nullptr && yyjson_is_arr(root))
    {
        yyjson_arr_iter dayIter;
        yyjson_arr_iter_init(root, &dayIter);
        yyjson_val *day = nullptr;
        while ((day = yyjson_arr_iter_next(&dayIter)) != nullptr)
        {
            if (!yyjson_is_arr(day))
            {
                continue;
            }
            std::vector<double> dayPrices;
            yyjson_arr_iter pointIter;
            yyjson_arr_iter_init(day, &pointIter);
            yyjson_val *point = nullptr;
            while ((point = yyjson_arr_iter_next(&pointIter)) != nullptr)
            {
                if (!yyjson_is_arr(point))
                {
                    continue;
                }
                double price = 0.0;
                if (ReadPriceField(yyjson_arr_get(point, 1), price))
                {
                    dayPrices.push_back(price);
                }
            }
            if (!dayPrices.empty())
            {
                days.push_back(std::move(dayPrices));
            }
        }
    }
    yyjson_doc_free(doc);

    // 每日满槽数必须在截断前用【全部】组算最大点数：
    // 接口末组是当天尚未走完的夜盘（点数偏少）。若先按 maxDays 截断再取最大，
    // historyDays=1 时只剩这个残缺末组，slotsPerDay 被压到末组点数，
    // 渲染侧算出 usedSlots == totalSlots，1 天视图被 100% 铺满、右侧没有留白。
    // 期货夜盘 21:00 到次日 15:00，满一天约 350 点左右，实际以接口返回为准。
    size_t maxPts = 0;
    for (const auto &d : days)
    {
        if (d.size() > maxPts)
        {
            maxPts = d.size();
        }
    }
    if (maxPts > 0)
    {
        series.slotsPerDay = static_cast<int>(maxPts);
    }
    // 无任何数据时保持 0，渲染层退化为「按实际点数铺满」的兜底分支

    // 只保留最近的 maxDays 个交易日
    size_t first = 0;
    if (maxDays > 0 && days.size() > static_cast<size_t>(maxDays))
    {
        first = days.size() - static_cast<size_t>(maxDays);
    }
    for (size_t i = first; i < days.size(); ++i)
    {
        series.days.push_back(std::move(days[i]));
    }
    return series;
}

// 分时刷新任务回调。绝不能把异常抛回 TaskWorkThread：
// TaskManager 一旦捕获到异常就把任务标成 m_bHasError 并永久跳过调度（见 TaskManager.cpp）。
static void OnRefreshStockHistoryData(LPVOID)
{
    g_data.RefreshHistoryPrices();
}

// LDataManager 构造
LDataManager::LDataManager()
{
    // 获取当前模块路径
    HMODULE hModule = reinterpret_cast<HMODULE>(&__ImageBase);
    wchar_t _path[MAX_PATH]{};
    GetModuleFileNameW(hModule, _path, MAX_PATH);

    // 提取目录路径
    wxFileName moduleDllPath(_path);
    m_moduleParentPath = moduleDllPath.GetPath();
    m_moduleName = moduleDllPath.GetName();
    m_isDisplayRightAlign = false;
}

LDataManager::~LDataManager()
{
    g_task->DeleteTask(TASK_KEY_INIT_LOAD);
    g_task->DeleteTask(TASK_KEY_STOCK_AUTO_REFRESH);
    g_task->DeleteTask(TASK_KEY_STOCK_HISTORY_INIT);
    g_task->DeleteTask(TASK_KEY_STOCK_HISTORY_REFRESH);
    ClearAllCodes();
    m_stockConfig.datas.clear();
}

LDataManager &LDataManager::Instance()
{
    return m_instance;
}

const wxString LDataManager::GetModuleParentPath()
{
    return m_moduleParentPath;
}
const wxString LDataManager::GetModuleName()
{
    return m_moduleName;
}

const wxArrayString LDataManager::GetAllCodes()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    wxArrayString all_codes;
    for (const auto &data : m_stockConfig.datas)
    {
        if (data && !data->code.empty())
        {
            all_codes.push_back(data->code);
        }
    }
    return all_codes;
}

void LDataManager::RealtimeRefreshFreq(long freq)
{
    m_stockConfig.realtimeRefreshFreq = freq;
}

long LDataManager::RealtimeRefreshFreq() const
{
    return m_stockConfig.realtimeRefreshFreq;
}

void LDataManager::IsDisplayName(bool flag)
{
    m_stockConfig.isDisplayName = flag;
}

bool LDataManager::IsDisplayName() const
{
    return m_stockConfig.isDisplayName;
}

void LDataManager::IsDisplayColor(bool flag)
{
    m_stockConfig.isDisplayColor = flag;
}

bool LDataManager::IsDisplayColor() const
{
    return m_stockConfig.isDisplayColor;
}

void LDataManager::IsDisplayRightAlign(bool flag)
{
    m_isDisplayRightAlign = flag;
}

bool LDataManager::IsDisplayRightAlign() const
{
    return m_isDisplayRightAlign;
}

void LDataManager::IsPriorityDisplayChanged(bool flag)
{
    m_stockConfig.isPriorityDisplayChanged = flag;
}

bool LDataManager::IsPriorityDisplayChanged() const
{
    return m_stockConfig.isPriorityDisplayChanged;
}

void LDataManager::IsDisplayCost(bool flag)
{
    m_stockConfig.isDisplayCost = flag;
}

bool LDataManager::IsDisplayCost() const
{
    return m_stockConfig.isDisplayCost;
}

void LDataManager::IsDisplayCostProfitPrice(bool flag)
{
    m_stockConfig.isDisplayCostProfitPrice = flag;
}

bool LDataManager::IsDisplayCostProfitPrice() const
{
    return m_stockConfig.isDisplayCostProfitPrice;
}

void LDataManager::IsDisplayCostProfitPercent(bool flag)
{
    m_stockConfig.isDisplayCostProfitPercent = flag;
}

bool LDataManager::IsDisplayCostProfitPercent() const
{
    return m_stockConfig.isDisplayCostProfitPercent;
}

void LDataManager::IsDisplayAlias(bool flag)
{
    m_stockConfig.isDisplayAlias = flag;
}

bool LDataManager::IsDisplayAlias() const
{
    return m_stockConfig.isDisplayAlias;
}

void LDataManager::HistoryDays(int days)
{
    if (days < 1)
    {
        days = 1;
    }
    if (days > 5)
    {
        days = 5;
    }
    const int oldDays = m_stockConfig.historyDays;
    m_stockConfig.historyDays = days;

    // 天数变了就必须立刻重拉：缓存里的序列是按旧天数截取的，
    // 不重拉的话走势图要等下一个 60 秒周期才更新，用户会以为设置没生效。
    if (days != oldDays && IsDrawHistoryGraph())
    {
        // 先清掉旧天数的缓存，避免渲染侧在新数据回来前继续显示旧跨度的曲线。
        // 清空后 UI 侧 GetHistoryPrices 返回空序列，绘制侧按"无图"处理，不会画错数据。
        ClearHistoryPrices();
        // interval=0 的首拉任务执行完由 TaskManager 自动删除；key 已存在时 AddTask 返回 FALSE，忽略即可。
        g_task->AddTask(TASK_KEY_STOCK_HISTORY_INIT, 0, OnRefreshStockHistoryData);
    }
}

int LDataManager::HistoryDays() const
{
    return m_stockConfig.historyDays;
}

void LDataManager::HistoryGraphMode(int mode)
{
    if (mode < 0)
    {
        mode = 0;
    }
    if (mode > 1)
    {
        mode = 1;
    }
    const int oldMode = m_stockConfig.historyGraphMode;
    m_stockConfig.historyGraphMode = mode;
    // 模式切换只影响渲染方式，数据本身不用重拉（两种模式共用同一份分时数据）
    (void)oldMode;
}

int LDataManager::HistoryGraphMode() const
{
    return m_stockConfig.historyGraphMode;
}

void LDataManager::HistoryGraphStyle(int style)
{
    if (style < 0)
    {
        style = 0;
    }
    if (style > 1)
    {
        style = 1;
    }
    m_stockConfig.historyGraphStyle = style;
}

int LDataManager::HistoryGraphStyle() const
{
    return m_stockConfig.historyGraphStyle;
}

void LDataManager::HistoryGraphColorDark(COLORREF color)
{
    m_stockConfig.historyGraphColorDark = color;
}

COLORREF LDataManager::HistoryGraphColorDark() const
{
    return m_stockConfig.historyGraphColorDark;
}

void LDataManager::HistoryGraphColorLight(COLORREF color)
{
    m_stockConfig.historyGraphColorLight = color;
}

COLORREF LDataManager::HistoryGraphColorLight() const
{
    return m_stockConfig.historyGraphColorLight;
}

void LDataManager::HistoryGraphDashedBox(bool flag)
{
    m_stockConfig.historyGraphDashedBox = flag;
}

bool LDataManager::HistoryGraphDashedBox() const
{
    return m_stockConfig.historyGraphDashedBox;
}

bool LDataManager::HistoryGraphShowSepLine() const
{
    return m_stockConfig.historyGraphShowSepLine;
}

void LDataManager::HistoryGraphShowSepLine(bool flag)
{
    m_stockConfig.historyGraphShowSepLine = flag;
}

COLORREF LDataManager::HistoryGraphSepLineColorDark() const
{
    return m_stockConfig.historyGraphSepLineColorDark;
}

void LDataManager::HistoryGraphSepLineColorDark(COLORREF color)
{
    m_stockConfig.historyGraphSepLineColorDark = color;
}

COLORREF LDataManager::HistoryGraphSepLineColorLight() const
{
    return m_stockConfig.historyGraphSepLineColorLight;
}

void LDataManager::HistoryGraphSepLineColorLight(COLORREF color)
{
    m_stockConfig.historyGraphSepLineColorLight = color;
}

void LDataManager::IsDrawHistoryGraph(bool flag)
{
    m_stockConfig.isDrawHistoryGraph = flag;
    // 打开开关时立刻拉一次分时，否则要等下一个 60 秒周期，走势图会先空一段时间。
    // interval=0 的任务执行完由 TaskManager 自动删除；key 已存在时 AddTask 返回 FALSE，忽略即可。
    if (flag)
    {
        g_task->AddTask(TASK_KEY_STOCK_HISTORY_INIT, 0, OnRefreshStockHistoryData);
    }
}

bool LDataManager::IsDrawHistoryGraph() const
{
    return m_stockConfig.isDrawHistoryGraph;
}

void LDataManager::IsScrollEnable(bool flag)
{
    m_stockConfig.isScrollEnable = flag;
}

bool LDataManager::IsScrollEnable() const
{
    return m_stockConfig.isScrollEnable;
}

void LDataManager::ScrollPageSize(int num)
{
    m_stockConfig.scrollPageSize = num;
}

int LDataManager::ScrollPageSize() const
{
    return m_stockConfig.scrollPageSize;
}

void LDataManager::ScrollInterval(int num)
{
    m_stockConfig.scrollInterval = num;
}

int LDataManager::ScrollInterval() const
{
    return m_stockConfig.scrollInterval;
}

bool LDataManager::HasStockWithCostPrice() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    bool has = false;
    m_stock_cache.ForEach(
        [&has](const wxString &code, StockDataPtr stock)
        {
            if (stock && stock->costPrice > 0.0)
            {
                has = true;
            }
        });
    return has;
}

void LDataManager::KLineWH(int w, int h)
{
    m_stockConfig.klineW = w;
    m_stockConfig.klineH = h;
}

void LDataManager::KLineW(int w)
{
    m_stockConfig.klineW = w;
}

int LDataManager::KLineW() const
{
    return m_stockConfig.klineW;
}

void LDataManager::KLineH(int h)
{
    m_stockConfig.klineH = h;
}

int LDataManager::KLineH() const
{
    return m_stockConfig.klineH;
}

std::pair<int, int> LDataManager::KLineWH() const
{
    return std::make_pair(m_stockConfig.klineW, m_stockConfig.klineH);
}

int LDataManager::DecimalPlaces() const
{
    return m_stockConfig.decimalPlaces;
}

void LDataManager::DecimalPlaces(int num)
{
    m_stockConfig.decimalPlaces = num;
}

void LDataManager::InitConfig()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    wxConfigBase *pConfig = wxConfigBase::Get();

    wxString all_stock_datas = pConfig->Read(KEY_CFG_ALL_STOCK_DATA);
    LLOG_DEBUG("all_stock_datas: %s", all_stock_datas);
    m_stockConfig.datas.clear();
    m_stock_cache.clear();
    if (!all_stock_datas.empty())
    {
        wxArrayString all_stock_list = CommonUtils::StringHelper::split(all_stock_datas, ";");
        if (all_stock_list.empty())
        {
            LLOG_WARN_F("all_stock_datas may be error!");
            pConfig->Write(KEY_CFG_ALL_STOCK_DATA, wxEmptyString);
        }
        else
        {
            for (wxString cfg_data : all_stock_list)
            {
                if (cfg_data.empty())
                {
                    continue;
                }
                StockDataPtr data(new LStockData());
                if (!data->LoadByConfig(cfg_data))
                {
                    continue;
                }
                m_stockConfig.datas.push_back(data);
                m_stock_cache.insert(data);
            }
        }
    }

    m_stockConfig.realtimeRefreshFreq = pConfig->Read(KEY_CFG_REALTIME_REFRESH_FREQ, DEFAULT_STOCK_REFRESH_FREQ);

    m_stockConfig.isDisplayColor = pConfig->ReadBool(KEY_CFG_DISPLAY_COLOR, true);
    m_stockConfig.isDisplayName = pConfig->ReadBool(KEY_CFG_DISPLAY_NAME, true);
    m_stockConfig.isPriorityDisplayChanged = pConfig->ReadBool(KEY_CFG_PRIORITY_DISPLAY_CHANGE, false);
    m_stockConfig.isDisplayCost = pConfig->ReadBool(KEY_CFG_DISPLAY_COST, true);
    m_stockConfig.isDisplayCostProfitPrice = pConfig->ReadBool(KEY_CFG_DISPLAY_COST_PROFIT_PRICE, true);
    m_stockConfig.isDisplayCostProfitPercent = pConfig->ReadBool(KEY_CFG_DISPLAY_COST_PROFIT_PERCENT, true);
    m_stockConfig.isDisplayAlias = pConfig->ReadBool(KEY_CFG_DISPLAY_ALIAS, false);
    m_stockConfig.isDrawHistoryGraph = pConfig->ReadBool(KEY_CFG_DRAW_HISTORY_GRAPH, false);
    m_stockConfig.historyDays = (int)pConfig->ReadLong(KEY_CFG_HISTORY_DAYS, 5);
    if (m_stockConfig.historyDays < 1)
    {
        m_stockConfig.historyDays = 1;
    }
    if (m_stockConfig.historyDays > 5)
    {
        m_stockConfig.historyDays = 5;
    }
    m_stockConfig.historyGraphMode = (int)pConfig->ReadLong(KEY_CFG_HISTORY_GRAPH_MODE, 0);
    m_stockConfig.historyGraphStyle = (int)pConfig->ReadLong(KEY_CFG_HISTORY_GRAPH_STYLE, 1);
    m_stockConfig.historyGraphColorDark = (COLORREF)pConfig->ReadLong(KEY_CFG_GRAPH_COLOR_DARK, (long)RGB(110, 110, 110));
    m_stockConfig.historyGraphColorLight = (COLORREF)pConfig->ReadLong(KEY_CFG_GRAPH_COLOR_LIGHT, (long)RGB(198, 198, 198));
    m_stockConfig.historyGraphDashedBox = pConfig->ReadBool(KEY_CFG_GRAPH_DASHED_BOX, false);
m_stockConfig.historyGraphShowSepLine = pConfig->ReadBool(KEY_CFG_GRAPH_SHOW_SEP_LINE, true);
m_stockConfig.historyGraphSepLineColorDark = (COLORREF)pConfig->ReadLong(KEY_CFG_GRAPH_SEP_COLOR_DARK, (long)RGB(80, 80, 80));
m_stockConfig.historyGraphSepLineColorLight = (COLORREF)pConfig->ReadLong(KEY_CFG_GRAPH_SEP_COLOR_LIGHT, (long)RGB(150, 150, 150));
    m_stockConfig.isScrollEnable = pConfig->ReadBool(KEY_CFG_SCROLL_ENABLE, false);
    m_stockConfig.scrollPageSize = pConfig->Read(KEY_CFG_SCROLL_PAGE_SIZE, DEFAULT_SCROLL_PAGE_SIZE);
    m_stockConfig.scrollInterval = pConfig->Read(KEY_CFG_SCROLL_INTERVAL, DEFAULT_SCROLL_INTERVAL);

    m_stockConfig.klineW = pConfig->Read(KEY_CFG_KLINE_SIZE_W, DEFAULT_KLINE_VIEW_W);
    m_stockConfig.klineH = pConfig->Read(KEY_CFG_KLINE_SIZE_H, DEFAULT_KLINE_VIEW_H);

    LLOG_DEBUG("codes: %d", m_stock_cache.size());
}

void LDataManager::InitTasks()
{
    if (RealtimeRefreshFreq() >= DEFAULT_STOCK_REFRESH_FREQ)
    {
        g_task->AddTask(TASK_KEY_INIT_LOAD, 0, OnRefreshStockRealtimeData);
    }
    g_task->AddTask(TASK_KEY_STOCK_AUTO_REFRESH, RealtimeRefreshFreq() * 1000L, OnRefreshStockRealtimeData);
    // 5 日分时（走势图背景）：先立刻拉一次（interval=0 的一次性任务，执行完自动删除），之后每 60 秒刷新
    g_task->AddTask(TASK_KEY_STOCK_HISTORY_INIT, 0, OnRefreshStockHistoryData);
    g_task->AddTask(TASK_KEY_STOCK_HISTORY_REFRESH, STOCK_HISTORY_REFRESH_INTERVAL_MS, OnRefreshStockHistoryData);
    // g_task->AddTask(TASK_KEY_STOCK_TIMELINE_AUTO_REFRESH, 1000L, OnRefreshStockTimelineData);
    // g_task->EnableTask(TASK_KEY_STOCK_TIMELINE_AUTO_REFRESH, false);
}

void LDataManager::ReLoadConfig()
{

    wxConfigBase *pConfig = wxConfigBase::Get();

    m_stockConfig.realtimeRefreshFreq = pConfig->Read(KEY_CFG_REALTIME_REFRESH_FREQ, DEFAULT_STOCK_REFRESH_FREQ);
    m_stockConfig.isDisplayColor = pConfig->ReadBool(KEY_CFG_DISPLAY_COLOR, true);
    m_stockConfig.isDisplayName = pConfig->ReadBool(KEY_CFG_DISPLAY_NAME, true);
    m_stockConfig.isPriorityDisplayChanged = pConfig->ReadBool(KEY_CFG_PRIORITY_DISPLAY_CHANGE, false);
    m_stockConfig.isDisplayCost = pConfig->ReadBool(KEY_CFG_DISPLAY_COST, true);
    m_stockConfig.isDisplayCostProfitPrice = pConfig->ReadBool(KEY_CFG_DISPLAY_COST_PROFIT_PRICE, true);
    m_stockConfig.isDisplayCostProfitPercent = pConfig->ReadBool(KEY_CFG_DISPLAY_COST_PROFIT_PERCENT, true);
    m_stockConfig.isDisplayAlias = pConfig->ReadBool(KEY_CFG_DISPLAY_ALIAS, false);
    m_stockConfig.isDrawHistoryGraph = pConfig->ReadBool(KEY_CFG_DRAW_HISTORY_GRAPH, false);
    m_stockConfig.historyDays = (int)pConfig->ReadLong(KEY_CFG_HISTORY_DAYS, 5);
    if (m_stockConfig.historyDays < 1)
    {
        m_stockConfig.historyDays = 1;
    }
    if (m_stockConfig.historyDays > 5)
    {
        m_stockConfig.historyDays = 5;
    }
    m_stockConfig.historyGraphMode = (int)pConfig->ReadLong(KEY_CFG_HISTORY_GRAPH_MODE, 0);
    m_stockConfig.historyGraphStyle = (int)pConfig->ReadLong(KEY_CFG_HISTORY_GRAPH_STYLE, 1);
    m_stockConfig.historyGraphColorDark = (COLORREF)pConfig->ReadLong(KEY_CFG_GRAPH_COLOR_DARK, (long)RGB(110, 110, 110));
    m_stockConfig.historyGraphColorLight = (COLORREF)pConfig->ReadLong(KEY_CFG_GRAPH_COLOR_LIGHT, (long)RGB(198, 198, 198));
    m_stockConfig.historyGraphDashedBox = pConfig->ReadBool(KEY_CFG_GRAPH_DASHED_BOX, false);
m_stockConfig.historyGraphShowSepLine = pConfig->ReadBool(KEY_CFG_GRAPH_SHOW_SEP_LINE, true);
m_stockConfig.historyGraphSepLineColorDark = (COLORREF)pConfig->ReadLong(KEY_CFG_GRAPH_SEP_COLOR_DARK, (long)RGB(80, 80, 80));
m_stockConfig.historyGraphSepLineColorLight = (COLORREF)pConfig->ReadLong(KEY_CFG_GRAPH_SEP_COLOR_LIGHT, (long)RGB(150, 150, 150));
    m_stockConfig.isScrollEnable = pConfig->ReadBool(KEY_CFG_SCROLL_ENABLE, false);
    m_stockConfig.scrollPageSize = pConfig->Read(KEY_CFG_SCROLL_PAGE_SIZE, DEFAULT_SCROLL_PAGE_SIZE);
    m_stockConfig.scrollInterval = pConfig->Read(KEY_CFG_SCROLL_INTERVAL, DEFAULT_SCROLL_INTERVAL);
    m_stockConfig.klineW = pConfig->Read(KEY_CFG_KLINE_SIZE_W, DEFAULT_KLINE_VIEW_W);
    m_stockConfig.klineH = pConfig->Read(KEY_CFG_KLINE_SIZE_H, DEFAULT_KLINE_VIEW_H);
}

void LDataManager::ReLoadAllStocks()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    m_stock_cache.clear();
    for (auto &&data : m_stockConfig.datas)
    {
        if (!data)
        {
            continue;
        }
        m_stock_cache.insert(data);
    }
}

bool LDataManager::IsChangeStockList()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    return AllStocks().size() != m_stockConfig.datas.size();
}

void LDataManager::SaveConfig()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    wxConfigBase *pConfig = wxConfigBase::Get();

    pConfig->Write(KEY_CFG_REALTIME_REFRESH_FREQ, m_stockConfig.realtimeRefreshFreq);
    pConfig->Write(KEY_CFG_DISPLAY_COLOR, m_stockConfig.isDisplayColor);
    pConfig->Write(KEY_CFG_PRIORITY_DISPLAY_CHANGE, m_stockConfig.isPriorityDisplayChanged);
    pConfig->Write(KEY_CFG_DISPLAY_NAME, m_stockConfig.isDisplayName);
    pConfig->Write(KEY_CFG_DISPLAY_COST, m_stockConfig.isDisplayCost);
    pConfig->Write(KEY_CFG_DISPLAY_COST_PROFIT_PRICE, m_stockConfig.isDisplayCostProfitPrice);
    pConfig->Write(KEY_CFG_DISPLAY_COST_PROFIT_PERCENT, m_stockConfig.isDisplayCostProfitPercent);
    pConfig->Write(KEY_CFG_DISPLAY_ALIAS, m_stockConfig.isDisplayAlias);
    pConfig->Write(KEY_CFG_DRAW_HISTORY_GRAPH, m_stockConfig.isDrawHistoryGraph);
    pConfig->Write(KEY_CFG_HISTORY_DAYS, m_stockConfig.historyDays);
    pConfig->Write(KEY_CFG_HISTORY_GRAPH_MODE, m_stockConfig.historyGraphMode);
    pConfig->Write(KEY_CFG_HISTORY_GRAPH_STYLE, m_stockConfig.historyGraphStyle);
    pConfig->Write(KEY_CFG_GRAPH_COLOR_DARK, (long)m_stockConfig.historyGraphColorDark);
    pConfig->Write(KEY_CFG_GRAPH_COLOR_LIGHT, (long)m_stockConfig.historyGraphColorLight);
    pConfig->Write(KEY_CFG_GRAPH_DASHED_BOX, m_stockConfig.historyGraphDashedBox);
pConfig->Write(KEY_CFG_GRAPH_SHOW_SEP_LINE, m_stockConfig.historyGraphShowSepLine);
pConfig->Write(KEY_CFG_GRAPH_SEP_COLOR_DARK, (long)m_stockConfig.historyGraphSepLineColorDark);
pConfig->Write(KEY_CFG_GRAPH_SEP_COLOR_LIGHT, (long)m_stockConfig.historyGraphSepLineColorLight);
    pConfig->Write(KEY_CFG_SCROLL_ENABLE, m_stockConfig.isScrollEnable);
    pConfig->Write(KEY_CFG_SCROLL_PAGE_SIZE, m_stockConfig.scrollPageSize);
    pConfig->Write(KEY_CFG_SCROLL_INTERVAL, m_stockConfig.scrollInterval);

    pConfig->Write(KEY_CFG_KLINE_SIZE_W, m_stockConfig.klineW);
    pConfig->Write(KEY_CFG_KLINE_SIZE_H, m_stockConfig.klineH);

    wxArrayString all_stock_list;
    m_stockConfig.datas.clear();

    m_stock_cache.ForEach([&all_stock_list, this](const wxString &code, StockDataPtr stock)
                          {
        if (stock && !stock->code.empty()) {
            all_stock_list.push_back(stock->ToConfig());
            m_stockConfig.datas.push_back(stock);
        } });

    wxString all_stock_datas = UtilStringHlp::vectorJoinString(all_stock_list, ";");
    LLOG_DEBUG("all_stock_datas: %s", all_stock_datas);
    pConfig->Write(KEY_CFG_ALL_STOCK_DATA, all_stock_datas);
    pConfig->Flush();

    g_task->ModifyTaskInterval(TASK_KEY_STOCK_AUTO_REFRESH, RealtimeRefreshFreq() * 1000L);
    // 用户刚显式改了刷新频率 => 顺手复位闭市退避，让新频率立即生效
    // （否则若此刻正处于退避档位，max(用户频率, 档位) 会继续用档位压着，看起来像设置没生效）
    ResetRefreshThrottle();
}

void LDataManager::AddStock(StockDataPtr data)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    if (data && !data->code.empty())
    {
        m_stock_cache.insert(data);
    }
}

void LDataManager::ClearAllCodes()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    m_stock_cache.clear();
}

wxVector<StockDataPtr> LDataManager::AllStocks()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    wxVector<StockDataPtr> datas;

    m_stock_cache.ForEach(
        [&datas](const wxString &code, StockDataPtr stock)
        {
            if (stock && !stock->code.empty())
            {
                datas.push_back(stock);
            }
        });

    return datas;
}

StockDataPtr LDataManager::GetStockByCode(const wxString &code)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    auto it = m_stock_cache.find(code);
    if (!it)
    {
        return StockDataPtr(nullptr);
    }
    return it;
}

StockDataPtr LDataManager::GetStockByIndex(WXUINT index)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    auto it = m_stock_cache.indexAt(index);
    if (!it)
    {
        return StockDataPtr(nullptr);
    }
    return it;
}

bool LDataManager::DeleteStockByCode(const wxString &code)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    return m_stock_cache.remove(code);
}

bool LDataManager::SetStockPosition(const StockDataPtr ptr, size_t pos)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    return m_stock_cache.move_to(ptr->code, pos);
}

bool LDataManager::HandleStockRealtimeData(wxString jsonp)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxData);
    if (jsonp.empty())
    {
        LLOG_WARN_F("HandleStockRealtimeData data EMPTY!");
        return false;
    }

    jsonp.Replace("\n", "");
    wxArrayString lines = UtilStringHlp::split(jsonp, ";");
    if (lines.empty())
    {
        LLOG_WARN_F("HandleStockRealtimeData data INVALID!");
        return false;
    }

    for (wxString line : lines)
    {
        if (line.empty())
        {
            continue;
        }
        line.Replace("var hq_str_", "");
        line.Replace("\"", "");

        wxArrayString item_arr = UtilStringHlp::split(line, '=');
        if (item_arr.size() != 2)
        {
            LLOG_WARN_F("HandleStockRealtimeData line data INVALID!");
            continue;
        }

        wxString stock_code = item_arr[0];
        wxString raw_data = item_arr[1];

        auto stock = m_stock_cache.find(stock_code);
        if (!stock)
        {
            LLOG_DEBUG("HandleStockRealtimeData ignore %s", stock_code);
            continue;
        }
        stock->LoadByRealtimeData(stock_code, raw_data);
    }
    return true;
}

void LDataManager::UpdateRefreshThrottle(const std::string &quoteSignature)
{
    // 本函数由实时刷新工作线程调用（一次性 INIT_LOAD 任务与周期任务共用同一个回调，可能并发），
    // 状态由 m_mtxRefreshThrottle 保护。
    // 绝不能把异常抛回 TaskWorkThread：TaskManager 一捕获到异常就会把任务标成 m_bHasError
    // 并永久跳过它的调度，实时刷新会彻底停摆（见 TaskManager.cpp TaskWorkThread/TaskAutoResetFlag）。
    try
    {
        const long freqSec = RealtimeRefreshFreq();
        const unsigned int userMs = static_cast<unsigned int>(freqSec > 0 ? freqSec : 1) * 1000U;

        // 加锁顺序：先取自选列表（内部取 m_mtxData），再取 m_mtxRefreshThrottle。
        // 全文件只有这一处同时碰两把锁，保持「先 m_mtxData 后 m_mtxRefreshThrottle」这一种顺序即可避免死锁。
        SYSTEMTIME beijing{};
        GetBeijingTime(beijing);
        const RefreshProbe *probe = ActiveProbe(WatchlistGroupMask(), beijing);

        unsigned int nextMs = userMs;
        size_t nextLevel = 0;
        bool needApply = false;

        {
            std::lock_guard<std::mutex> lock(m_mtxRefreshThrottle);

            // 首轮没有可比对的基线，视为「有变化」，先按用户频率跑起来
            const bool changed = !m_refreshHasLast || quoteSignature != m_refreshLastSignature;
            m_refreshLastSignature = quoteSignature;
            m_refreshHasLast = true;

            if (changed || probe != nullptr)
            {
                // 行情在动（或到了开盘前窗口）=> 立刻回到用户设定的频率
                m_refreshBackoffLevel = 0;
                m_refreshStillRounds = 0;
            }
            else if (++m_refreshStillRounds >= REFRESH_STILL_ROUNDS)
            {
                m_refreshStillRounds = 0;
                if (m_refreshBackoffLevel + 1 < REFRESH_BACKOFF_LEVELS)
                {
                    ++m_refreshBackoffLevel;
                }
            }

            nextLevel = m_refreshBackoffLevel;
            const unsigned int levelMs = REFRESH_BACKOFF_SECS[m_refreshBackoffLevel] * 1000U;
            nextMs = levelMs > userMs ? levelMs : userMs;

            // 只在间隔真的变了的时候才去动任务表：否则每轮都要抢一次任务锁、还会刷一条重复日志
            if (nextMs != m_refreshAppliedMs)
            {
                m_refreshAppliedMs = nextMs;
                needApply = true;
            }
        }

        if (needApply)
        {
            g_task->ModifyTaskInterval(TASK_KEY_STOCK_AUTO_REFRESH, nextMs);

            // 日志只打「间隔发生变化」，正常盘中不会刷屏
            if (probe != nullptr)
            {
                LLOG_INFO_F("Refresh probe window: %s -> interval %u ms", probe->label, nextMs);
            }
            else if (nextLevel == 0)
            {
                LLOG_INFO_F("Refresh interval reset -> %u ms", nextMs);
            }
            else
            {
                LLOG_INFO_F("Refresh backoff level %u -> interval %u ms", static_cast<unsigned int>(nextLevel), nextMs);
            }
        }
    }
    catch (...)
    {
        LLOG_ERROR("UpdateRefreshThrottle: exception, interval unchanged (%d)", 0);
    }
}

void LDataManager::ResetRefreshThrottle()
{
    std::lock_guard<std::mutex> lock(m_mtxRefreshThrottle);
    m_refreshBackoffLevel = 0;
    m_refreshStillRounds = 0;
    m_refreshHasLast = false;
    m_refreshLastSignature.clear();
    // 置 0 = 未知：下一轮刷新会重新下发一次间隔，把「期间别人直接改过任务间隔」也一并纠正掉
    m_refreshAppliedMs = 0;
}

void LDataManager::OnRefreshStockTimelineData(const std::string &callback_id, const std::string &data)
{
    wxLogDebug("OnRefreshStockTimelineData: %s", data.c_str());

    // 注意：本函数所有提前返回的分支都必须回调一次空响应，
    // 否则 JS 侧的 loading 状态永远不会结束（弹窗一直显示「加载中」）
    auto replyEmpty = [&callback_id]()
    {
        LStockServerSocket::GetInstance().GetBridge()->BridgeSendDataWithCallback(callback_id, "{}");
    };

    yyjson_doc *doc = yyjson_read(data.c_str(), data.size(), 0);
    if (doc == nullptr)
    {
        wxLogDebug("OnRefreshStockTimelineData: json parse error!");
        replyEmpty();
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr)
    {
        wxLogDebug("OnRefreshStockTimelineData: root");
        yyjson_doc_free(doc);
        replyEmpty();
        return;
    }

    LStockPeriodType type = IntToStockPeriodType(UtilJsonHlp::GetInt(root, "type", -1));
    switch (type)
    {
    case LStockPeriodType::TIMELINE:
        break;
    default:
        wxLogDebug("OnRefreshStockTimelineData: not support %d", type);
        yyjson_doc_free(doc);
        replyEmpty();
        return;
    }

    std::string code = UtilJsonHlp::GetString(root, "code").ToStdString();

    auto stock = g_data.GetStockByCode(code);
    if (stock && !stock->code.empty())
    {
        if (stock->CanUpdateStockPeriodData(type))
        {
            wxArrayString params;
            wxString url_prefix = wxEmptyString;

            // h5t.js
            switch (market(stock->code))
            {
            case MarketType::MarketType_REPO:
                //     REPO: {
                //         T_Head_STR: "t1",
                //         T_EMI_URL: "http://finance.sina.com.cn/finance/eqlweight/$symbol.js",
                //         T_URL: "https://quotes.sina.cn/cn/api/openapi.php/CN_MinlineService.getMinlineData?symbol=$symbol&version=7.11.0&callback=$cb=&dpc=1",
                //         T5_URL: "http://finance.sina.com.cn/realstock/company/$symbol/hisdata/klc_cm_bond.js?day=$rn",
                //         TRADING_DATES_URL: c,
                //         HISTORY_DATA_URL: "http://finance.sina.com.cn/realstock/company/$symbol/hisdata/$y/$m.js?d=$date",
                //         LAST5_URL: "http://finance.sina.com.cn/realstock/lastfive/$symbol.js?_=$rn"
                //     },
                url_prefix = "https://quotes.sina.cn/cn/api/openapi.php/CN_MinlineService.getMinlineData?";

                // symbol=$symbol&version=7.11.0&callback=$cb=&dpc=1

                params.push_back(L"symbol=" + stock->code);
                params.push_back(L"version=7.11.0");
                params.push_back(L"dpc=1");

                // {
                //     "result": {
                //         "status": {
                //             "code": 0,
                //             "msg": "MySql:lv1:success"
                //         },
                //         "data": [
                //             {
                //                 "m": "09:30:00",
                //                 "v": "477000",
                //                 "p": "7.19",
                //                 "avg_p": "7.19",
                //                 "tot_v": "477000"
                //             },
                //         ]
                //     }
                // }

                break;
            case MarketType::MarketType_BJ:
            //     BJ: {
            //         T_URL: "https://cn.finance.sina.com.cn/minline/getMinlineData?symbol=$symbol&version=7.11.0&callback=$cb=&dpc=1"
            //     },
            case MarketType::MarketType_SI:
            //     SI: {
            //         T_Head_STR: "t1",
            //         T_URL: "https://cn.finance.sina.com.cn/minline/getMinlineData?symbol=$symbol&version=7.11.0&callback=$cb=&dpc=1",
            //         T5_URL: "http://finance.sina.com.cn/realstock/company/$symbol/hisdata/klc_cm.js?day=$rn",
            //         TRADING_DATES_URL: c,
            //         LAST5_URL: "http://finance.sina.com.cn/realstock/lastfive/$symbol.js?_=$rn"
            //     },
            case MarketType::MarketType_CNI:
            case MarketType::MarketType_DY:
            case MarketType::MarketType_GN:
            case MarketType::MarketType_HY:
            case MarketType::MarketType_SZ_SH:
                //     CN: {
                //         T_Head_STR: "t1",
                //         T_EMI_URL: "http://finance.sina.com.cn/finance/eqlweight/$symbol.js",
                //         T_URL: "https://cn.finance.sina.com.cn/minline/getMinlineData?symbol=$symbol&callback=$cb=&version=7.11.0&dpc=1",
                //         T5_URL: "http://finance.sina.com.cn/realstock/company/$symbol/hisdata/klc_cm.js?day=$rn",
                //         TRADING_DATES_URL: c,
                //         HISTORY_DATA_URL: "http://finance.sina.com.cn/realstock/company/$symbol/hisdata/$y/$m.js?d=$date",
                //         LAST5_URL: "http://finance.sina.com.cn/realstock/lastfive/$symbol.js?_=$rn"
                //     },
                url_prefix = "https://cn.finance.sina.com.cn/minline/getMinlineData?";

                params.push_back(L"symbol=" + stock->code);
                params.push_back(L"version=7.11.0");
                params.push_back(L"dpc=1");

                // {
                //     "result": {
                //         "status": {
                //             "code": 0,
                //             "msg": "MySql:lv1:success"
                //         },
                //         "data": [
                //             {
                //                 "m": "09:30:00",
                //                 "v": "5513800",
                //                 "p": "4.96",
                //                 "avg_p": "4.96",
                //                 "tot_v": "5513800"
                //             }
                //         ]
                //     }
                // }

                break;
            case MarketType::MarketType_OTC:
                //     OTC: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock.finance.sina.com.cn/thirdmarket/api/openapi.php/NQHQService.minline?symbol=$symbol&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     },

                // https://stock.finance.sina.com.cn/thirdmarket/quotes/832059.html

                url_prefix = "https://stock.finance.sina.com.cn/thirdmarket/api/openapi.php/NQHQService.minline?";

                params.push_back(L"symbol=" + stock->code);
                break;
            case MarketType::MarketType_BTC:
                break;
            case MarketType::MarketType_US:
            {
                //     US: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock.finance.sina.com.cn/usstock/api/jsonp_v2.php/$cb=/US_MinlineNService.getMinline?symbol=$symbol&day=1&random=$rn",
                //         T5_URL: "http://stock.finance.sina.com.cn/usstock/api/jsonp_v2.php/$cb/US_MinlineNService.getMinline?symbol=$symbol&day=5&random=$rn",
                //         TRADING_DATES_URL: "http://stock.finance.sina.com.cn/usstock/api/openapi.php/US_MinKService.getTradeDays?&start_day=$start&end_day=$end&callback=$cb="
                //     },

                wxString code = UtilStringHlp::remove(stock->code, "gb_");
                url_prefix = wxT("https://stock.finance.sina.com.cn/usstock/api/jsonp_v2.php/var%20t1") + code + wxT("=/US_MinlineNService.getMinline?");

                params.push_back(L"symbol=" + code);
                params.push_back(L"day=1");
                // params.push_back(L"random=1");

                // /*<script>location.href='//sina.com';</script>*/
                // var t1aapl=("09:30:00,4534779,331.859,332.4800;09:31:00,414814,331.867,331.5050;09:32:00,217974,331.886,332.8530;09:33:00,307562,331.917,332.9920;09:34:00,323419,331.977,333.4400;09:35:00,283580

                break;
            }
            case MarketType::MarketType_HK:
                //     HK: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock.finance.sina.com.cn/hkstock/api/openapi.php/HK_StockService.getHKMinline?symbol=$symbol&random=$rn&callback=$cb=",
                //         T5_URL: "http://quotes.sina.cn/hk/api/openapi.php/HK_MinlineService.getMinline?symbol=$symbol&day=5&callback=$cb=",
                //         LAST5_URL: "http://stock.finance.sina.com.cn/hkstock/api/jsonp_v2.php/$cb/HK_StockService.getStock5DayAvgVolume?symbol=$symbol",
                //         TRADING_DATES_URL: c
                //     },
            case MarketType::MarketType_HKAP:
                //     HKAP: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://quotes.sina.cn/hk/api/openapi.php/HK_MinlineService.getMinlineDp?symbol=$symbol&callback=$cb=",
                //         T5_URL: "http://quotes.sina.cn/hk/api/openapi.php/HK_MinlineService.getMinline?symbol=$symbol&day=5&callback=$cb=",
                //         LAST5_URL: "http://stock.finance.sina.com.cn/hkstock/api/jsonp_v2.php/$cb/HK_StockService.getStock5DayAvgVolume?symbol=$symbol",
                //         TRADING_DATES_URL: c
                //     },
                url_prefix = "https://quotes.sina.cn/hk/api/openapi.php/HK_MinlineService.getMinline?";

                params.push_back(L"symbol=" + stock->code);
                params.push_back(L"day=1");
                // {
                //     "result": {
                //         "status": {
                //             "code": 0
                //         },
                //         "data": [
                //             [
                //                 {
                //                     "date": "2026-07-17",
                //                     "prevclose": "70.45000",
                //                     "m": "09:30:00",
                //                     "price": "71.10000",
                //                     "volume": "83500",
                //                     "avg_p": "71.055"
                //                 },
                //                 {
                //                     "m": "09:31:00",
                //                     "price": "71.35000",
                //                     "volume": "122500",
                //                     "avg_p": "71.233"
                //                 },
                //             ]
                //         ]
                //     }
                // }

                break;
            case MarketType::MarketType_HF:
            {
                //     HF: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock2.finance.sina.com.cn/futures/api/openapi.php/GlobalFuturesService.getGlobalFuturesMinLine?symbol=$symbol&callback=$cb=",
                //         T5_URL: "http://stock2.finance.sina.com.cn/futures/api/openapi.php/GlobalFuturesService.getGlobalFutures5MLine?symbol=$symbol&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     },
                url_prefix = "https://stock2.finance.sina.com.cn/futures/api/openapi.php/GlobalFuturesService.getGlobalFuturesMinLine?";

                wxString code = UtilStringHlp::remove(stock->code, "hf_");

                params.push_back(L"symbol=" + code);

                // {
                //     "result": {
                //         "status": {
                //             "code": 0
                //         },
                //         "data": {
                //             "minLine_1d": [
                //                 [
                //                     "2026-07-17",
                //                     "3992.100",
                //                     "cme",
                //                     "",
                //                     "06:00",
                //                     "3981.260",
                //                     "0",
                //                     "0",
                //                     "3981.259",
                //                     "2026-07-17 06:00:00"
                //                 ],
                //                 [
                //                     "06:01",
                //                     "3982.021",
                //                     "0",
                //                     "0",
                //                     "3981.442",
                //                     "2026-07-17 06:01:00"
                //                 ],
                //             ]
                //         }
                //     }
                // }

                break;
            }
            case MarketType::MarketType_globalbd:
                //     globalbd: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://bond.finance.sina.com.cn/hq/gb/min?symbol=$symbol&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     },
                break;
            case MarketType::MarketType_LSE:
                //     LSE: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://quotes.sina.cn/lse/api/openapi.php/LSEService.minline?symbol=$symbol&type=1&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     }
                break;
            case MarketType::MarketType_NF:
                //     NF: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock2.finance.sina.com.cn/futures/api/jsonp.php/$cb=/InnerFuturesNewService.getMinLine?symbol=$symbol",
                //         T5_URL: "http://stock2.finance.sina.com.cn/futures/api/jsonp.php/$cb=/InnerFuturesNewService.getFourDaysLine?symbol=$symbol",
                //         TRADING_DATES_URL: c
                //     },
                // 国内期货（上期所/大商所/郑商所/中金所/能源中心）。
                // 与 US 分支同理：jsonp 回调名必须写进 path（$cb 在 path 上，不能用 ?callback=），
                // 且 symbol 必须去掉 nf_ 前缀 —— 否则接口返回 var t1=(null)
                {
                    wxString symbol = UtilStringHlp::remove(stock->code, "nf_");
                    url_prefix = wxT("https://stock2.finance.sina.com.cn/futures/api/jsonp.php/var%20t1") + symbol + wxT("=/InnerFuturesNewService.getMinLine?");

                    params.push_back(L"symbol=" + symbol);

                    // /*<script>location.href='//sina.com';</script>*/
                    // var t1=([["21:00","3107.000","3106.825","18220","1598337","3108.000","2026-09-14"],["21:01",...],...]);
                    // 每行: [时间, 价格, 均价, 分钟量, 累计量, 首行额外字段..., 日期]
                }
                break;
            case MarketType::MarketType_GOODS:
                //     GOODS: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock2.finance.sina.com.cn/futures/api/openapi.php/SpotService.getMinLine?symbol=$symbol&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     },
                break;
            case MarketType::MarketType_fund:
                //     fund: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://app.xincai.com/fund/api/jsonp.json/$cb=/XinCaiFundService.getFundYuCeNav?symbol=$symbol&___qn=3",
                //         TRADING_DATES_URL: c
                //     },
                break;
            case MarketType::MarketType_option_cn:
                //     option_cn: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock.finance.sina.com.cn/futures/api/openapi.php/StockOptionDaylineService.getOptionMinline?symbol=$symbol&random=$rn&callback=$cb=",
                //         T5_URL: "http://stock.finance.sina.com.cn/futures/api/openapi.php/StockOptionDaylineService.getFiveDayLine?symbol=$symbol&random=$rn&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     },
                break;
            case MarketType::MarketType_op_m:
                //     op_m: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock.finance.sina.com.cn/futures/api/openapi.php/FutureOptionAllService.getOptionMinline?symbol=$symbol&random=$rn&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     },
                break;
            case MarketType::MarketType_global_index:
                //     global_index: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://gi.finance.sina.com.cn/hq/min/?symbol=$symbol&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     },
                break;
            case MarketType::MarketType_forex:
                break;
            case MarketType::MarketType_forex_yt:
                break;
            case MarketType::MarketType_CFF:
                //     CFF: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://stock2.finance.sina.com.cn/futures/api/jsonp.php/$cb=/InnerFuturesNewService.getMinLine?symbol=$symbol",
                //         T5_URL: "http://stock2.finance.sina.com.cn/futures/api/jsonp.php/$cb=/InnerFuturesNewService.getFourDaysLine?symbol=$symbol",
                //         TRADING_DATES_URL: c
                //     },
                // 中金所（CFF_RE_XXX，如 CFF_RE_IF0）。接口与 NF 完全一致，
                // 只是 code 前缀换成 CFF_RE_，symbol 同样要去前缀（CFF_RE_IF0 -> IF0）
                {
                    wxString symbol = UtilStringHlp::remove(stock->code, "CFF_RE_");
                    url_prefix = wxT("https://stock2.finance.sina.com.cn/futures/api/jsonp.php/var%20t1") + symbol + wxT("=/InnerFuturesNewService.getMinLine?");

                    params.push_back(L"symbol=" + symbol);
                }
                break;
            case MarketType::MarketType_MSCI:
                //     MSCI: {
                //         T_Head_STR: "t1",
                //         T_URL: "http://quotes.sina.cn/msci/api/openapi.php/MSCIService.getMinLine?symbol=$symbol&callback=$cb=",
                //         TRADING_DATES_URL: c
                //     },
                break;
            default:
                break;
            }

            if (url_prefix.empty())
            {
                // 该市场尚未配置分时接口。这里必须回一次空响应：
                // JS 侧只有 native_response 会把 loading 标志置回 false，
                // 静默 return 会让弹窗永远停在「加载中」
                LLOG_WARN_F("OnRefreshStockTimelineData not support market: %s", code.c_str());
                LStockServerSocket::GetInstance().GetBridge()->BridgeSendDataWithCallback(callback_id, "{}");
                return;
            }

            wxString url = url_prefix + CommonUtils::StringHelper::vectorJoinString(params, "&");
            CString strHeaders = (wxT("Referer: ") + stock->url).wc_str();

            std::string data;
            if (UtilNetHlp::GetURL(url.ToStdWstring(), data, false, WEB_USERAGENT, strHeaders, strHeaders.GetLength()))
            {
                // stock->addTimelineData(data);

                stock->updatePeriodRawData(type, data);

                LStockServerSocket::GetInstance().GetBridge()->BridgeSendDataWithCallback(callback_id, stock->GetBridgData(type).ToStdString());
            }
            else
            {
                // 接口拉取失败也必须回调，否则同样会卡在「加载中」
                LLOG_WARN_F("OnRefreshStockTimelineData GetURL failed: %s", url.ToUTF8().data());
                replyEmpty();
            }
        }
        else
        {
            LStockServerSocket::GetInstance().GetBridge()->BridgeSendDataWithCallback(callback_id, stock->GetBridgData(type).ToStdString());
        }
    }
    else
    {
        wxLogDebug("OnRefreshStockTimelineData: not found stock %s", code.c_str());
        replyEmpty();
    }

    yyjson_doc_free(doc);
}

StockHistorySeries LDataManager::GetHistoryPrices(const wxString &code) const
{
    std::lock_guard<std::mutex> lock(m_mtxHistory);
    auto it = m_historyPrices.find(code);
    if (it == m_historyPrices.end())
    {
        return StockHistorySeries{};
    }
    return it->second;
}

bool LDataManager::HasHistoryPrices(const wxString &code) const
{
    std::lock_guard<std::mutex> lock(m_mtxHistory);
    auto it = m_historyPrices.find(code);
    return it != m_historyPrices.end() && it->second.totalPoints() >= 2;
}

void LDataManager::ClearHistoryPrices()
{
    std::lock_guard<std::mutex> lock(m_mtxHistory);
    m_historyPrices.clear();
}

// 单只股票/期货的分时拉取。任何失败都返回 false，由调用侧保留上一份缓存
bool LDataManager::FetchHistoryPrices(const wxString &code, StockHistorySeries &out)
{
    try
    {
        if (code.empty())
        {
            return false;
        }

        int days = HistoryDays();
        if (days < 1)
        {
            days = 1;
        }
        if (days > STOCK_HISTORY_MAX_DAYS)
        {
            days = STOCK_HISTORY_MAX_DAYS;
        }

        const MarketType type = market(code);
        wxString url;
        if (IsStockKLineMarket(type))
        {
            const int dataLen = days * STOCK_HISTORY_KLINE_POINTS_PER_DAY;
            url = HISTORY_STOCK_KLINE_URL + code +
                  wxString::Format(wxT("&scale=5&ma=no&datalen=%d"), dataLen);
        }
        else if (type == MarketType::MarketType_NF || type == MarketType::MarketType_CFF)
        {
            const wxString symbol = (type == MarketType::MarketType_NF)
                                        ? UtilStringHlp::remove(code, wxT("nf_"))
                                        : UtilStringHlp::remove(code, wxT("CFF_RE_"));
            url = HISTORY_FUTURES_URL_PREFIX + symbol + HISTORY_FUTURES_URL_MID + symbol;
        }
        else
        {
            return false;
        }

        CString strHeaders = _T("Referer: https://finance.sina.com.cn");
        std::string raw;
        if (!UtilNetHlp::GetURL(url.ToStdWstring(), raw, false, WEB_USERAGENT, strHeaders, strHeaders.GetLength()) || raw.empty())
        {
            LLOG_WARN_F("FetchHistoryPrices GetURL failed: %s", url.ToUTF8().data());
            return false;
        }

        std::string json;
        if (!ExtractJsonArray(raw, json))
        {
            LLOG_WARN_F("FetchHistoryPrices no json array: %s", url.ToUTF8().data());
            return false;
        }

        const bool isFutures = (type == MarketType::MarketType_NF || type == MarketType::MarketType_CFF);
        StockHistorySeries series = isFutures ? ParseFuturesFourDaysPrices(json, days) : ParseStockKLinePrices(json);
        if (series.totalPoints() < 2)
        {
            LLOG_WARN_F("FetchHistoryPrices too few points (%d): %s", static_cast<int>(series.totalPoints()), code.ToUTF8().data());
            return false;
        }

        out = std::move(series);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void LDataManager::RefreshHistoryPrices()
{
    try
    {
        // 走势图关着就不拉：分时是纯展示数据，不参与行情计算，
        // 开关关闭时每分钟为每只股票发一次请求纯属浪费（与实时刷新的闭市退避是同一套省流思路）
        if (!IsDrawHistoryGraph())
        {
            return;
        }

        const wxArrayString codes = GetAllCodes();
        if (codes.empty())
        {
            ClearHistoryPrices();
            return;
        }

        for (size_t i = 0; i < codes.GetCount(); ++i)
        {
            const wxString code = codes[i];
            StockHistorySeries prices;
            // 网络失败/字段缺失/市场不支持：保留上一份缓存，不清空
            if (!FetchHistoryPrices(code, prices))
            {
                continue;
            }
            std::lock_guard<std::mutex> lock(m_mtxHistory);
            m_historyPrices[code] = std::move(prices);
        }

        // 剔除已从自选删除的股票，避免缓存随增删无限增长
        std::lock_guard<std::mutex> lock(m_mtxHistory);
        for (auto it = m_historyPrices.begin(); it != m_historyPrices.end();)
        {
            if (codes.Index(it->first) == wxNOT_FOUND)
            {
                it = m_historyPrices.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    catch (...)
    {
        LLOG_WARN("RefreshHistoryPrices failed");
    }
}
