#include "pch.h"
#include "StockItem.h"
#include "StockV2.h"
#include <wx/dcgraph.h>

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
