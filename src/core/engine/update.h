#pragma once
//
// update.h - telling the user that a newer release exists.
//
// The check runs inside the shell, on a thread of its own, once per shell start. It asks GitHub for the latest
// release of the project, compares the version it reports with the one built into this DLL, and shows a
// notification when the release is newer. Clicking the notification opens the release page in the browser.
//
// Nothing is downloaded or installed. The user decides what to do with the information, which is why the check
// costs one small request and nothing else.
//
// Settings, under HKEY_CURRENT_USER\<REGPATH>:
//
//     UpdateCheck   dword   0 turns the check off entirely. Default 1.
//     UpdateURL     string  Where to ask. Default is the project's own releases endpoint.
//     UpdateSkip    string  A version the user has been told about already; it is not shown again.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the check on a background thread. Returns at once. Safe to call when the check is turned off, in which
// case it does nothing.
void SP_UpdateStart(void);

// Stops the check and takes the notification away. Called when the engine unloads.
void SP_UpdateStop(void);

#ifdef __cplusplus
}
#endif
