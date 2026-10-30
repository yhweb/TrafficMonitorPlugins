#pragma once
#include "pch.h"
#include <wx/sharedptr.h>
#include <mutex>
#include "StockDef.h"

#define g_data LDataManager::Instance()

#define TASK_KEY_INIT_LOAD "INIT_STOCK_DATA"
#define TASK_KEY_STOCK_AUTO_REFRESH "TASK_AUTO_REFRESH_STOCK_DATA"
#define TASK_KEY_STOCK_TIMELINE_AUTO_REFRESH "TASK_AUTO_REFRESH_STOCK_TIMELINE_DATA"
#define TASK_KEY_STOCK_HISTORY_INIT "INIT_STOCK_HISTORY_DATA"
#define TASK_KEY_STOCK_HISTORY_REFRESH "TASK_AUTO_REFRESH_STOCK_HISTORY_DATA"

#define STOCK_DISPLAY_ITEM_MAX 99

// 默认刷新频率
constexpr unsigned int DEFAULT_STOCK_REFRESH_FREQ = 10;
// 5 日分时（走势图数据）刷新间隔，单位毫秒
constexpr unsigned int STOCK_HISTORY_REFRESH_INTERVAL_MS = 60 * 1000;
// 5 分钟粒度下一个交易日的点数（A股每天约 48 根），historyDays * 该值 = 要拉的 K 线根数
// （接口上限 1900，2000+ 返回空；5 天 = 240 根，实测 sz002086 / sh515880 正常）
constexpr int STOCK_HISTORY_KLINE_POINTS_PER_DAY = 48;
// 最多保留的分时交易日数，与配置项 /stock/history_days 的 1..5 区间一致
constexpr int STOCK_HISTORY_MAX_DAYS = 5;

// 分时走势序列：按交易日分组，用于「固定时间轴 + 未来留白」的渲染。
// days 里每一项是某个交易日的价格序列（时间升序）；slotsPerDay 是「满一天」应有的槽位数，
// 最后一天若尚未收盘，其点数会少于 slotsPerDay，渲染侧据此在右侧留白。
struct StockHistorySeries
{
    std::vector<std::vector<double>> days;
    int slotsPerDay{0};                       // 每日满槽数（0 = 未知，按实际点数铺满）
    bool empty() const { return days.empty(); }
    size_t totalPoints() const
    {
        size_t n = 0;
        for (const auto &d : days) { n += d.size(); }
        return n;
    }
};

// 默认K线图宽度
constexpr unsigned int DEFAULT_KLINE_VIEW_W = 600;
// 默认K线图高度
constexpr unsigned int DEFAULT_KLINE_VIEW_H = 400;
// 最大K线图大小
constexpr unsigned int MAX_KLINE_VIEW_SIZE = 1000;
// 最小K线图大小
constexpr unsigned int MIN_KLINE_VIEW_SIZE = 100;
// 默认小数点有效位数
constexpr unsigned int DEFAULT_DECIMAL_PLACES = 2;
// 最大小数点有效位数
constexpr unsigned int MAX_DECIMAL_PLACES = 10;
// 最小小数点有效位数
constexpr unsigned int MIN_DECIMAL_PLACES = 0;
// 默认滚动显示每页数量
constexpr int DEFAULT_SCROLL_PAGE_SIZE = 2;
// 默认滚动间隔（秒）
constexpr int DEFAULT_SCROLL_INTERVAL = 5;

using namespace STOCK;

namespace {
    WX_DECLARE_HASH_MAP(wxString, size_t, wxStringHash, wxStringEqual, LCacheIndexMap);

    using StockDataPtr = wxSharedPtr<LStockData>;
    using CachePair = std::pair<wxString, StockDataPtr>;
    using CachePairPtr = wxSharedPtr<CachePair>;

    class LStockCacheManager {
    private:
        // 存储真实数据，可自由调整顺序
        wxVector<CachePairPtr> data_;
        // 字符串唯一索引：key -> data_下标
        LCacheIndexMap cache_index_;

    private:
        // 全量刷新索引（移动/删除后调用）
        void rebuild_index()
        {
            cache_index_.clear();
            for (size_t i = 0; i < data_.size(); ++i)
            {
                const auto& item = data_[i];
                cache_index_[item->first] = i;
            }
        }

    public:
        bool insert(StockDataPtr data) {
            if (data && !data->code.empty()) {
                wxString key = data->code;
                if (cache_index_.count(key)) {
                    return false;
                }
                CachePairPtr item(new CachePair(key, data));
                data_.push_back(item);
                cache_index_[key] = data_.size() - 1;
                return true;
            }
            return false;
        }

        StockDataPtr indexAt(size_t pos) {
            if (data_.size() > pos) {
                return data_[pos]->second;
            }
            return StockDataPtr(nullptr);
        }

        // 根据字符串快速查找，返回对象指针，nullptr不存在
        StockDataPtr find(const wxString& key) {
            if (key.empty()) {
                return StockDataPtr(nullptr);
            }
            auto idx_it = cache_index_.find(key);
            if (idx_it == cache_index_.end()) {
                return StockDataPtr(nullptr);
            }
            return indexAt(idx_it->second);
        }

        // 将指定key移动到目标下标pos位置
        bool move_to(const wxString& key, size_t target_pos) {
            if (key.empty()) {
                return false;
            }
            auto idx_it = cache_index_.find(key);
            if (idx_it == cache_index_.end()) {
                return false;
            }

            // 取出当前所在位置
            size_t origin_pos = idx_it->second;
            if (origin_pos == target_pos) {
                return true;
            }

            // 取出要移动的元素
            auto target_item = data_[origin_pos];
            // 删除原位置元素
            data_.erase(data_.begin() + origin_pos);
            // 插入到目标位置
            data_.insert(data_.begin() + target_pos, target_item);

            // 重建索引：erase+insert会改变所有下标，必须刷新哈希表
            rebuild_index();
            return true;
        }

        // 删除指定股票
        bool remove(const wxString& key)
        {
            if (key.empty()) {
                return false;
            }
            auto idx_it = cache_index_.find(key);
            if (idx_it == cache_index_.end())
                return false;

            size_t pos = idx_it->second;
            data_.erase(data_.begin() + pos);
            rebuild_index();
            return true;
        }

        void clear()
        {
            data_.clear();
            cache_index_.clear();
        }

        // 回调遍历所有股票
        template<typename Func>
        void ForEach(Func func)
        {
            for (auto&& item : data_)
                func(item->first, item->second);
        }

        template<typename Func>
        void ForEach(Func func) const
        {
            for (const auto &item : data_)
                func(item->first, item->second);
        }

        // 获取当前元素总数
        size_t size() const { return data_.size(); }
    };
}

class LDataManager
{
public:
    LDataManager(const LDataManager &) = delete;
    LDataManager &operator=(const LDataManager &) = delete;

    static LDataManager &Instance();
    ~LDataManager();

public:
    const wxString GetModuleParentPath();
    const wxString GetModuleName();
    const wxArrayString GetAllCodes();
    wxVector<StockDataPtr> AllStocks();
    long RealtimeRefreshFreq() const;
    void RealtimeRefreshFreq(long freq);
    bool IsDisplayName() const;
    void IsDisplayName(bool flag);
    bool IsDisplayColor() const;
    void IsDisplayColor(bool flag);
    bool IsDisplayRightAlign() const;
    void IsDisplayRightAlign(bool flag);
    bool IsPriorityDisplayChanged() const;
    void IsPriorityDisplayChanged(bool flag);
    bool IsDisplayCost() const;
    void IsDisplayCost(bool flag);
    bool IsDisplayCostProfitPrice() const;
    void IsDisplayCostProfitPrice(bool flag);
    bool IsDisplayCostProfitPercent() const;
    void IsDisplayCostProfitPercent(bool flag);
    bool IsDisplayAlias() const;
    void IsDisplayAlias(bool flag);
    // 走势图显示天数（1~5）
    int HistoryDays() const;
    // 走势图渲染模式：0=铺满（数据铺满整个宽度，视觉饱满），1=固定时间轴（按真实时间留白，末尾贴右）
    int HistoryGraphMode() const;
    void HistoryGraphMode(int mode);
    // 走势图样式：0=折线（只画线条），1=面积（线条下方实心填充）
    int HistoryGraphStyle() const;
    void HistoryGraphStyle(int style);
    // 面积图颜色：深色/浅色模式各一套（COLORREF）
    COLORREF HistoryGraphColorDark() const;
    void HistoryGraphColorDark(COLORREF color);
    COLORREF HistoryGraphColorLight() const;
    void HistoryGraphColorLight(COLORREF color);
    // 是否显示面积图虚线外框
    bool HistoryGraphDashedBox() const;
    // 日分隔竖线（已结束交易日 + 当前价位置标记）是否显示
    bool HistoryGraphShowSepLine() const;
    void HistoryGraphShowSepLine(bool flag);
    // 分隔线颜色：深色模式 / 浅色模式各一个
    COLORREF HistoryGraphSepLineColorDark() const;
    void HistoryGraphSepLineColorDark(COLORREF color);
    COLORREF HistoryGraphSepLineColorLight() const;
    void HistoryGraphSepLineColorLight(COLORREF color);
    void HistoryGraphDashedBox(bool flag);
    void HistoryDays(int days);
    bool IsDrawHistoryGraph() const;
    void IsDrawHistoryGraph(bool flag);
    bool IsScrollEnable() const;
    void IsScrollEnable(bool flag);
    int ScrollPageSize() const;
    void ScrollPageSize(int num);
    int ScrollInterval() const;
    void ScrollInterval(int num);
    // 是否存在设置了成本价的股票
    bool HasStockWithCostPrice() const;
    void KLineWH(int w, int h);
    void KLineW(int w);
    int KLineW() const;
    void KLineH(int h);
    int KLineH() const;
    std::pair<int, int> KLineWH() const;
    int DecimalPlaces() const;
    void DecimalPlaces(int num);

    void InitConfig();
    void InitTasks();
    void ReLoadConfig();
    void ReLoadAllStocks();
    void SaveConfig();

    bool IsChangeStockList();
    
    void ClearAllCodes();
    void AddStock(StockDataPtr data);
    bool HandleStockRealtimeData(wxString jsonp);

    // 闭市退避：按本轮行情「是否变化」决定下一次实时刷新的间隔。
    // 连续多轮行情完全不变 => 判定闭市，逐档拉长间隔（30/60/120/300 秒，不低于用户设定频率）；
    // 一旦变化、或命中「开盘前探针」窗口 => 立即恢复用户频率，保证开盘秒级响应。
    // quoteSignature 传本轮实时行情的原始响应文本（只做逐字节比较，不需要解析、也不做编码转换）。
    void UpdateRefreshThrottle(const std::string &quoteSignature);
    // 复位闭市退避（用户显式改了刷新频率时调用，否则新频率会被退避档位压着）
    void ResetRefreshThrottle();

    StockDataPtr GetStockByCode(const wxString& code);
    StockDataPtr GetStockByIndex(WXUINT index);
    bool DeleteStockByCode(const wxString& code);
    bool SetStockPosition(const StockDataPtr ptr, size_t pos);

    // ==================== 5 日分时（走势图数据源） ====================
    // 取某只股票的 5 日分时价格序列，按时间升序（index 0 最早）。
    // 返回的是值拷贝：UI 线程（DrawItem）拿到快照后不再持锁，刷新线程可以随时覆盖缓存。
    // 序列已按 HistoryDays() 截成「最近 N 个交易日」，绘制侧直接用即可，不需要再按日切分。
    // 还没有拉取成功（首次请求未回来 / 网络失败 / 该市场不支持）时返回空 vector，调用侧按无数据处理。
    StockHistorySeries GetHistoryPrices(const wxString &code) const;
    // 是否已有可用的分时缓存（不拷贝序列），给绘制侧判断要不要等数据
    bool HasHistoryPrices(const wxString &code) const;
    // 立即刷新全部自选股的分时缓存（同步发网络请求，勿在 UI 线程直接调用）。
    // 由任务 TASK_KEY_STOCK_HISTORY_INIT（首拉一次）与 TASK_KEY_STOCK_HISTORY_REFRESH（60 秒周期）调用。
    void RefreshHistoryPrices();
    // 清空分时缓存
    void ClearHistoryPrices();

    static void OnRefreshStockTimelineData(const std::string& callback_id, const std::string& data);

private:
    LDataManager();

    // 拉取单只股票/期货的分时序列：成功返回 true 并填充 out，失败返回 false（调用侧保留旧缓存）
    bool FetchHistoryPrices(const wxString &code, StockHistorySeries &out);

private:
    struct StockConfgInfo
    {
        // 实时数据刷新频率，单位秒
        long realtimeRefreshFreq;
        // 所有股票数据
        wxVector<StockDataPtr> datas;
        // 是否显示股票名称
        bool isDisplayName;
        // 是否开启颜色显示
        bool isDisplayColor;
        // 是否优先显示变动
        bool isPriorityDisplayChanged;
        // 是否显示成本价
        bool isDisplayCost;
        // 是否显示价差（当前价-成本价）
        bool isDisplayCostProfitPrice;
        // 是否显示盈亏涨跌幅
        bool isDisplayCostProfitPercent;
        // 是否显示别名（以股票代码代替名称）
        bool isDisplayAlias;
        // 是否绘制历史走势图背景（横向滚动图）
        bool isDrawHistoryGraph;
        // 走势图显示天数，1~5
        int historyDays;
        // 走势图渲染模式（0=铺满 / 1=固定时间轴）
        int historyGraphMode;
        // 走势图样式（0=折线 / 1=面积）
        int historyGraphStyle;
        // 面积图颜色（深色模式 / 浅色模式）
        COLORREF historyGraphColorDark;
        COLORREF historyGraphColorLight;
        // 是否显示面积图虚线外框（默认 false）
        bool historyGraphDashedBox;
    // 是否显示日分隔竖线（含当前价位置标记）
    bool historyGraphShowSepLine;
    // 分隔线颜色（深色 / 浅色）
    COLORREF historyGraphSepLineColorDark;
    COLORREF historyGraphSepLineColorLight;
        // 是否开启滚动显示
        bool isScrollEnable;
        // 滚动显示每页数量
        int scrollPageSize;
        // 滚动间隔（秒）
        int scrollInterval;
        int klineW;
        int klineH;
        // 小数点位数
        int decimalPlaces;
    };

    static LDataManager m_instance;

    wxString m_moduleParentPath;
    wxString m_moduleName;

    StockConfgInfo m_stockConfig;
    // 数值右对齐
    bool m_isDisplayRightAlign;

    LStockCacheManager m_stock_cache;
    // 保护 m_stock_cache（wxVector + 哈希索引）与 m_stockConfig.datas 的互斥锁。
    // 两者会被 UI 线程（增删改/遍历）、实时刷新工作线程（HandleStockRealtimeData/GetAllCodes）、
    // socket 工作线程（GetStockByCode）并发访问；insert/remove/move_to 会重建哈希索引，
    // 与并发 find/遍历竞争会损坏哈希表。
    mutable std::recursive_mutex m_mtxData;

    // ---- 闭市退避状态（只被 UpdateRefreshThrottle / ResetRefreshThrottle 访问）----
    // 实时刷新任务与一次性 INIT_LOAD 任务用的是同一个回调，理论上可能并发进入，故单独加锁。
    // 注意加锁顺序：本锁必须在 m_mtxData 之后取（UpdateRefreshThrottle 里先取完自选再加本锁）。
    std::mutex m_mtxRefreshThrottle;
    // 当前退避档位（下标，见 REFRESH_BACKOFF_SECS）
    size_t m_refreshBackoffLevel{0};
    // 连续「行情无变化」的轮数，累计到阈值就升一档
    int m_refreshStillRounds{0};
    // 是否已有可比对的上一轮行情
    bool m_refreshHasLast{false};
    // 上一轮行情的原始响应文本（用它当「有没有在动」的判据，不解析字段）
    std::string m_refreshLastSignature;
    // 最近一次实际下发到任务的间隔（0 = 未知，需重下发），避免每轮都去动任务表
    unsigned int m_refreshAppliedMs{0};
    // ---- 5 日分时缓存 ----
    // code -> 价格序列（时间升序，index 0 最早，已按 historyDays 截成最近 N 个交易日）。
    // UI 线程每次重绘都会读，任务线程写入。用独立锁而不是 m_mtxData：拉取分时要发网络请求（秒级），
    // 不能占着自选列表的锁，否则 UI 的增删改与遍历会被长时间阻塞。两把锁互不嵌套，无死锁风险。
    mutable std::mutex m_mtxHistory;
    std::map<wxString, StockHistorySeries> m_historyPrices;
    //wxVector<wxSharedPtr<LStockData>> m_cacheStocks;
    //StockDataMap m_cacheStockMap;
};
