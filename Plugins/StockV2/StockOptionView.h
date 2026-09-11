#pragma once

#include "pch.h"
#include <wx/clrpicker.h>
#include <wx/spinctrl.h>
#include <wx/srchctrl.h>
#include <wx/dataview.h>
#include "StockDef.h"

class LStockOptionView : public wxFrame
{
public:
    LStockOptionView(const wxString &title, const wxPoint &pos, const wxSize &size, const int& close_evt_id);
    ~LStockOptionView();

    wxDECLARE_EVENT_TABLE();

private:
    void InitUI();
    void LoadOptions();
    void BindAllEvents();
    void UpdateCostDisplayEnable();
    void OnAddStock(wxCommandEvent &);
    void OnDeleteSel(wxCommandEvent &);
    void OnBatchClear(wxCommandEvent &);
    void OnMoveUp(wxCommandEvent&);
    void OnMoveDown(wxCommandEvent&);
    void OnTopSel(wxCommandEvent&);
    void OnRestoreDefault(wxCommandEvent &);
    void OnSaveConfig(wxCommandEvent &);
    void OnCancel(wxCommandEvent&);
    void OnListRightMenu(wxDataViewEvent& event);
    void OnMenuSelect(wxCommandEvent &e);
    void OnClose(wxCloseEvent& event);
    void OnStockListItemActivated(wxDataViewEvent& event);
    void OnStockListItemValueChanged(wxDataViewEvent& event);
    void OnStockListItemEditingStarted(wxDataViewEvent& event);

private:
    wxVector<wxSharedPtr<STOCK::LStockData>> m_stock_datas;

private:
    wxArrayString COMB_FREQ_OPTS = { "3", "5", "10", "30", "60", "300" };

    int m_close_evt_id;

    wxButton *m_btnAdd;
    wxButton *m_btnRemove;
    wxButton *m_btnClean;
    wxButton *m_btnMoveUP;
    wxButton *m_btnMoveDown;
    wxButton *m_btnMoveTop;

    wxButton *m_btnRestoreDefault;

    wxDataViewCtrl* m_stockListCtrl;
    wxObjectDataPtr<STOCK::LStockListVM> m_solVM;

    wxComboBox *m_cmbRefreshFreq;

    wxSpinCtrl* m_KLineViewWidthSpinCtrl;
    wxSpinCtrl* m_KLineViewHeightSpinCtrl;

    wxCheckBox* m_colorCheck;
    // 优先显示变动价格
    wxCheckBox* m_priorityDisplayChangedCheck;
    wxCheckBox* m_isDisplayStockNameCheck;
    // 成本价显示相关
    wxCheckBox* m_isDisplayCostCheck;
    wxCheckBox* m_isDisplayCostProfitPriceCheck;
    wxCheckBox* m_isDisplayCostProfitPercentCheck;
    wxCheckBox* m_isDisplayAliasCheck;
    // 滚动显示相关
    wxCheckBox* m_isScrollEnableCheck;
    wxSpinCtrl* m_scrollPageSizeSpin;
    wxSpinCtrl* m_scrollIntervalSpin;
    // 打开对话框时的滚动配置（用于判断是否需要提示重启）
    bool m_initialScrollEnable;
    int m_initialScrollPageSize;
    int m_initialScrollInterval;
    //wxCheckBox *m_autoUpdateCheck;
    //wxCheckBox *m_alertCheck;

    wxColourPickerCtrl *m_colourRise;
    wxColourPickerCtrl *m_colourFall;

    wxButton *m_btnOk;
    wxButton *m_btnCancel;
};
