#include "inputbox.h"
#include <string.h>

// The dialog is built in memory (DLGTEMPLATE), so no dialog resource is needed and the caller's strings can be
// used directly.

#define IDC_INPUT_PROMPT    1001
#define IDC_INPUT_EDIT      1002

typedef struct InputBoxState
{
    LPCWSTR wszPrompt;
    LPCWSTR wszTitle;
    LPCWSTR wszDefault;
    LPWSTR wszAnswer;
    DWORD cchAnswer;
    BOOL bPassword;
} InputBoxState;

static INT_PTR CALLBACK InputBoxDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            InputBoxState* state = (InputBoxState*)lParam;
            SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)state);
            SetWindowTextW(hDlg, state->wszTitle);
            SetDlgItemTextW(hDlg, IDC_INPUT_PROMPT, state->wszPrompt);
            SetDlgItemTextW(hDlg, IDC_INPUT_EDIT, state->wszDefault ? state->wszDefault : L"");
            if (state->bPassword)
            {
                SendDlgItemMessageW(hDlg, IDC_INPUT_EDIT, EM_SETPASSWORDCHAR, L'\x25CF', 0);
            }
            SendDlgItemMessageW(hDlg, IDC_INPUT_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hDlg, IDC_INPUT_EDIT));
            return FALSE; // focus was set manually
        }
        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case IDOK:
                {
                    InputBoxState* state = (InputBoxState*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
                    GetDlgItemTextW(hDlg, IDC_INPUT_EDIT, state->wszAnswer, state->cchAnswer);
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                {
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                }
            }
            break;
        }
    }
    return FALSE;
}

// Appends a NUL terminated string to the template buffer (which must stay WORD aligned).
static WORD* AppendString(WORD* p, const wchar_t* s)
{
    size_t len = wcslen(s) + 1;
    memcpy(p, s, len * sizeof(wchar_t));
    return p + len;
}

static WORD* AlignDword(WORD* p)
{
    return (WORD*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
}

static WORD* AppendItem(WORD* p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD atom, const wchar_t* text)
{
    p = AlignDword(p);
    DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
    item->style = style;
    item->dwExtendedStyle = 0;
    item->x = x;
    item->y = y;
    item->cx = cx;
    item->cy = cy;
    item->id = id;
    p = (WORD*)(item + 1);
    *p++ = 0xFFFF;   // class given as an atom
    *p++ = atom;
    p = AppendString(p, text);
    *p++ = 0;        // no creation data
    return p;
}

HRESULT InputBox(BOOL bPassword, HWND hWndParent, LPCWSTR wszPrompt, LPCWSTR wszTitle, LPCWSTR wszDefault,
                 LPWSTR wszAnswer, DWORD cchAnswer, BOOL* pbCancelled)
{
    if (!wszAnswer || !cchAnswer || !pbCancelled)
    {
        return E_INVALIDARG;
    }
    *pbCancelled = TRUE;
    wszAnswer[0] = 0;

    // Dialog units. The window is 260 x 78 DLU; the prompt wraps over two lines.
    const short W = 260, H = 78;
    WORD buffer[512];
    ZeroMemory(buffer, sizeof(buffer));
    WORD* p = buffer;

    DLGTEMPLATE* dlg = (DLGTEMPLATE*)p;
    dlg->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlg->dwExtendedStyle = 0;
    dlg->cdit = 4;
    dlg->x = 0;
    dlg->y = 0;
    dlg->cx = W;
    dlg->cy = H;
    p = (WORD*)(dlg + 1);
    *p++ = 0;                       // no menu
    *p++ = 0;                       // default dialog class
    p = AppendString(p, L"");       // title is set in WM_INITDIALOG
    *p++ = 9;                       // font size
    p = AppendString(p, L"Segoe UI");

    p = AppendItem(p, WS_CHILD | WS_VISIBLE | SS_LEFT, 7, 7, W - 14, 22, IDC_INPUT_PROMPT, 0x0082 /*static*/, L"");
    p = AppendItem(p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | (bPassword ? ES_PASSWORD : 0),
                   7, 32, W - 14, 14, IDC_INPUT_EDIT, 0x0081 /*edit*/, L"");
    p = AppendItem(p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, W - 7 - 50 - 4 - 50, H - 7 - 14, 50, 14, IDOK, 0x0080 /*button*/, L"OK");
    p = AppendItem(p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, W - 7 - 50, H - 7 - 14, 50, 14, IDCANCEL, 0x0080 /*button*/, L"Cancel");

    InputBoxState state;
    state.wszPrompt = wszPrompt ? wszPrompt : L"";
    state.wszTitle = wszTitle ? wszTitle : L"";
    state.wszDefault = wszDefault;
    state.wszAnswer = wszAnswer;
    state.cchAnswer = cchAnswer;
    state.bPassword = bPassword;

    INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(NULL), dlg, hWndParent, InputBoxDlgProc, (LPARAM)&state);
    if (result == -1)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    *pbCancelled = (result != IDOK);
    return S_OK;
}
