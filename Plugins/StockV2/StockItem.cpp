#include "pch.h"
#include "StockItem.h"
#include "StockV2.h"
#include <wx/dcgraph.h>
#include <cmath>
#include <climits>
#include <string>
#include <unordered_map>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace
{
    // 条目 DIB 缓存的最长复用时间（毫秒）：起点是宿主重绘背景的时刻未必可知，
    // 用一个较短的时限保证最坏情况下缓存残影在 1.2 秒内自愈。
    const unsigned long long item_dib_cache_ttl_ms = 1200ULL;
}

namespace
{
    class GdiplusInitializer
    {
    public:
        GdiplusInitializer()
        {
            Gdiplus::GdiplusStartupInput input;
            m_status = Gdiplus::GdiplusStartup(&m_token, &input, nullptr);
        }

        ~GdiplusInitializer()
        {
            if (m_status == Gdiplus::Ok)
            {
                Gdiplus::GdiplusShutdown(m_token);
            }
        }

        bool IsReady() const
        {
            return m_status == Gdiplus::Ok;
        }

    private:
        ULONG_PTR m_token = 0;
        Gdiplus::Status m_status = Gdiplus::GenericError;
    };

    GdiplusInitializer &GetGdiplusInitializer()
    {
        static GdiplusInitializer initializer;
        return initializer;
    }

    // GDI+ 文本对象缓存：Font 按 (LOGFONT, LOGPIXELSY) 复用，SolidBrush 复用并 SetColor，
    // StringFormat 按对齐方式常驻。绘制与宽度度量共用同一套对象，保证度量与绘制严格同源。
    // 全部用 static 指针 + 首次 new 且永不 delete，避免静态对象析构晚于 GdiplusShutdown；thread_local 保证线程安全。
    struct GdiTextResources
    {
        Gdiplus::Font *font = nullptr;
        LOGFONT fontLogFont{};
        int fontLogPixelsY = -1;
        Gdiplus::SolidBrush *brush = nullptr;
        Gdiplus::StringFormat *formatNear = nullptr;
        Gdiplus::StringFormat *formatFar = nullptr;
        Gdiplus::StringFormat *formatMeasure = nullptr;
    };

    GdiTextResources &GetGdiTextResources()
    {
        static thread_local GdiTextResources resources;
        return resources;
    }

    // 度量结果缓存：MeasureString 每次都要建 Graphics，开销远大于 GetTextExtent；
    // 同一字体/DPI 下的同一串文本宽度是常量，按 (字体签名, DPI) 失效后复用，避免每帧重复度量。
    struct TextWidthCache
    {
        bool valid = false;
        LOGFONT fontLogFont{};
        int fontLogPixelsY = -1;
        std::unordered_map<std::wstring, int> widths;
    };

    TextWidthCache &GetTextWidthCache()
    {
        static thread_local TextWidthCache cache;
        return cache;
    }

    // 度量专用绘图面：MeasureString 每次构造 Graphics 都要新建 HDC 相关资源，开销远超度量本身。
    // 这里用一个常驻 1x1 内存 DC 承载一个常驻 Graphics，只做度量、不做输出，避免每帧反复创建。
    struct MeasureSurface
    {
        HDC memDC = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        Gdiplus::Graphics *graphics = nullptr;
        int logPixelsY = -1;
    };

    MeasureSurface &GetMeasureSurface()
    {
        static thread_local MeasureSurface surface;
        return surface;
    }

    // 取常驻度量 Graphics；DPI 变化时重建（与参考 HDC 兼容，保证度量与绘制同一 DPI）
    Gdiplus::Graphics *AcquireMeasureGraphics(HDC referenceHdc, int logPixelsY)
    {
        MeasureSurface &surface = GetMeasureSurface();
        if (surface.graphics != nullptr && surface.logPixelsY == logPixelsY)
        {
            return surface.graphics;
        }

        delete surface.graphics;
        surface.graphics = nullptr;
        if (surface.memDC != nullptr)
        {
            if (surface.oldBitmap != nullptr)
            {
                ::SelectObject(surface.memDC, surface.oldBitmap);
            }
            if (surface.bitmap != nullptr)
            {
                ::DeleteObject(surface.bitmap);
            }
            ::DeleteDC(surface.memDC);
            surface.memDC = nullptr;
            surface.bitmap = nullptr;
            surface.oldBitmap = nullptr;
        }

        HDC memDC = ::CreateCompatibleDC(referenceHdc);
        if (memDC == nullptr)
        {
            return nullptr;
        }
        HBITMAP bitmap = ::CreateCompatibleBitmap(memDC, 1, 1);
        if (bitmap == nullptr)
        {
            ::DeleteDC(memDC);
            return nullptr;
        }
        surface.memDC = memDC;
        surface.bitmap = bitmap;
        surface.oldBitmap = ::SelectObject(memDC, bitmap);
        surface.graphics = new Gdiplus::Graphics(memDC);
        surface.graphics->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        surface.logPixelsY = logPixelsY;
        return surface.graphics;
    }

    // LOGFONT 全字段比较（判断缓存的 GDI+ Font 是否仍适用于当前字体环境）
    bool IsSameLogFontExact(const LOGFONT &a, const LOGFONT &b)
    {
        if (a.lfHeight != b.lfHeight || a.lfWidth != b.lfWidth ||
            a.lfEscapement != b.lfEscapement || a.lfOrientation != b.lfOrientation ||
            a.lfWeight != b.lfWeight || a.lfItalic != b.lfItalic ||
            a.lfUnderline != b.lfUnderline || a.lfStrikeOut != b.lfStrikeOut ||
            a.lfCharSet != b.lfCharSet || a.lfOutPrecision != b.lfOutPrecision ||
            a.lfClipPrecision != b.lfClipPrecision || a.lfQuality != b.lfQuality ||
            a.lfPitchAndFamily != b.lfPitchAndFamily)
        {
            return false;
        }
        for (int i = 0; i < LF_FACESIZE; ++i)
        {
            if (a.lfFaceName[i] != b.lfFaceName[i])
            {
                return false;
            }
        }
        return true;
    }

    // 取当前 HDC 的 GDI+ 字体（命中缓存则复用）。失败返回 nullptr。
    // temporary=true 表示「新建但未写入缓存」，调用方用完须 delete。
    Gdiplus::Font *AcquireGdiFont(CDC *pDC, HDC hdc, GdiTextResources &resources, bool &temporary)
    {
        temporary = false;
        if (pDC == nullptr)
        {
            return nullptr;
        }

        LOGFONT logFont{};
        CFont *font = pDC->GetCurrentFont();
        if (font == nullptr || font->GetLogFont(&logFont) == 0)
        {
            return nullptr;
        }

        const int logPixelsY = ::GetDeviceCaps(hdc, LOGPIXELSY);
        if (resources.font != nullptr && resources.fontLogPixelsY == logPixelsY &&
            IsSameLogFontExact(resources.fontLogFont, logFont))
        {
            return resources.font;
        }

        Gdiplus::Font *pFreshFont = new Gdiplus::Font(hdc, &logFont);
        if (pFreshFont->GetLastStatus() != Gdiplus::Ok)
        {
            // 与原实现一致：字体创建失败时仍用它绘制（结果为不画），用完即弃、不写缓存。
            temporary = true;
            return pFreshFont;
        }

        Gdiplus::Font *pOldFont = resources.font;
        resources.font = pFreshFont;
        resources.fontLogFont = logFont;
        resources.fontLogPixelsY = logPixelsY;
        if (pOldFont != nullptr)
        {
            delete pOldFont; // 旧字体只被缓存持有，不会与本次绘制冲突
        }
        return pFreshFont;
    }

    // 文本是否走 GDI+ 绘制路径（只有这条路径才需要用 GDI+ 同源度量，其余走 GDI 度量保持原样）
    bool IsGdiPlusTextPath(CDC *pDC)
    {
        if (pDC == nullptr)
        {
            return false;
        }
        const int transparency = g_data.TextTransparency();
        return transparency > 0 && transparency < 100 && GetGdiplusInitializer().IsReady();
    }

    // 文本宽度度量：字体与 StringFormat 与 DrawTextWithTransparency 完全同源（GDI+ MeasureString），
    // 避免 GDI 度量与 GDI+ 绘制 advance 不一致导致末段被裁 / 列不齐。
    // 非 GDI+ 绘制路径（透明 0 或 100、GDI+ 不可用、字体取不到）回退 GetTextExtent，与原行为一致。
    int MeasureTextWidth(CDC *pDC, const CString &text)
    {
        if (pDC == nullptr)
        {
            return 0;
        }
        if (!IsGdiPlusTextPath(pDC))
        {
            return pDC->GetTextExtent(text).cx;
        }

        const HDC hdc = pDC->GetSafeHdc();
        GdiTextResources &resources = GetGdiTextResources();

        bool temporaryFont = false;
        Gdiplus::Font *pFont = AcquireGdiFont(pDC, hdc, resources, temporaryFont);
        if (pFont == nullptr)
        {
            return pDC->GetTextExtent(text).cx;
        }

        // 缓存查找（字体签名 + DPI 变化即整体失效）
        TextWidthCache &widthCache = GetTextWidthCache();
        const int logPixelsY = resources.fontLogPixelsY;
        if (widthCache.valid && widthCache.fontLogPixelsY == logPixelsY &&
            IsSameLogFontExact(widthCache.fontLogFont, resources.fontLogFont))
        {
            const std::wstring key(text.GetString(), static_cast<size_t>(text.GetLength()));
            auto hit = widthCache.widths.find(key);
            if (hit != widthCache.widths.end())
            {
                if (temporaryFont)
                {
                    delete pFont;
                }
                return hit->second;
            }
        }
        else
        {
            widthCache.widths.clear();
            widthCache.valid = true;
            widthCache.fontLogFont = resources.fontLogFont;
            widthCache.fontLogPixelsY = logPixelsY;
        }

        if (resources.formatMeasure == nullptr)
        {
            Gdiplus::StringFormat *pFormatMeasure = new Gdiplus::StringFormat();
            pFormatMeasure->SetAlignment(Gdiplus::StringAlignmentNear);
            pFormatMeasure->SetLineAlignment(Gdiplus::StringAlignmentCenter);
            pFormatMeasure->SetTrimming(Gdiplus::StringTrimmingNone);
            pFormatMeasure->SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                                           Gdiplus::StringFormatFlagsNoClip |
                                           Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
            resources.formatMeasure = pFormatMeasure;
        }

        int width = 0;
        Gdiplus::Graphics *pMeasureGraphics = AcquireMeasureGraphics(hdc, logPixelsY);
        if (pMeasureGraphics != nullptr)
        {
            // 布局矩形给足空间，确保 MeasureString 不做换行/截断，取到的就是完整推进宽度
            Gdiplus::RectF layoutRect(0.0f, 0.0f, 1.0e6f, 1.0e6f);
            Gdiplus::RectF bounds;
            if (pMeasureGraphics->MeasureString(text.GetString(), text.GetLength(), pFont, layoutRect,
                                                resources.formatMeasure, &bounds) == Gdiplus::Ok)
            {
                // 向上取整：绘制时的矩形宽度不小于实际推进宽度，避免 DrawString 因差 1 像素触发省略号
                width = static_cast<int>(std::ceil(bounds.Width));
            }
        }
        else
        {
            width = pDC->GetTextExtent(text).cx;
        }

        if (!temporaryFont)
        {
            // 只在字体确实被缓存持有时才写缓存（临时字体场景下键不可信，宁可下次重算）
            if (widthCache.widths.size() > 256)
            {
                widthCache.widths.clear();
            }
            widthCache.widths[std::wstring(text.GetString(), static_cast<size_t>(text.GetLength()))] = width;
        }

        if (temporaryFont)
        {
            delete pFont;
        }
        return width;
    }

    void DrawTextWithTransparency(CDC* pDC, const CString& text, const CRect& rect,
                                  COLORREF color, UINT format)
    {
        const int transparency = g_data.TextTransparency();
        if (transparency >= 100)
        {
            return;
        }
        if (transparency <= 0)
        {
            pDC->SetTextColor(color);
            CRect drawRect(rect);
            pDC->DrawText(text, drawRect, format);
            return;
        }
        if (!GetGdiplusInitializer().IsReady())
        {
            pDC->SetTextColor(color);
            CRect drawRect(rect);
            pDC->DrawText(text, drawRect, format);
            return;
        }

        const HDC hdc = pDC->GetSafeHdc();
        GdiTextResources &resources = GetGdiTextResources();

        bool temporaryFont = false;
        Gdiplus::Font *pGdiFont = AcquireGdiFont(pDC, hdc, resources, temporaryFont);
        if (pGdiFont == nullptr)
        {
            pDC->SetTextColor(color);
            CRect drawRect(rect);
            pDC->DrawText(text, drawRect, format);
            return;
        }

        Gdiplus::Graphics graphics(hdc);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

        const BYTE alpha = static_cast<BYTE>((100 - transparency) * 255 / 100);

        const Gdiplus::Color brushColor(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
        if (resources.brush == nullptr)
        {
            resources.brush = new Gdiplus::SolidBrush(brushColor);
        }
        else
        {
            resources.brush->SetColor(brushColor);
        }

        if (resources.formatNear == nullptr || resources.formatFar == nullptr)
        {
            Gdiplus::StringFormat *pFormatNear = new Gdiplus::StringFormat();
            pFormatNear->SetAlignment(Gdiplus::StringAlignmentNear);
            pFormatNear->SetLineAlignment(Gdiplus::StringAlignmentCenter);
            pFormatNear->SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            pFormatNear->SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                                        Gdiplus::StringFormatFlagsNoClip);

            Gdiplus::StringFormat *pFormatFar = new Gdiplus::StringFormat();
            pFormatFar->SetAlignment(Gdiplus::StringAlignmentFar);
            pFormatFar->SetLineAlignment(Gdiplus::StringAlignmentCenter);
            pFormatFar->SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            pFormatFar->SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                                       Gdiplus::StringFormatFlagsNoClip);

            resources.formatNear = pFormatNear;
            resources.formatFar = pFormatFar;
        }
        Gdiplus::StringFormat *pStringFormat =
            (format & DT_RIGHT) ? resources.formatFar : resources.formatNear;

        Gdiplus::RectF layoutRect(static_cast<Gdiplus::REAL>(rect.left),
                                  static_cast<Gdiplus::REAL>(rect.top),
                                  static_cast<Gdiplus::REAL>(rect.Width()),
                                  static_cast<Gdiplus::REAL>(rect.Height()));
        graphics.DrawString(text.GetString(), text.GetLength(), pGdiFont, layoutRect, pStringFormat,
                            resources.brush);

        if (temporaryFont)
        {
            delete pGdiFont;
        }
    }

    // 名称列宽度缓存条目：只保存最近一组 (字体签名, 内容指纹, 宽度)
    struct StockNameWidthCache
    {
        bool valid = false;
        LOGFONT font = {};
        unsigned long long contentHash = 0;
        bool gdiPlusPath = false;   // 走 GDI+ 度量还是 GDI 度量：两者宽度可能差 1像素，必须一起缓存
        int maxWidth = 0;
    };

    StockNameWidthCache &GetStockNameWidthCache()
    {
        static thread_local StockNameWidthCache cache;
        return cache;
    }

    // FNV-1a 64 位：把 64 位整数按 8 个字节混入
    void Fnv1aMixWord(unsigned long long &hash, unsigned long long value)
    {
        for (int i = 0; i < 8; ++i)
        {
            unsigned char byte = (unsigned char)((value >> (i * 8)) & 0xFFULL);
            hash = (hash ^ (unsigned long long)byte) * 1099511628211ULL;
        }
    }

    // FNV-1a 64 位：长度 + 逐 UTF-16 单元混入，避免相邻字符串拼接产生歧义
    void Fnv1aMixString(unsigned long long &hash, const wxString &text)
    {
        const size_t len = text.length();
        Fnv1aMixWord(hash, (unsigned long long)len);
        const wchar_t *units = text.wc_str();
        for (size_t i = 0; i < len; ++i)
        {
            unsigned long long unit = (unsigned long long)(unsigned short)units[i];
            hash = (hash ^ unit) * 1099511628211ULL;
        }
    }

    // 字体签名比较：lfFaceName 逐字节比较，其余关键字段逐个比较
    bool IsSameLogFontSignature(const LOGFONT &a, const LOGFONT &b)
    {
        if (a.lfHeight != b.lfHeight)
            return false;
        if (a.lfWeight != b.lfWeight)
            return false;
        if (a.lfItalic != b.lfItalic)
            return false;
        if (a.lfUnderline != b.lfUnderline)
            return false;
        if (a.lfStrikeOut != b.lfStrikeOut)
            return false;
        if (a.lfCharSet != b.lfCharSet)
            return false;
        const unsigned char *pa = (const unsigned char *)a.lfFaceName;
        const unsigned char *pb = (const unsigned char *)b.lfFaceName;
        for (size_t i = 0; i < sizeof(a.lfFaceName); ++i)
        {
            if (pa[i] != pb[i])
                return false;
        }
        return true;
    }

    // 计算所有股票中最长的「名称: 」显示宽度，用于名称列右对齐（使各条目价格起点对齐）
    int GetMaxStockNameWidth(CDC *pDC)
    {
        // 字体签名：取当前字体 LOGFONT；取不到时不走缓存，直接实时计算
        LOGFONT logFont = {};
        bool fontSignatureValid = false;
        if (pDC)
        {
            CFont *pCurrentFont = pDC->GetCurrentFont();
            if (pCurrentFont && pCurrentFont->GetLogFont(&logFont))
            {
                fontSignatureValid = true;
            }
        }

        auto stocks = g_data.AllStocks();
        const bool displayAlias = g_data.IsDisplayAlias();
        // 度量路径也决定宽度（GDI+ MeasureString vs GDI GetTextExtent），纳入缓存键
        const bool gdiPlusPath = IsGdiPlusTextPath(pDC);

        // 内容指纹：股票数量 + IsDisplayAlias + 每个 stock 的 GetName()/GetAlias()
        unsigned long long contentHash = 1469598103934665603ULL;
        Fnv1aMixWord(contentHash, (unsigned long long)stocks.size());
        Fnv1aMixWord(contentHash, displayAlias ? 1ULL : 0ULL);
        Fnv1aMixWord(contentHash, gdiPlusPath ? 1ULL : 0ULL);
        for (const auto &stock : stocks)
        {
            if (stock)
            {
                Fnv1aMixWord(contentHash, 1ULL);
                Fnv1aMixString(contentHash, stock->GetName());
                Fnv1aMixString(contentHash, stock->GetAlias());
            }
            else
            {
                Fnv1aMixWord(contentHash, 0ULL);
            }
        }

        StockNameWidthCache &cache = GetStockNameWidthCache();
        if (fontSignatureValid && cache.valid && cache.contentHash == contentHash &&
            cache.gdiPlusPath == gdiPlusPath && IsSameLogFontSignature(cache.font, logFont))
        {
            return cache.maxWidth;
        }

        int maxWidth = 0;
        for (const auto &stock : stocks)
        {
            if (stock)
            {
                wxString displayName = (g_data.IsDisplayAlias() && !stock->GetAlias().empty()) ? stock->GetAlias() : stock->GetName();
                CString text = (displayName + ": ").wc_str();
                int w = MeasureTextWidth(pDC, text);
                if (w > maxWidth)
                    maxWidth = w;
            }
        }

        if (fontSignatureValid)
        {
            cache.valid = true;
            cache.font = logFont;
            cache.contentHash = contentHash;
            cache.gdiPlusPath = gdiPlusPath;
            cache.maxWidth = maxWidth;
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
        s_name = (g_data.IsDisplayAlias() && !data->GetAlias().empty()) ? data->GetAlias() : data->GetName();
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
    width += MeasureTextWidth(pDC, GetDisplayContent(data, false));

    CString costLabel = GetCostLabelContent(data);
    if (!costLabel.IsEmpty())
    {
        width += MeasureTextWidth(pDC, costLabel);
    }

    CString costDir = GetCostDirContent(data);
    if (!costDir.IsEmpty())
    {
        width += MeasureTextWidth(pDC, costDir);
    }

    LLOG_DEBUG("GetItemWidthEx: %d", width);
    return width;
}
const StockHistorySeries &LStockItem::GetHistoryPrices()
{
    // 取当前显示股票的分时序列（真实数据，由 LDataManager 后台任务拉取并缓存）。
    // 缓存键 = 股票代码 + 天数 + 数据版本：滚动显示时 index 会变，用户也可能改天数，
    // 后台每次写入历史数据都会让版本号 +1；三者任一变化才重取，否则直接复用缓存。
    auto data = g_data.GetStockByIndex(index);
    wxString code = data ? data->code : wxString();
    int days = g_data.HistoryDays();
    const unsigned long long rev = g_data.HistoryRev();

    if (code != m_historyCacheCode || days != m_historyCacheDays)
    {
        m_historySeries = StockHistorySeries{};
        m_historyCacheCode = code;
        m_historyCacheDays = days;
        m_historyCacheRev = 0;
    }

    if (code.IsEmpty())
    {
        return m_historySeries;
    }

    // 版本未变且已有数据：直接返回缓存，不做任何深拷贝
    // 注意 rev 在拷贝前读取：即使期间数据被更新，最坏只是多取一次，不会读到陈旧数据
    if (rev == m_historyCacheRev && !m_historySeries.empty())
    {
        return m_historySeries;
    }

    if (g_data.GetHistoryPricesInto(code, m_historySeries))
    {
        m_historyCacheRev = rev;
    }
    else
    {
        m_historySeries = StockHistorySeries{};
        m_historyCacheRev = rev;
    }
    return m_historySeries;
}

void LStockItem::DrawHistoryGraph(CDC* pDC, const CRect& rect, bool dark_mode)
{
    const int width = rect.Width();
    const int height = rect.Height();
    if (width <= 0 || height <= 0)
        return;

    const StockHistorySeries &series = GetHistoryPrices();
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
    // 缓冲成员化以跨帧复用，assign 语义等同原来的 vector(width, 0.0)（定长 + 全填 0）
    m_colSum.assign(width, 0.0);
    m_colCnt.assign(width, 0);

    // 日分隔竖线：记录每个交易日末点的边界列下标与当日末价，面积图画完后统一绘制
    // clear 语义等同重新构造空 vector（不残留旧数据）
    m_daySeparators.clear();

    // 把一段价格序列按实际点数比例铺到像素列区间 [cLo, cHi) 上
    auto spreadToColumns = [this](const std::vector<double>& pts, int cLo, int cHi)
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
                m_colSum[c] += pts[j];
                m_colCnt[c] += 1;
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

            // 该天数据的末端边界列即分隔线位置（== 下一天的 cLo，相邻天共用同一取整边界）；
            // 顶部取当日最后一根数据的收盘价
            m_daySeparators.emplace_back(cHi, day.back());
        }
    }
    else
    {
        // slotsPerDay 未知（<=0）：退化为按实际点数铺满整个宽度（保留旧行为）
        const double totalN = static_cast<double>(series.totalPoints());
        std::vector<double> flat;
        flat.reserve(series.totalPoints());
        for (const auto& d : series.days)
        {
            flat.insert(flat.end(), d.begin(), d.end());
            if (!d.empty())
            {
                m_daySeparators.emplace_back(static_cast<int>(std::lround(flat.size() / totalN * width)), d.back());
            }
        }
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

    // 价格 -> y 坐标（面积列与日分隔竖线共用同一套量程和钳制，保证高度一致）
    auto priceToY = [&](double value) -> int
    {
        if (range <= 0.0)
            return rect.top + height / 2;
        double ratio = (value - minPrice) / range;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        int y = bottom - static_cast<int>(std::lround(ratio * usable));
        if (y < rect.top) y = rect.top;
        if (y > bottom) y = bottom;
        return y;
    };

    // 逐列绘制底部对齐的竖线，形成面积填充效果
    COLORREF graphColor = dark_mode ? g_data.HistoryGraphColorDark() : g_data.HistoryGraphColorLight();


    CPen pen(PS_SOLID, 1, graphColor);
    CPen* pOldPen = pDC->SelectObject(&pen);

    // 先收集每条竖线的端点（每列一条独立 2 点折线），再一次性提交给 PolyPolyline
    std::vector<POINT> segPoints;
    std::vector<DWORD> segCounts;
    segPoints.reserve(static_cast<size_t>(width) * 2);
    segCounts.reserve(static_cast<size_t>(width));

    for (int c = 0; c < width; ++c)
    {
        if (m_colCnt[c] <= 0)
            continue;   // 未走到的时段（右侧留白）不画

        int x = rect.left + c;
        if (x < rect.left)
            continue;
        if (x > rect.right - 1)
            break;

        const int y = priceToY(m_colSum[c] / static_cast<double>(m_colCnt[c]));

        segPoints.push_back(POINT{ x, bottom });   // 先 bottom，对应原 MoveTo
        segPoints.push_back(POINT{ x, y });        // 再 y，对应原 LineTo
        segCounts.push_back(2);
    }

    if (!segCounts.empty())
        pDC->PolyPolyline(segPoints.data(), segCounts.data(), static_cast<int>(segCounts.size()));

    pDC->SelectObject(pOldPen);

    // 分隔线颜色：深色 / 浅色各取一个配置值
    const COLORREF sepColor = dark_mode ? g_data.HistoryGraphSepLineColorDark() : g_data.HistoryGraphSepLineColorLight();
    const bool showSepLine = g_data.HistoryGraphShowSepLine();

    // 日分隔竖线：只在「已经走完的交易日」末列画，从底部画到当日末价高度，用来切分各交易日。
    // m_daySeparators 的最后一个元素是最后一天——它还没走完，没有「当日末价」可言，
    // 所以不参与本循环，改由下面的「当前价线」单独绘制。
    if (showSepLine && m_daySeparators.size() > 1)
    {
        CPen sepPen(PS_SOLID, 1, sepColor);
        CPen* pOldSepPen = pDC->SelectObject(&sepPen);
        for (size_t si = 0; si + 1 < m_daySeparators.size(); ++si)
        {
            int x = rect.left + m_daySeparators[si].first;
            if (x < rect.left)
                x = rect.left;
            if (x > rect.right - 1)
                x = rect.right - 1;
            pDC->MoveTo(x, bottom);
            pDC->LineTo(x, priceToY(m_daySeparators[si].second));
        }
        pDC->SelectObject(pOldSepPen);
    }

    // 当前价线：最后一天数据的末端列，从底部画到最新价，用来标出当前价位置
    if (showSepLine && !m_daySeparators.empty())
    {
        CPen curPen(PS_SOLID, 1, sepColor);
        CPen* pOldCurPen = pDC->SelectObject(&curPen);
        int x = rect.left + m_daySeparators.back().first;
        if (x < rect.left)
            x = rect.left;
        if (x > rect.right - 1)
            x = rect.right - 1;
        pDC->MoveTo(x, bottom);
        pDC->LineTo(x, priceToY(m_daySeparators.back().second));
        pDC->SelectObject(pOldCurPen);
    }

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



LStockItem::ItemDibCache::~ItemDibCache()
{
    if (dc != nullptr)
    {
        if (oldBitmap != nullptr)
        {
            ::SelectObject(dc, oldBitmap);
            oldBitmap = nullptr;
        }
        if (bitmap != nullptr)
        {
            ::DeleteObject(bitmap);
            bitmap = nullptr;
        }
        ::DeleteDC(dc);
        dc = nullptr;
    }
    valid = false;
}

// 把参数化的条目内容（走势图背景 + 四段文字）绘制到给定 DC 的 rect 区域内。
// 抽出成独立函数，使直接绘制与离屏 DIB 渲染共用同一套绘制代码，保证像素一致。
void LStockItem::RenderItem(CDC *pDC, const CRect &rect, bool dark_mode, const ItemDrawParams &params)
{
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

    const UINT flags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    int cursor = rect.left;

    // 1. 名称（右对齐到「最长名称」列）
    if (params.showName)
    {
        CRect rc{ rect };
        rc.left = cursor;
        rc.right = cursor + params.nameColWidth;
        DrawTextWithTransparency(pDC, params.nameText, rc, params.colorDefault, flags | DT_RIGHT);
        cursor += params.nameColWidth;
    }

    // 2. 数值（价格 + 涨跌幅）
    CRect rcValue{ rect };
    rcValue.left = cursor;
    rcValue.right = cursor + params.valueWidth;
    DrawTextWithTransparency(pDC, params.valueText, rcValue, params.valueColor, flags);
    cursor += params.valueWidth;

    // 3. 成本价标签（中性色）
    if (!params.costLabel.IsEmpty())
    {
        CRect rc{ rect };
        rc.left = cursor;
        rc.right = cursor + params.labelWidth;
        DrawTextWithTransparency(pDC, params.costLabel, rc, params.colorDefault, flags);
        cursor += params.labelWidth;
    }

    // 4. 差额/涨跌幅：盈利红、亏损绿、持平用默认色
    if (!params.costDir.IsEmpty())
    {
        CRect rc{ rect };
        rc.left = cursor;
        rc.right = cursor + params.dirWidth;
        DrawTextWithTransparency(pDC, params.costDir, rc, params.dirColor, flags);
    }
}

void LStockItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    auto data = g_data.GetStockByIndex(index);
    if (data == nullptr) {
        return;
    }

    CDC* pDC = CDC::FromHandle((HDC)hDC);
    CRect rect(CPoint(x, y), CSize(w, h));

    // ---- 1. 先算好本帧绘制参数（签名比对与渲染共用）----
    ItemDrawParams params;
    if (dark_mode)
    {
        params.colorDefault = RGB(255, 255, 255);
        params.valueColor = params.colorDefault;
        params.dirColor = params.colorDefault;
    }
    else
    {
        params.colorDefault = RGB(0, 0, 0);
        params.valueColor = params.colorDefault;
        params.dirColor = params.colorDefault;
    }

    params.showName = g_data.IsDisplayName();
    if (params.showName)
    {
        wxString displayName = (g_data.IsDisplayAlias() && !data->GetAlias().empty()) ? data->GetAlias() : data->GetName();
        params.nameText = (displayName + ": ").wc_str();
    }
    params.valueText = GetDisplayContent(data, false);
    params.costLabel = GetCostLabelContent(data);
    params.costDir = GetCostDirContent(data);

    params.nameColWidth = params.showName ? GetMaxStockNameWidth(pDC) : 0;
    params.valueWidth = MeasureTextWidth(pDC, params.valueText);
    params.labelWidth = params.costLabel.IsEmpty() ? 0 : MeasureTextWidth(pDC, params.costLabel);
    params.dirWidth = params.costDir.IsEmpty() ? 0 : MeasureTextWidth(pDC, params.costDir);

    const COLORREF color_red = dark_mode ? RGB(255, 121, 120) : RGB(195, 0, 0);
    const COLORREF color_green = dark_mode ? RGB(111, 215, 149) : RGB(46, 139, 87);
    if (g_data.IsDisplayColor())
    {
        const bool isUp = data->GetChangeFluctuation().find('+') != wxString::npos;
        params.valueColor = isUp ? color_red : color_green;
    }
    if (!params.costDir.IsEmpty() && g_data.IsDisplayColor())
    {
        const int dir = data->GetCostProfitDirection();
        if (dir > 0)
            params.dirColor = color_red;
        else if (dir < 0)
            params.dirColor = color_green;
    }

    // ---- 2. 内容签名：任何影响像素的输入都参与，未变则直接贴回上帧位图 ----
    unsigned long long sig = 1469598103934665603ULL;
    Fnv1aMixWord(sig, (unsigned long long)w);
    Fnv1aMixWord(sig, (unsigned long long)h);
    Fnv1aMixWord(sig, dark_mode ? 1ULL : 0ULL);
    Fnv1aMixWord(sig, (unsigned long long)params.colorDefault);
    Fnv1aMixWord(sig, (unsigned long long)params.valueColor);
    Fnv1aMixWord(sig, (unsigned long long)params.dirColor);
    Fnv1aMixWord(sig, (unsigned long long)params.nameColWidth);
    Fnv1aMixWord(sig, (unsigned long long)params.valueWidth);
    Fnv1aMixWord(sig, (unsigned long long)params.labelWidth);
    Fnv1aMixWord(sig, (unsigned long long)params.dirWidth);
    Fnv1aMixString(sig, wxString(params.nameText.GetString()));
    Fnv1aMixString(sig, wxString(params.valueText.GetString()));
    Fnv1aMixString(sig, wxString(params.costLabel.GetString()));
    Fnv1aMixString(sig, wxString(params.costDir.GetString()));
    // 走势图数据版本、开关与外观配置：任一变化都必须重绘背景
    Fnv1aMixWord(sig, g_data.IsDrawHistoryGraph() ? 1ULL : 0ULL);
    if (g_data.IsDrawHistoryGraph())
    {
        Fnv1aMixWord(sig, g_data.HistoryRev());
        Fnv1aMixWord(sig, (unsigned long long)g_data.HistoryGraphMode());
        Fnv1aMixWord(sig, (unsigned long long)g_data.HistoryGraphStyle());
        Fnv1aMixWord(sig, (unsigned long long)g_data.HistoryGraphColorDark());
        Fnv1aMixWord(sig, (unsigned long long)g_data.HistoryGraphColorLight());
        Fnv1aMixWord(sig, g_data.HistoryGraphDashedBox() ? 1ULL : 0ULL);
        Fnv1aMixWord(sig, g_data.HistoryGraphShowSepLine() ? 1ULL : 0ULL);
        Fnv1aMixWord(sig, (unsigned long long)g_data.HistoryGraphSepLineColorDark());
        Fnv1aMixWord(sig, (unsigned long long)g_data.HistoryGraphSepLineColorLight());
        Fnv1aMixWord(sig, (unsigned long long)g_data.TextTransparency());
    }

    // ---- 3. 缓存开关与尺寸护栏 ----
    const bool cacheUsable = g_data.IsItemDibCache() && w > 0 && h > 0 && w <= 2048 && h <= 256;
    if (!cacheUsable)
    {
        if (m_dib) { m_dib.reset(); }
        RenderItem(pDC, rect, dark_mode, params);
        return;
    }

    if (!m_dib) { m_dib = std::make_shared<ItemDibCache>(); }
    ItemDibCache &dib = *m_dib;

    // 命中：内容未变且未超过复用时限，直接把上帧位图贴回
    const unsigned long long now = ::GetTickCount64();
    if (dib.valid && dib.sig == sig && dib.w == w && dib.h == h &&
        (now - dib.tick) <= item_dib_cache_ttl_ms)
    {
        ::BitBlt((HDC)hDC, x, y, w, h, dib.dc, 0, 0, SRCCOPY);
        return;
    }

    // 未命中：按需重建离屏位图（尺寸变化时重建）
    if (dib.dc == nullptr || dib.w != w || dib.h != h)
    {
        if (dib.dc != nullptr)
        {
            ItemDibCache stale;
            stale.dc = dib.dc;
            stale.bitmap = dib.bitmap;
            stale.oldBitmap = dib.oldBitmap;
            // stale 析构时释放旧资源
            dib.dc = nullptr;
            dib.bitmap = nullptr;
            dib.oldBitmap = nullptr;
        }
        HDC memDC = ::CreateCompatibleDC((HDC)hDC);
        if (memDC == nullptr)
        {
            RenderItem(pDC, rect, dark_mode, params);
            return;
        }
        HBITMAP bitmap = ::CreateCompatibleBitmap((HDC)hDC, w, h);
        if (bitmap == nullptr)
        {
            ::DeleteDC(memDC);
            RenderItem(pDC, rect, dark_mode, params);
            return;
        }
        dib.dc = memDC;
        dib.bitmap = bitmap;
        dib.oldBitmap = ::SelectObject(memDC, bitmap);
        dib.w = w;
        dib.h = h;
        dib.valid = false;
    }

    // 先把宿主背景拷贝进离屏位图，再在离屏位图上绘制（保证半透明文字与背景合成正确）
    ::BitBlt(dib.dc, 0, 0, w, h, (HDC)hDC, x, y, SRCCOPY);

    const HFONT hFont = (HFONT)::GetCurrentObject((HDC)hDC, OBJ_FONT);
    HGDIOBJ oldFont = nullptr;
    if (hFont != nullptr) { oldFont = ::SelectObject(dib.dc, hFont); }

    CDC *pMemDC = CDC::FromHandle(dib.dc);
    RenderItem(pMemDC, CRect(CPoint(0, 0), CSize(w, h)), dark_mode, params);

    if (oldFont != nullptr) { ::SelectObject(dib.dc, oldFont); }

    // 回贴到目标 DC
    ::BitBlt((HDC)hDC, x, y, w, h, dib.dc, 0, 0, SRCCOPY);

    dib.sig = sig;
    dib.tick = now;
    dib.valid = true;
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
