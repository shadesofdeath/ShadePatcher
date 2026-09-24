#pragma once
//
// inputbox.h - a minimal modal text prompt (used by the ";w" input lines of settings.reg).
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Shows a dialog with a prompt, a single-line edit control and OK / Cancel. On OK the text is copied to wszAnswer
// (cchAnswer characters) and *pbCancelled is FALSE; on Cancel *pbCancelled is TRUE. Returns S_OK when the dialog
// could be shown.
HRESULT InputBox(BOOL bPassword, HWND hWndParent, LPCWSTR wszPrompt, LPCWSTR wszTitle, LPCWSTR wszDefault,
                 LPWSTR wszAnswer, DWORD cchAnswer, BOOL* pbCancelled);

#ifdef __cplusplus
}
#endif
