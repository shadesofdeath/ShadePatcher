#pragma once
//
// search.h - what the search box finds besides apps: Settings pages, a calculation, and anything the Run dialog
// would open (a program on the PATH, a path with %VARIABLES%, a shell: folder, a URL).
//
// Everything here is synchronous and cheap enough for every keystroke: no shell namespace enumeration, only
// string work, a registry read and a few file-system probes of local paths.
//
#include <Windows.h>

#include <string>
#include <vector>

namespace sm {

enum class ResultKind
{
    Setting,    // opens an ms-settings: page
    Calc,       // Enter copies the value
    Run,        // runs a program with arguments (Run-dialog style)
    Open,       // opens a path, folder, shell: location or URL
    Window,     // an open window: switches to it
    History,    // a past search: puts it back in the box
};

struct Result
{
    ResultKind kind;
    std::wstring title;       // shown in the row
    std::wstring subtitle;    // shown after the title, muted (path, hint)
    std::wstring target;      // uri, file, or the value to copy
    std::wstring arguments;   // Run only
    bool strong = false;      // Run/Open: the query is clearly a command or path; list it before the apps
    HWND window = nullptr;    // Window only
};

// Settings pages whose name or keywords contain the query, best first, at most `limit`.
std::vector<Result> SearchSettings(const std::wstring& query, size_t limit);

// The value of the query when it is an arithmetic expression (+ - * / ^ %, parentheses, "," or "." decimals,
// an optional leading "="). Returns false for anything that is not one, including a bare number.
bool Calculate(const std::wstring& query, double* value);

// The calculation as a Result: title "= value" in the user's number format.
Result CalcResult(double value);

// A Run-dialog interpretation of the query, when it has one.
bool ResolveCommand(const std::wstring& query, Result* result);

// Open top-level windows (those Alt+Tab shows) whose title contains the query, at most `limit`.
std::vector<Result> SearchWindows(const std::wstring& query, size_t limit);

// Carries out a Setting, Run, Open or Window result. `asAdmin` runs a program elevated.
bool ExecuteResult(const Result& result, bool asAdmin);

} // namespace sm
