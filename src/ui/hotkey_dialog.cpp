// The Add / Edit Hotkey dialog: an action, the key combination that runs it, and
// whether it works everywhere or only in FastPlay's main window.

#include "ui/hotkey_dialog.h"
#include "ui/ui_common.h"

#include "globals.h"
#include "hotkeys.h"
#include "accessibility.h"
#include "keycodes.h"
#if defined(__WXOSX__) || defined(__WXGTK__)
#include "system_keys.h"
#endif

namespace {

#ifdef __WXOSX__
const unsigned kAllowedModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
const char* const kModifierRequired =
    "Global hotkeys require at least one modifier key (Command, Option, Control, or Shift).";
#else
const unsigned kAllowedModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
const char* const kModifierRequired = "Global hotkeys require at least one modifier key (Ctrl, Alt, or Shift).";
#endif

class HotkeyDialog : public wxDialog {
public:
    HotkeyDialog(wxWindow* parent, const HotkeyDlgData& data)
        : wxDialog(parent, wxID_ANY, data.isEdit ? "Edit Hotkey" : "Add Hotkey") {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1);

        // Action combo box
        grid->Add(new wxStaticText(this, wxID_ANY, "&Action:"), 0, wxALIGN_CENTER_VERTICAL);
        m_action = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(280, -1));
        for (int i = 0; i < g_hotkeyActionCount; i++) {
            m_action->Append(g_hotkeyActions[i].name);
        }
        if (data.actionIdx >= 0 && data.actionIdx < g_hotkeyActionCount) {
            m_action->SetSelection(data.actionIdx);
        }
        grid->Add(m_action, 1, wxEXPAND);

        // Hotkey field: shows the combination, which is captured while it has focus
        grid->Add(new wxStaticText(this, wxID_ANY, "&Hotkey:"), 0, wxALIGN_CENTER_VERTICAL);
        m_key = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(280, -1), wxTE_READONLY);
        grid->Add(m_key, 1, wxEXPAND);
        sizer->Add(grid, 0, wxEXPAND | wxALL, 10);

        m_global = new wxCheckBox(this, wxID_ANY, "&Global (works while other programs are in use)");
        m_global->SetValue(data.global);
        sizer->Add(m_global, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* ok = new wxButton(this, wxID_OK, "OK");
        ok->SetDefault();
        buttons->Add(ok, 0, wxRIGHT, 6);
        buttons->Add(new wxButton(this, wxID_CANCEL, "Cancel"));
        sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        // Set hotkey field (only Ctrl, Alt and Shift can be entered; on macOS
        // Command, Option, Shift and Control)
        if (data.vk != 0) {
            m_vk = data.vk;
            m_modifiers = data.modifiers & kAllowedModifiers;
        }
        m_global->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_global->SetFocus(); });
        ShowKey();

        Bind(wxEVT_CHAR_HOOK, &HotkeyDialog::OnCharHook, this);
        Bind(wxEVT_BUTTON, &HotkeyDialog::OnOK, this, wxID_OK);

        SetSizerAndFit(sizer);
        CentreOnParent();
        m_action->SetFocus();
    }

    int GetActionIdx() const { return m_action->GetSelection(); }
    unsigned GetModifiers() const { return m_modifiers; }
    unsigned GetVk() const { return m_vk; }
    bool IsGlobal() const { return m_global->GetValue(); }

private:
    void ShowKey() {
        m_key->ChangeValue(m_vk != 0 ? WX(FormatHotkey(m_modifiers, m_vk)) : wxString("None"));
    }

    // Set (or with vk 0, clear) the hotkey, and say it: a screen reader does not
    // announce a read-only field changing.
    void SetKey(unsigned modifiers, unsigned vk) {
        m_modifiers = (vk != 0) ? modifiers : 0;
        m_vk = vk;
        ShowKey();
        SpeakW(WS(m_key->GetValue()));
    }

    void OnCharHook(wxKeyEvent& event) {
        if (FindFocus() != m_key) {
            event.Skip();
            return;
        }

        int key = event.GetKeyCode();
        switch (key) {
            // Dialog keys: Tab moves focus, Enter presses OK, Escape cancels
            case WXK_TAB:
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER:
            case WXK_ESCAPE:
            // A modifier on its own is not a hotkey
            case WXK_SHIFT:
            case WXK_CONTROL:
            case WXK_ALT:
            case WXK_WINDOWS_LEFT:
            case WXK_WINDOWS_RIGHT:
#ifdef __WXOSX__
            case WXK_RAW_CONTROL:  // Control; WXK_CONTROL is Command
#endif
                event.Skip();
                return;

            // These clear the hotkey
            case WXK_BACK:
            case WXK_DELETE:
            case WXK_NUMPAD_DELETE:
            case WXK_SPACE:
                SetKey(0, 0);
                return;

            default:
                break;
        }

#ifdef __WXMSW__
        // Global hotkeys are registered with Windows virtual key codes
        unsigned vk = static_cast<unsigned>(event.GetRawKeyCode()) & 0xFF;
        if (vk == 0) {
            event.Skip();
            return;
        }
        unsigned modifiers = 0;
        if (event.ControlDown()) modifiers |= MOD_CONTROL;
        if (event.AltDown()) modifiers |= MOD_ALT;
        if (event.ShiftDown()) modifiers |= MOD_SHIFT;
        SetKey(modifiers, vk);
#elif defined(__WXOSX__)
        // Stored as Windows virtual key codes on every system (keycodes.h)
        unsigned vk = MacKeyCodeToVirtualKey(static_cast<unsigned>(event.GetRawKeyCode()));
        if (vk == 0) {
            event.Skip();
            return;
        }
        unsigned modifiers = 0;
        if (event.ControlDown()) modifiers |= MOD_CONTROL;  // Command
        if (event.AltDown()) modifiers |= MOD_ALT;          // Option
        if (event.ShiftDown()) modifiers |= MOD_SHIFT;
        if (event.RawControlDown()) modifiers |= MOD_WIN;   // Control
        SetKey(modifiers, vk);
#elif defined(__WXGTK__)
        // Stored as Windows virtual key codes on every system (keycodes.h), by the
        // key's place (its hardware code), not the character it types
        unsigned vk = LinuxKeyCodeToVirtualKey(static_cast<unsigned>(event.GetRawKeyFlags()));
        if (vk == 0) {
            event.Skip();
            return;
        }
        unsigned modifiers = 0;
        if (event.ControlDown()) modifiers |= MOD_CONTROL;
        if (event.AltDown()) modifiers |= MOD_ALT;
        if (event.ShiftDown()) modifiers |= MOD_SHIFT;
        SetKey(modifiers, vk);
#else
        event.Skip();
#endif
    }

    void OnOK(wxCommandEvent&) {
        // Require at least one modifier for global hotkeys (a local one may be a
        // single key, as many of the main window's are)
        if (m_vk == 0) {
            wxMessageBox("Please enter a hotkey.", "Error", wxOK | wxICON_WARNING, this);
            return;
        }
        if (m_modifiers == 0 && m_global->GetValue()) {
            wxMessageBox(kModifierRequired, "Error", wxOK | wxICON_WARNING, this);
            return;
        }
        EndModal(wxID_OK);
    }

    wxChoice* m_action;
    wxTextCtrl* m_key;
    wxCheckBox* m_global;
    unsigned m_modifiers = 0;
    unsigned m_vk = 0;
};

}  // namespace

bool ShowHotkeyDialog(wxWindow* parent, HotkeyDlgData& data) {
    HotkeyDialog dlg(parent, data);
    if (dlg.ShowModal() != wxID_OK) return false;
    data.actionIdx = dlg.GetActionIdx();
    data.modifiers = dlg.GetModifiers();
    data.vk = dlg.GetVk();
    data.global = dlg.IsGlobal();
    return true;
}
