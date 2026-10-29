#include "pch.h"
#include "StockItem.h"
#include "StockV2.h"
#include <wx/dcgraph.h>
#include <cmath>
#include <climits>

namespace
{
    // 计算所有股票中最长的「名称: 」显示宽度，用于名称列右对齐（使各条目价格起点对齐）
    int GetMaxStockNameWidth(CDC *pDC)
    {
        int maxWidth = 0;
        auto stocks = g_data.AllStocks();
        for (const auto &stock : stocks)
        {
            if (stock && !stock->GetName().empty())
            {
                wxString displayName = g_data.IsDisplayAlias() ? stock->GetCode() : stock->GetName();
                CString text = (displayName + ": ").wc_str();
                int w = pDC->GetTextExtent(text).cx;
                if (w > maxWidth)
                    maxWidth = w;
            }
        }
        return maxWidth;
    }
}

const wchar_t *LStockItem::GetItemName() const
{
    auto data = g_data.GetStockByIndex(index);
    if (data) {
        // 用线程本地缓冲返回副本：工作线程每 10 秒会并发重写 data->name，
        // 直接返回 data->name 的内部指针会因并发重写而悬空
        static thread_local wxString s_name;
        s_name = data->GetName();
        return s_name.wc_str();
    }
    return UtilResHlp.StringRes(IDS_LOADING);
}

const wchar_t *LStockItem::GetItemId() const
{
    static std::wstring item_id;
    item_id = L"qL0KmmYi";
    // 用槽位下标生成稳定 ID：主程序用 GetItemId 作为显示项的持久化唯一标识，
    // 滚动时 index 会变，若用它会导致 ID 漂移、主程序找不到对应显示项而跳过绘制。
    item_id += std::to_wstring(slotIndex);
    return item_id.c_str();
}

const wchar_t *LStockItem::GetItemLableText() const
{
    return L"";
}

const wchar_t *LStockItem::GetItemValueText() const
{
    return L"";
}

bool LStockItem::IsCustomDraw() const
{
    return true;
}

CString LStockItem::GetDisplayContent(wxSharedPtr<STOCK::LStockData> data, bool include_name) const
{
    if (data == nullptr) {
        return L"";
    }
    wxString content;
    if (include_name)
        content = content + data->GetName() + ": ";
    content += g_data.IsPriorityDisplayChanged() ? data->GetChangePrice() : data->GetCurrentPrice();
    content += " ";
    content += data->GetChangeFluctuation();
    return content.wc_str();
}

CString LStockItem::GetCostLabelContent(wxSharedPtr<STOCK::LStockData> data) const
{
    if (data == nullptr || data->costPrice <= 0.0 || !g_data.IsDisplayCost()) {
        return L"";
    }
    return (wxString(" ") + data->GetCostPriceText()).wc_str();
}

CString LStockItem::GetCostDirContent(wxSharedPtr<STOCK::LStockData> data) const
{
    if (data == nullptr || data->costPrice <= 0.0) {
        return L"";
    }

    wxString content;
    if (g_data.IsDisplayCostProfitPrice() && !data->GetCostProfitPrice().empty()) {
        content += data->GetCostProfitPrice();
    }
    if (g_data.IsDisplayCostProfitPercent() && !data->GetCostProfitPercent().empty()) {
        if (!content.empty()) content += " ";
        content += data->GetCostProfitPercent();
    }

    if (content.empty()) {
        return L"";
    }
    // 与前面的价格/成本文本之间补一个空格分隔
    return (wxString(" ") + content).wc_str();
}

int LStockItem::GetItemWidthEx(void* hDC) const
{
    CDC* pDC = CDC::FromHandle((HDC)hDC);

    auto data = g_data.GetStockByIndex(index);

    int width = 0;
    if (g_data.IsDisplayName())
    {
        // 名称列取最长名称宽度，保证各条目价格起点对齐
        width = GetMaxStockNameWidth(pDC);
    }
    width += pDC->GetTextExtent(GetDisplayContent(data, false)).cx;

    CString costLabel = GetCostLabelContent(data);
    if (!costLabel.IsEmpty())
    {
        width += pDC->GetTextExtent(costLabel).cx;
    }

    CString costDir = GetCostDirContent(data);
    if (!costDir.IsEmpty())
    {
        width += pDC->GetTextExtent(costDir).cx;
    }

    LLOG_DEBUG("GetItemWidthEx: %d", width);
    return width;
}
StockHistorySeries LStockItem::GetHistoryPrices()
{
    // 取当前显示股票的分时序列（真实数据，由 LDataManager 后台任务拉取并缓存）。
    // 缓存键 = 股票代码 + 天数：滚动显示时 index 会变，用户也可能改天数，任一变化都要重取。
    auto data = g_data.GetStockByIndex(index);
    wxString code = data ? data->code : wxString();
    int days = g_data.HistoryDays();

    if (code != m_historyCacheCode || days != m_historyCacheDays)
    {
        m_historySeries = StockHistorySeries{};
        m_historyCacheCode = code;
        m_historyCacheDays = days;
    }

    if (code.IsEmpty())
    {
        return m_historySeries;
    }

    m_historySeries = g_data.GetHistoryPrices(code);
    return m_historySeries;
}

void LStockItem::DrawHistoryGraph(CDC* pDC, const CRect& rect, bool dark_mode)
{
    const int width = rect.Width();
    const int height = rect.Height();
    if (width <= 0 || height <= 0)
        return;

    StockHistorySeries series = GetHistoryPrices();
    if (series.empty())
        return;
    if (series.totalPoints() < 2)
        return;

    const int bottom = rect.bottom - 1;
    const int usable = height - 1;
    const int slotsPerDay = series.slotsPerDay;
    const int dayCount = static_cast<int>(series.days.size());

    // ---- 时间槽模式：X 轴 = 固定时间槽，不是数据点索引 ----
    // totalSlots = 天数 * 每日满槽数；usedSlots = 已完成日的满槽 + 最后一天实际点数。
    // 只画 slotIdx < usedSlots 的部分，右侧（未来时段）留白，所以当前时刻不再贴右边缘。

    // 每列的价格累积与计数：列内多点取均值（降采样），列间按日边界切开
    std::vector<double> colSum(width, 0.0);
    std::vector<int> colCnt(width, 0);

    // 把一段价格序列按实际点数比例铺到像素列区间 [cLo, cHi) 上
    auto spreadToColumns = [&colSum, &colCnt](const std::vector<double>& pts, int cLo, int cHi)
    {
        const int pn = static_cast<int>(pts.size());
        const int cols = cHi - cLo;
        if (pn <= 0 || cols <= 0)
            return;
        for (int j = 0; j < pn; ++j)
        {
            const double r0 = static_cast<double>(j) / pn;
            const double r1 = static_cast<double>(j + 1) / pn;
            int a = cLo + static_cast<int>(std::floor(r0 * cols));
            int b = cLo + static_cast<int>(std::ceil(r1 * cols));
            if (b <= a) b = a + 1;
            if (a < cLo) a = cLo;
            if (b > cHi) b = cHi;
            for (int c = a; c < b; ++c)
            {
                colSum[c] += pts[j];
                colCnt[c] += 1;
            }
        }
    };

    if (slotsPerDay > 0)
    {
        const double totalSlots = static_cast<double>(dayCount) * slotsPerDay;
        for (int di = 0; di < dayCount; ++di)
        {
            const std::vector<double>& day = series.days[di];
            const int pn = static_cast<int>(day.size());
            if (pn <= 0)
                continue;

            // 已完成日占满自己的槽段；最后一天只占实际已走的槽位（超过满槽数的钳住）
            int localUsed = slotsPerDay;
            if (di == dayCount - 1 && pn < slotsPerDay)
                localUsed = pn;

            // 该天的槽段换算成连续像素区间，再取整成它独占的列区间。
            // 相邻天共用同一个边界取整函数，保证首尾对齐、不重不漏、不跨日混合。
            const double slotBase = static_cast<double>(di) * slotsPerDay;
            const double fx0 = slotBase * width / totalSlots;
            const double fx1 = (slotBase + localUsed) * width / totalSlots;
            int cLo = static_cast<int>(std::lround(fx0));
            int cHi = static_cast<int>(std::lround(fx1));
            if (cHi <= cLo)
                cHi = cLo + 1;   // 该天不足一像素列：并入起点那一列，数据不丢
            if (cLo < 0) cLo = 0;
            if (cHi > width) cHi = width;
            if (cHi <= cLo)
                continue;

            spreadToColumns(day, cLo, cHi);
        }
    }
    else
    {
        // slotsPerDay 未知（<=0）：退化为按实际点数铺满整个宽度（保留旧行为）
        std::vector<double> flat;
        flat.reserve(series.totalPoints());
        for (const auto& d : series.days)
            flat.insert(flat.end(), d.begin(), d.end());
        if (flat.size() < 2)
            return;
        spreadToColumns(flat, 0, width);
    }

    // 价格基准：全序列最高/最低自适应（最后一天超出满槽数、实际不画的部分不参与量程）
    double minPrice = 0.0;
    double maxPrice = 0.0;
    bool hasPrice = false;
    for (int di = 0; di < dayCount; ++di)
    {
        const std::vector<double>& day = series.days[di];
        int pn = static_cast<int>(day.size());
        if (slotsPerDay > 0 && di == dayCount - 1 && pn > slotsPerDay)
            pn = slotsPerDay;
        for (int j = 0; j < pn; ++j)
        {
            if (!hasPrice)
            {
                minPrice = day[j];
                maxPrice = day[j];
                hasPrice = true;
            }
            else
            {
                if (day[j] < minPrice) minPrice = day[j];
                if (day[j] > maxPrice) maxPrice = day[j];
            }
        }
    }
    if (!hasPrice)
        return;
    const double range = maxPrice - minPrice;

    // 逐列绘制底部对齐的竖线，形成面积填充效果
    COLORREF graphColor = dark_mode ? g_data.HistoryGraphColorDark() : g_data.HistoryGraphColorLight();


    CPen pen(PS_SOLID, 1, graphColor);
    CPen* pOldPen = pDC->SelectObject(&pen);

    for (int c = 0; c < width; ++c)
    {
        if (colCnt[c] <= 0)
            continue;   // 未走到的时段（右侧留白）不画

        int x = rect.left + c;
        if (x < rect.left)
            continue;
        if (x > rect.right - 1)
            break;

        const double value = colSum[c] / static_cast<double>(colCnt[c]);

        int y;
        if (range <= 0.0)
        {
            y = rect.top + height / 2;
        }
        else
        {
            double ratio = (value - minPrice) / range;
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;
            y = bottom - static_cast<int>(std::lround(ratio * usable));
        }
        if (y < rect.top) y = rect.top;
        if (y > bottom) y = bottom;

        pDC->MoveTo(x, bottom);
        pDC->LineTo(x, y);
    }

    pDC->SelectObject(pOldPen);

    // 可选：面积图外圈虚线框（与面积图同色）。PS_DOT 必须配 1px 笔宽 + 透明背景，
    // 否则虚线间隙会被背景色填成实线；用 MoveTo/LineTo 画四条边，不用 Rectangle 以免填充。
    if (g_data.HistoryGraphDashedBox())
    {
        CPen dashPen(PS_DOT, 1, graphColor);
        CPen *pOldDashPen = pDC->SelectObject(&dashPen);
        const int oldBkMode = pDC->SetBkMode(TRANSPARENT);
        const int left = rect.left;
        const int top = rect.top;
        const int right = rect.right - 1;
        pDC->MoveTo(left, top);
        pDC->LineTo(right, top);
        pDC->LineTo(right, bottom);
        pDC->LineTo(left, bottom);
        pDC->LineTo(left, top);
        pDC->SetBkMode(oldBkMode);
        pDC->SelectObject(pOldDashPen);
    }
}



void LStockItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    auto data = g_data.GetStockByIndex(index);
    if (data == nullptr) {
        return;
    }

    // 绘图句柄
    CDC* pDC = CDC::FromHandle((HDC)hDC);

    // 矩形区域
    CRect rect(CPoint(x, y), CSize(w, h));

    // 历史走势图背景：先画图后画字，使走势图位于文字下层
    if (g_data.IsDrawHistoryGraph())
    {
        DrawHistoryGraph(pDC, rect, dark_mode);
    }
    else
    {
        // 关闭时清空缓存，避免再次开启时显示过期数据
        if (!m_historySeries.empty() || !m_historyColumns.empty())
        {
            m_historySeries = StockHistorySeries{};
            m_historyColumns.clear();
        }
    }

    // 文本颜色
    COLORREF color_default;
    COLORREF color_red;
    COLORREF color_green;
    if (dark_mode)
    {
        color_default = RGB(255, 255, 255);
        color_red = RGB(255, 121, 120);
        color_green = RGB(111, 215, 149);
    }
    else
    {
        color_default = RGB(0, 0, 0);
        color_red = RGB(195, 0, 0);
        color_green = RGB(46, 139, 87);
    }

    // 各段文本
    CString nameText;
    if (g_data.IsDisplayName())
    {
        wxString displayName = g_data.IsDisplayAlias() ? data->GetCode() : data->GetName();
        nameText = (displayName + ": ").wc_str();
    }
    CString valueText = GetDisplayContent(data, false);
    CString costLabel = GetCostLabelContent(data);
    CString costDir = GetCostDirContent(data);

    // 各段宽度（名称列取最长名称宽度，保证各条目价格起点对齐）
    int nameColWidth = g_data.IsDisplayName() ? GetMaxStockNameWidth(pDC) : 0;
    int valueWidth = pDC->GetTextExtent(valueText).cx;
    int labelWidth = costLabel.IsEmpty() ? 0 : pDC->GetTextExtent(costLabel).cx;
    int dirWidth = costDir.IsEmpty() ? 0 : pDC->GetTextExtent(costDir).cx;

    // 绘制起点：整体靠左，名称右对齐到名称列后其余内容依次排列
    int cursor = rect.left;

    UINT flags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;

    // 1. 名称（右对齐到「最长名称」列）
    if (g_data.IsDisplayName())
    {
        pDC->SetTextColor(color_default);
        CRect rc{ rect };
        rc.left = cursor;
        rc.right = cursor + nameColWidth;
        pDC->DrawText(nameText, rc, flags | DT_RIGHT);
        cursor += nameColWidth;
    }

    // 2. 数值（价格 + 涨跌幅）
    if (g_data.IsDisplayColor())
    {
        bool isUp = data->GetChangeFluctuation().find('+') != wxString::npos;
        if (isUp)
            pDC->SetTextColor(color_red);
        else
            pDC->SetTextColor(color_green);
    }
    else
    {
        pDC->SetTextColor(color_default);
    }
    CRect rcValue{ rect };
    rcValue.left = cursor;
    rcValue.right = cursor + valueWidth;
    pDC->DrawText(valueText, rcValue, flags);
    cursor += valueWidth;

    // 3. 成本价标签（中性色）
    if (!costLabel.IsEmpty())
    {
        pDC->SetTextColor(color_default);
        CRect rc{ rect };
        rc.left = cursor;
        rc.right = cursor + labelWidth;
        pDC->DrawText(costLabel, rc, flags);
        cursor += labelWidth;
    }

    // 4. 差额/涨跌幅：盈利红、亏损绿、持平用默认色（不变）
    if (!costDir.IsEmpty())
    {
        if (g_data.IsDisplayColor())
        {
            int dir = data->GetCostProfitDirection();
            if (dir > 0)
                pDC->SetTextColor(color_red);
            else if (dir < 0)
                pDC->SetTextColor(color_green);
            else
                pDC->SetTextColor(color_default);
        }
        else
        {
            pDC->SetTextColor(color_default);
        }
        CRect rc{ rect };
        rc.left = cursor;
        rc.right = cursor + dirWidth;
        pDC->DrawText(costDir, rc, flags);
    }
}

const wchar_t *LStockItem::GetItemValueSampleText() const
{
    return L"--";
}

int LStockItem::OnMouseEvent(MouseEventType type, int x, int y, void *hWnd, int flag)
{
    CWnd *pWnd = CWnd::FromHandle((HWND)hWnd);
    LLOG_DEBUG("OnMouseEvent: %d", type);
    CPoint ptScreen = CPoint(x, y);
    switch (type)
    {
    case IPluginItem::MT_RCLICKED:
        LStockPlugin::Instance()->ShowStockViewMenu(hWnd, ptScreen, g_data.GetStockByIndex(index));
        return 1;

    case IPluginItem::MT_LCLICKED:
    {
        LStockPlugin::Instance()->ShowStockView(index);
    }
    default:
        break;
    }
    return 0;
}
