#pragma once
//
// filesearch.h - files and folders from the Windows Search index, for the search box.
//
// The index is queried with SQL through its OLE DB provider (Search.CollatorDSO), the way Explorer's own search
// does, on a thread of its own: the first query of a session takes about half a second, so the menu never waits
// for it. Typing restarts a short debounce; only the answer to the latest query is posted.
//
#include <Windows.h>

#include <string>
#include <vector>

#include "search.h"

namespace sm {

struct FileResults
{
    unsigned id;                  // the request it answers
    std::vector<Result> results;  // ResultKind::Open, target = the file or folder path
};

class FileSearch
{
public:
    bool Start(HWND notify, UINT message, WPARAM kind);
    void Stop();

    // Searches the user's files for `query`; an empty query cancels. Results arrive as a FileResults* with
    // `kind` as wParam, and only for the most recent id.
    void Request(unsigned id, const std::wstring& query, size_t limit);

private:
    static DWORD WINAPI ThreadProc(void* param);
    void Run();

    HANDLE m_thread = nullptr;
    HANDLE m_wake = nullptr;
    HWND m_notify = nullptr;
    UINT m_message = 0;
    WPARAM m_kind = 0;
    volatile LONG m_stop = 0;

    SRWLOCK m_lock = SRWLOCK_INIT;
    unsigned m_id = 0;
    std::wstring m_query;
    size_t m_limit = 6;
};

} // namespace sm
