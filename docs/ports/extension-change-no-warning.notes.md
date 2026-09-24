# extension-change-no-warning - integration notes

Source: `src/core/mods/extension_change_no_warning.cpp`
Based on the Windhawk mod `extension-change-no-warning` v1.0.1 by m417z.

## 1. SP_MOD_DECLARE symbol

`g_modExtensionChangeNoWarning`

Suggested placement in `mod_table.c`: the "File Explorer" group (next to `g_modExplorerAutoFileSizes`).
Order does not matter for this mod; it has no shared gestures.

## 2. ExceptionHandling

No. Plain Win32 only, no C++/WinRT, nothing throws. A plain `<ClCompile Include="mods\extension_change_no_warning.cpp" />`
line in `core.vcxproj` is enough.

Note for the build: the file contains `#ifndef _M_X64 #error` because the variadic forwarding relies on the x64
calling convention (see section 5). The engine is x64-only so this never fires; it is there to document the assumption.

## 3. Settings read

None besides the engine's `Enabled`. The original Windhawk mod has no settings either, and there is nothing worth adding:
the behaviour is a single yes/no.

## 4. Proposed settings.reg lines and strings

Only id 1430 is used (1431-1439 stay free).

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\extension-change-no-warning]
;b %R:1430%
"Enabled"=dword:00000000
```

Suggested place: the File Explorer section, right after the `explorer-auto-file-sizes` block. No `*` restart marker:
the hook is installed in place, so enabling/disabling takes effect on the next rename without restarting Explorer.

`strings.h`:

```
#define IDS_MOD_EXTNOWARN               1430
```

`lang/gui.en-US.rc`:

```
    IDS_MOD_EXTNOWARN               "Do not ask for confirmation when renaming changes a file's extension"
```

`lang/gui.tr-TR.rc`:

```
    IDS_MOD_EXTNOWARN               "Yeniden adlandırma dosya uzantısını değiştirdiğinde onay sorma"
```

## 5. What changed versus the original, and what was left out

Nothing was left out: the port covers the whole mod (both hooks, same recognition rule, same IDYES answer).

What is different, and why:

- **No `[[clang::musttail]]`.** The repo builds with the default MSVC toolset (`$(DefaultPlatformToolset)`, no clang),
  which has no musttail. `ShellMessageBoxW` / `ShellMessageBoxInternal` are printf-like variadic functions, so the
  detour has to hand the caller's format inserts to the original somehow. The port declares the detours and the
  trampoline types with the fixed parameters plus **16 trailing `ULONG_PTR` slots** and forwards all of them. On x64
  every argument after the fourth is on the stack in order, so this reproduces the caller's stack layout and the
  original's `va_list` sees the same values. The extra slots beyond what the caller pushed read the caller's own
  frame (readable memory) and are never consumed by the original. This is the standard MSVC way of detouring
  printf-like functions; 16 slots is far beyond any shell32 format's insert count. x64 only, guarded by `#error`.
- **Hooks are taken from shlwapi.dll by `GetProcAddress`, inside one `SP_HookBegin`/`SP_HookCommit` transaction.**
  `ShellMessageBoxW` is required (Init fails without it); `ShellMessageBoxInternal` is optional and only queued when
  the export exists, so the transaction is never handed a name that cannot be resolved. shell32's own
  `ShellMessageBoxW` export is a forwarder to shlwapi's (verified on this machine: identical address), so hooking
  shlwapi catches every caller.
- Verified on this machine (shell32 10.0.26100.9278, shlwapi 10.0.26100.8875): shell32 string 4112 is the
  "If you change a file name extension, the file might become unusable..." text (Turkish here), 4148 is "Rename"
  ("Ad Değiştir"), and shlwapi exports both `ShellMessageBoxW` and `ShellMessageBoxInternal`.
- Logging: one `SP_Log` line per suppressed prompt (info level), nothing per ordinary call.
- No BeforeUninit/Uninit: nothing is changed in live windows; the engine removes the hooks.

## 6. How to verify on a live shell

1. Enable the mod in the settings window (or set `Enabled=1` under
   `HKCU\Software\ShadePatcher\Mods\extension-change-no-warning`).
2. On the desktop or in a File Explorer window, create a new text file, press F2 and rename `New Text Document.txt`
   to `test.md` (or any other extension), Enter.
   - Without the mod: a "Rename" dialog with an exclamation icon asks "If you change a file name extension, the file
     might become unusable. Are you sure you want to change it?" with Yes/No.
   - With the mod: the file is renamed immediately, no dialog.
3. Rename the file back to `.txt`: same, no dialog. Works the same for a folder-view rename, a desktop rename and a
   rename from the Properties dialog (they all go through the same prompt).
4. Check that other shell prompts still appear, so the forwarding is intact: rename a file to a name containing `:`
   or `?` - the "A file name can't contain any of the following characters" message must still show, formatted
   correctly (that message goes through the same `ShellMessageBoxW` with inserts). Renaming to a name that already
   exists must still show its message too.
5. Disable the mod: the next extension change asks again, without restarting Explorer.
6. With `Logging=1` under `HKCU\Software\ShadePatcher`, each suppressed prompt logs
   `Answered Yes to the extension change prompt` (or `... (internal entry point)`) tagged `extension-change-no-warning`.
