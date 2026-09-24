#pragma once
//
// explorer.h - controlling the shell process (explorer.exe) and the engine inside it.
//
// Both halves of the product use this. The settings window restarts the shell from here, and the core DLL's
// rundll32 entry points (ZZRestartExplorer, ZZStartup) do the parts that cannot run inside the shell itself.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// How the engine got into the shell, as reported by FindEngineInShell.
#define SP_ENGINE_MISSING   0   // the shell has no engine in it
#define SP_ENGINE_INJECTED  1   // ShadePatcher.dll was loaded from the build or install folder (sp_inject)
#define SP_ENGINE_PROXY     2   // the installer's copy in the Windows folder, which every shell picks up

// The process id of this session's shell, or 0 when no shell is running.
DWORD FindShellProcessId(void);

// TRUE when the engine's crash guard has put it into safe mode for this session (no mods loaded).
BOOL IsEngineInSafeMode(void);

// TRUE while a restart started by RestartExplorer or RestartExplorerNow, in any process, is still running.
BOOL IsExplorerRestartInProgress(void);

// Looks inside the shell for the engine. Returns one of the SP_ENGINE_ values; for SP_ENGINE_INJECTED the path
// of the loaded DLL is copied to `path` (which may be NULL) so the same build can be put back after a restart.
int FindEngineInShell(wchar_t* path, DWORD cch);

// Loads a DLL into the running shell, the way sp_inject does. Returns TRUE when the shell has it afterwards.
BOOL InjectIntoShell(const wchar_t* dllPath);

// TRUE when this process is explorer.exe.
BOOL IsThisProcessTheShell(void);

// Restarts the shell and puts the engine back the way it was. Safe to call from anywhere, including from inside
// explorer.exe: in that case the work is handed to a helper process (rundll32 running ZZRestartExplorer), since
// a thread cannot outlive the process it is asking to exit, and the settings window is reopened afterwards.
// From any other process the restart runs on a background thread and this returns at once; a second call
// while one is under way does nothing.
void RestartExplorer(void);

// The restart itself, for a process that is not the shell. Asks the shell to exit, terminates what is left,
// starts a fresh explorer.exe with `reinjectDll` (when not NULL) loaded before its first instruction, so the
// engine is in place before the taskbar and the desktop are built, exactly like the installed proxy.
void RestartExplorerNow(const wchar_t* reinjectDll);

// Starts explorer.exe if no shell is running.
void StartExplorer(void);

// Waits until the shell's taskbar exists. Returns FALSE on timeout.
BOOL WaitForShell(DWORD timeoutMs);

#ifdef __cplusplus
}
#endif
