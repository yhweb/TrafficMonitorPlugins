#include "pch.h"
#include "StockView.h"
#include <windows.h>
#include <wx/frame.h>
#include <sstream>
#include "StockV2.h"
#include "StockSockets.h"
#include <wx/display.h>
#include <wx/settings.h>
#include <wx/sstream.h>

namespace LStockViewer
{
    //----------------------------------------------------------------------------
    // LStockView
    //----------------------------------------------------------------------------

    wxIMPLEMENT_CLASS(LStockView, wxFrame);

    wxBEGIN_EVENT_TABLE(LStockView, wxFrame)
        EVT_MOUSE_EVENTS(LStockView::OnMouse)
        EVT_ACTIVATE(LStockView::OnActivate)
    wxEND_EVENT_TABLE()

    void LStockView::OnMouse(wxMouseEvent& WXUNUSED(event))
    {
        LLOG_INFO("OnMouse");
    }

    LStockView::LStockView(wxWindow* parent)
        : wxFrame(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                  wxBORDER_NONE | wxFRAME_TOOL_WINDOW | wxSTAY_ON_TOP | wxFRAME_NO_TASKBAR),
          m_webView(nullptr), m_is_cleaned(false)
    {
    }

    LStockView::~LStockView()
    {
        Clean();
    }

    void LStockView::OnActivate(wxActivateEvent& event)
    {
        // 失焦（用户点击了弹窗外部）时自动关闭，模拟原 wxPopupTransientWindow 的 dismiss 行为
        if (!event.GetActive())
        {
            LLOG_DEBUG("%p LStockView::OnActivate deactivate -> dismiss", this);
            Clean();
            Destroy();
        }
        event.Skip();
    }

    void LStockView::Clean()
    {
        LLOG_DEBUG("Clean");

        if (m_is_cleaned)
            return;
        m_is_cleaned = TRUE;

        LStockServerSocket& sss = LStockServerSocket::GetInstance();
        sss.StopSocketServer();
        sss.GetBridge()->UnregisterCallFunc("request_kline_data");

        if (m_webView)
        {
            m_webView->Destroy();
            m_webView = nullptr;
        }
        //Destroy();
    }

    // 计算窗口最终位置（屏幕边界适配）
    // 注意：pt 是屏幕坐标（wxGetMousePosition() 的返回值），不要再做 ClientToScreen 转换。
    wxRect LStockView::CalculateWindowPosition(wxPoint pt, const int width, const int height)
    {
        const wxPoint ptScreen = pt;

        // 使用「工作区」而非整个屏幕作为边界：工作区不含任务栏，
        // 从而保证弹窗底部不会覆盖任务栏。
        wxRect workArea;
        const int dpy = wxDisplay::GetFromPoint(ptScreen);
        if (dpy != wxNOT_FOUND)
        {
            // 显式转 unsigned int，消除 wxDisplay 构造函数重载歧义（索引 vs 视频模式 vs 窗口指针）
            workArea = wxDisplay(static_cast<unsigned int>(dpy)).GetClientArea();
        }
        else if (wxDisplay::GetCount() > 0)
        {
            // 找不到所属显示器时，用主显示器工作区兜底（默认构造即主显示器，无歧义）
            workArea = wxDisplay().GetClientArea();
        }
        else
        {
            workArea = wxRect(0, 0, 800, 600);
        }

        // 点击点位于任务栏（屏幕下方），弹窗默认显示在鼠标正上方
        int x = ptScreen.x;
        int y = ptScreen.y - height;

        // 若鼠标位于任务栏内（工作区下方），弹窗底部贴齐工作区底部（即任务栏顶部）
        if (ptScreen.y >= workArea.GetBottom())
            y = workArea.GetBottom() - height;

        // 上方空间不足时，改为显示在鼠标下方
        if (y < workArea.GetTop())
            y = ptScreen.y;

        // 水平方向：向右越界则向左对齐
        if (x + width > workArea.GetRight())
            x = workArea.GetRight() - width;

        // 最终夹紧到工作区范围内，保证不超出工作区下边界（任务栏顶部）/右边界
        if (x < workArea.GetLeft())
            x = workArea.GetLeft();
        if (y < workArea.GetTop())
            y = workArea.GetTop();
        if (y + height > workArea.GetBottom())
            y = workArea.GetBottom() - height;

        return wxRect(x, y, width, height);
    }

    // 创建WebView组件
    wxWebView *LStockView::CreateWebViewComponent(wxWindow* parent, int wxWindowStyle)
    {
        wxWebView *webView = nullptr;

        const bool edgeAvailable = wxWebView::IsBackendAvailable(wxWebViewBackendEdge);
        LLOG_ERROR("[StockView] WebView Edge backend available: %d", edgeAvailable);

        if (edgeAvailable)
        {
            webView = wxWebView::New(wxWebViewBackendEdge);
            if (webView)
            {
                if (!webView->Create(parent, wxID_ANY, wxWebViewDefaultURLStr, wxDefaultPosition, wxDefaultSize))
                {
                    LLOG_ERROR("[StockView] WebView Create() failed");
                    delete webView;
                    webView = nullptr;
                }
            }
            else
            {
                LLOG_ERROR("[StockView] wxWebView::New(Edge) returned nullptr");
            }

            //m_webView = wxWebView::New(parent,
            //                         wxID_ANY,
            //                         wxASCII_STR(wxWebViewDefaultURLStr),
            //                         wxDefaultPosition,
            //                         wxDefaultSize,
            //                         wxWebViewBackendEdge,
            //                         wxWindowStyle,
            //                         wxASCII_STR(wxWebViewNameStr));
        }

        if (!webView)
        {
            return nullptr;
        }

        return webView;
    }

    BOOL LStockView::Setup(wxPoint pt, const int stock_index)
    {
        m_stock = g_data.GetStockByIndex(stock_index);
        m_is_cleaned = false;

        int wxWindowStyle = wxNO_BORDER | wxTRANSPARENT_WINDOW;

        const int width = FromDIP(g_data.KLineW());  // g_data.RDPI(g_data.m_setting_data.m_kline_width);
        const int height = FromDIP(g_data.KLineH()); // g_data.RDPI(g_data.m_setting_data.m_kline_height);

        wxRect windowRect = CalculateWindowPosition(pt, width, height);

        wxLogDebug("StockView: %p Shown pos(%d, %d) size(%d, %d)", this, windowRect.GetLeft(), windowRect.GetBottom(), windowRect.width, windowRect.height);

        //wxScrolledWindow* m_panel = new wxScrolledWindow(this, wxID_ANY);
        wxPanel* m_panel = new wxPanel(this, wxID_ANY);

        // Keep this code to verify if mouse events work, they're required if
        // you're making a control like a combobox where the items are highlighted
        // under the cursor, the m_panel is set focus in the Popup() function
        m_panel->Bind(wxEVT_MOTION, &LStockView::OnMouse, this);

        m_panel->SetWindowStyle(wxWindowStyle);
        m_panel->SetBackgroundColour(*wxLIGHT_GREY);

        m_panel->SetSize(0, 0, windowRect.width, windowRect.height);

        // 创建WebView
        m_webView = CreateWebViewComponent(m_panel, wxWindowStyle);
        if (!m_webView)
        {
            LLOG_ERROR("Webview init failed!");
            Clean();
            return FALSE;
        }

        wxLogDebug("Backend: %s Version: %s", m_webView->GetClassInfo()->GetClassName(), wxWebView::GetBackendVersionInfo().ToString().wc_str());

        m_webView->SetSize(0, 0, windowRect.width, windowRect.height);

        wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);
        //topSizer->Add(m_webView, 0, wxALL, 0);
        //topSizer->Add(m_webView, wxSizerFlags().Expand().Proportion(1));
        //topSizer->Add(m_webView, wxALL);
        topSizer->Add(m_webView, 1, wxEXPAND | wxALL, 0);
        //topSizer->Add(text, 0, wxALL, 5);
        m_panel->SetSizer(topSizer);
        // Use the fitting size for the panel if we don't need scrollbars.
        //topSizer->Fit(m_panel);
        SetClientSize(m_panel->GetSize());

#ifdef DEBUG
        m_webView->EnableAccessToDevTools(TRUE);
        m_webView->EnableContextMenu(TRUE);
#endif

        // m_webView->LoadURL("https://www.baidu.com");

        // 加载HTML资源 + 注入数据
        wxString htmlContent = ResHtml(IDR_HTML_STOCK_SF);
        LLOG_DEBUG("html: %d", htmlContent.length());
        // LLOG_DEBUG(htmlContent);
        if (htmlContent.empty())
        {
            LLOG_ERROR("Load HTML resource failed!");
            Clean();
            return FALSE;
        }

        LStockServerSocket &sss = LStockServerSocket::GetInstance();
        sss.StartSocketServer();
        sss.GetBridge()->RegisterCallFunc("request_kline_data", LDataManager::OnRefreshStockTimelineData);
        wxLogDebug("StockServer StartUp: %d", sss.GetWebBridgePort());

        wxString dataScript = BuildDataScript(sss.GetWebBridgePort());
        wxLogDebug(dataScript);
        htmlContent.Replace("<!-- __DATA_INJECT__ -->", dataScript);
        m_webView->SetPage(htmlContent, "about:blank");

        // 用屏幕边界适配后的位置定位窗口（pt 已是屏幕坐标，不要再 ClientToScreen）
        this->Move(windowRect.GetLeft(), windowRect.GetTop());
        this->Show(true);

        // 窗口置顶
        this->Raise();
        this->SetFocus();
#ifdef __WXMSW__
        ::BringWindowToTop((HWND)this->GetHandle());
        ::SetForegroundWindow((HWND)this->GetHandle());
#endif

        wxLogDebug("StockView setup success, stock: %s", m_stock->code);
        return TRUE;
    }

    wxString LStockView::BuildDataScript(UINT bridgePort)
    {
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        wxScopedCharBuffer utf8Name = m_stock->GetName().ToUTF8();
        yyjson_mut_obj_add_strcpy(doc, root, "title", utf8Name.data());
        yyjson_mut_obj_add_strcpy(doc, root, "code", m_stock->GetCode().ToUTF8());

        // 弹窗分时图里的成本价黄虚线复用「显示成本」开关（0 = 不绘制）
        // 用 int 而非 bool 写值，与文件里既有的 yyjson_mut_obj_add_int 用法保持一致
        yyjson_mut_obj_add_int(doc, root, "displayCost", g_data.IsDisplayCost() ? 1 : 0);

        yyjson_mut_val* bridgeObj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, bridgeObj, "host", "127.0.0.1");
        yyjson_mut_obj_add_int(doc, bridgeObj, "port", bridgePort);
        //yyjson_mut_obj_add_strcpy(doc, bridgeObj, "token", "");
        yyjson_mut_obj_add_val(doc, root, "bridge", bridgeObj);

        size_t jsonLen = 0;
        char* jsonStr = yyjson_mut_write(doc, 0, &jsonLen);

        wxString result;
        result << R"(<script id="__WX_DATA__" type="application/json" crossorigin="anonymous">)";
        if (jsonStr && jsonLen > 0)
        {
            result += wxString::FromUTF8(jsonStr, jsonLen);
            free(jsonStr);
        }
        result << R"(</script>)";

        yyjson_mut_doc_free(doc);

        return result;
    }

}