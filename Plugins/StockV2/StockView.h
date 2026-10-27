#pragma once
#include "pch.h"
#include <afxwin.h>
#include <wx/webview.h>
#include <string>
#include <wx/popupwin.h>

namespace LStockViewer
{
    // 股票视图面板
    //
    // 注意：这里不能使用 wxPopupTransientWindow 承载 WebView2（Edge）。
    // wxPopupTransientWindow 是「非激活态」的瞬态弹窗，而 Chromium/WebView2 需要正常的
    // 顶层窗口来完成 TSF（文本服务框架，ctfmon.exe）的文本输入注册；在瞬态弹窗中创建
    // WebView2 会触发 ctfmon.exe 的「快速异常检测失败」崩溃（生产 Release 偶发）。
    // 因此统一改用无边框、无任务栏、置顶的普通 wxFrame 来模拟弹窗行为，并在失焦时自动关闭。
    class LStockView : public wxFrame
    {
    public:
        LStockView(wxWindow* parent);
        ~LStockView() override;
        // 初始化接口
        BOOL Setup(wxPoint pt, const int stock_index);
        // 资源清理
        void Clean();

    public:
        void OnMouse(wxMouseEvent &WXUNUSED(event));
        // 失焦（点击弹窗外部）时自动关闭，模拟原 wxPopupTransientWindow 的 dismiss 行为
        void OnActivate(wxActivateEvent &event);

    private:
        wxRect CalculateWindowPosition(wxPoint pt, const int width, const int height);
        wxString BuildDataScript(UINT bridgePort);
        wxWebView *CreateWebViewComponent(wxWindow* parent, int wxWindowStyle);

    private:
        wxWebView* m_webView;
        wxSharedPtr<STOCK::LStockData> m_stock;
        bool m_is_cleaned;               // 防止重复清理
    private:
        wxDECLARE_ABSTRACT_CLASS(LStockView);
        wxDECLARE_EVENT_TABLE();
    };

}