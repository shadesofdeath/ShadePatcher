//
// filesearch.cpp - see filesearch.h.
//
#include "filesearch.h"

#include <oledb.h>
#include <msdasc.h>
#include <wrl/client.h>

#include "engine/log.h"
#include "text.h"

#pragma comment(lib, "ole32.lib")

namespace sm {

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kTag[] = "custom-start-menu";

// Declared in msdasc.h / oledb.h but only defined in libraries this project does not link.
const CLSID kClsidDataInitialize = { 0x2206CDB0, 0x19C1, 0x11D1, { 0x89, 0xE0, 0x00, 0xC0, 0x4F, 0xD7, 0xA8, 0x29 } };
const GUID kDbGuidDefault = { 0xC8B521FB, 0x5CF3, 0x11CE, { 0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D } };

// One row as the accessor fills it.
struct Row
{
    DBSTATUS urlStatus;
    DBLENGTH urlLength;
    wchar_t url[1024];
    DBSTATUS nameStatus;
    DBLENGTH nameLength;
    wchar_t name[260];
    DBSTATUS folderStatus;
    DBLENGTH folderLength;
    wchar_t folder[1024];
};

// A literal for the index's SQL: quotes doubled, LIKE wildcards made literal.
std::wstring SqlLiteral(const std::wstring& text)
{
    std::wstring out;
    for (wchar_t c : text)
    {
        switch (c)
        {
        case L'\'': out += L"''"; break;
        case L'%':  out += L"[%]"; break;
        case L'_':  out += L"[_]"; break;
        case L'[':  out += L"[[]"; break;
        default:
            if (c >= 0x20)
            {
                out += c;
            }
            break;
        }
    }
    return out;
}

// "file:C:/Users/x/a.txt" -> "C:\Users\x\a.txt"
std::wstring PathFromUrl(const wchar_t* url)
{
    std::wstring path = url;
    if (_wcsnicmp(path.c_str(), L"file:", 5) == 0)
    {
        path = path.substr(5);
    }
    for (wchar_t& c : path)
    {
        if (c == L'/')
        {
            c = L'\\';
        }
    }
    return path;
}

class Index
{
public:
    // Opens the provider once per thread; the session is reused by every query after it.
    bool Open()
    {
        if (m_create)
        {
            return true;
        }
        ComPtr<IDataInitialize> init;
        ComPtr<IDBInitialize> source;
        ComPtr<IDBCreateSession> session;
        wchar_t connection[] = L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';";
        HRESULT hr;
        if (FAILED(hr = CoCreateInstance(kClsidDataInitialize, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&init))) ||
            FAILED(hr = init->GetDataSource(nullptr, CLSCTX_INPROC_SERVER, connection, __uuidof(IDBInitialize),
                                            reinterpret_cast<IUnknown**>(source.GetAddressOf()))) ||
            FAILED(hr = source->Initialize()) || FAILED(hr = source.As(&session)) ||
            FAILED(hr = session->CreateSession(nullptr, __uuidof(IDBCreateCommand),
                                               reinterpret_cast<IUnknown**>(m_create.GetAddressOf()))))
        {
            SP_LOG_ERR(kTag, L"The Windows Search index could not be opened (0x%08X)", hr);
            m_create.Reset();
            return false;
        }
        m_source = source;
        return true;
    }

    std::vector<Result> Query(const std::wstring& query, size_t limit)
    {
        std::vector<Result> results;
        if (!Open())
        {
            return results;
        }
        wchar_t profile[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        std::wstring q = SqlLiteral(query);
        wchar_t top[16];
        swprintf_s(top, L"%u", (unsigned)limit);
        // Names that start with the query or have a word that does; the newest first. AppData and the package
        // caches of developer tools are the index's noise, never what someone types into Start for.
        std::wstring sql = L"SELECT TOP ";
        sql += top;
        sql += L" System.ItemUrl, System.ItemNameDisplay, System.ItemFolderPathDisplay FROM SYSTEMINDEX WHERE "
               L"SCOPE='file:";
        sql += SqlLiteral(profile);
        sql += L"' AND (System.FileName LIKE '";
        sql += q;
        sql += L"%' OR System.FileName LIKE '% ";
        sql += q;
        sql += L"%') AND NOT System.ItemUrl LIKE '%/AppData/%' AND NOT System.ItemUrl LIKE '%/node_modules/%' "
               L"AND NOT System.ItemUrl LIKE '%/.git/%' AND NOT System.ItemUrl LIKE '%/go/pkg/%' "
               L"AND NOT System.ItemUrl LIKE '%/Library/PackageCache/%' ORDER BY System.DateModified DESC";

        ComPtr<ICommandText> command;
        ComPtr<IRowset> rowset;
        ComPtr<IAccessor> accessor;
        HRESULT hr;
        if (FAILED(hr = m_create->CreateCommand(nullptr, __uuidof(ICommandText),
                                                reinterpret_cast<IUnknown**>(command.GetAddressOf()))) ||
            FAILED(hr = command->SetCommandText(kDbGuidDefault, sql.c_str())) ||
            FAILED(hr = command->Execute(nullptr, __uuidof(IRowset), nullptr, nullptr,
                                         reinterpret_cast<IUnknown**>(rowset.GetAddressOf()))) ||
            !rowset || FAILED(hr = rowset.As(&accessor)))
        {
            if (FAILED(hr))
            {
                SP_LOG_ERR(kTag, L"Index query failed (0x%08X)", hr);
                m_create.Reset();   // reopen next time: the service may have restarted
                m_source.Reset();
            }
            return results;
        }

        DBBINDING bindings[3] = {};
        const DBBYTEOFFSET values[3] = { offsetof(Row, url), offsetof(Row, name), offsetof(Row, folder) };
        const DBBYTEOFFSET lengths[3] = { offsetof(Row, urlLength), offsetof(Row, nameLength), offsetof(Row, folderLength) };
        const DBBYTEOFFSET statuses[3] = { offsetof(Row, urlStatus), offsetof(Row, nameStatus), offsetof(Row, folderStatus) };
        const DBLENGTH sizes[3] = { sizeof(Row::url), sizeof(Row::name), sizeof(Row::folder) };
        for (int i = 0; i < 3; ++i)
        {
            bindings[i].iOrdinal = i + 1;
            bindings[i].obValue = values[i];
            bindings[i].obLength = lengths[i];
            bindings[i].obStatus = statuses[i];
            bindings[i].dwPart = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
            bindings[i].dwMemOwner = DBMEMOWNER_CLIENTOWNED;
            bindings[i].eParamIO = DBPARAMIO_NOTPARAM;
            bindings[i].cbMaxLen = sizes[i];
            bindings[i].wType = DBTYPE_WSTR;
        }
        HACCESSOR handle = 0;
        DBBINDSTATUS bindStatus[3] = {};
        if (FAILED(accessor->CreateAccessor(DBACCESSOR_ROWDATA, 3, bindings, sizeof(Row), &handle, bindStatus)))
        {
            return results;
        }
        DBCOUNTITEM obtained = 0;
        HROW* rows = nullptr;
        while (results.size() < limit &&
               SUCCEEDED(rowset->GetNextRows(DB_NULL_HCHAPTER, 0, 16, &obtained, &rows)) && obtained > 0)
        {
            for (DBCOUNTITEM i = 0; i < obtained; ++i)
            {
                Row row = {};
                if (SUCCEEDED(rowset->GetData(rows[i], handle, &row)) && row.urlStatus == DBSTATUS_S_OK &&
                    results.size() < limit)
                {
                    Result r;
                    r.kind = ResultKind::Open;
                    r.target = PathFromUrl(row.url);
                    r.title = row.nameStatus == DBSTATUS_S_OK ? row.name : r.target;
                    r.subtitle = row.folderStatus == DBSTATUS_S_OK ? row.folder : L"";
                    results.push_back(std::move(r));
                }
            }
            rowset->ReleaseRows(obtained, rows, nullptr, nullptr, nullptr);
            CoTaskMemFree(rows);
            rows = nullptr;
        }
        accessor->ReleaseAccessor(handle, nullptr);
        return results;
    }

private:
    ComPtr<IDBInitialize> m_source;
    ComPtr<IDBCreateCommand> m_create;
};

} // namespace

bool FileSearch::Start(HWND notify, UINT message, WPARAM kind)
{
    m_notify = notify;
    m_message = message;
    m_kind = kind;
    m_stop = 0;
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_thread = m_wake ? CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr) : nullptr;
    return m_thread != nullptr;
}

void FileSearch::Stop()
{
    if (m_thread)
    {
        InterlockedExchange(&m_stop, 1);
        SetEvent(m_wake);
        // A query in flight finishes in well under a second; the index cannot be interrupted.
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_wake)
    {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
}

void FileSearch::Request(unsigned id, const std::wstring& query, size_t limit)
{
    AcquireSRWLockExclusive(&m_lock);
    m_id = id;
    m_query = query;
    m_limit = limit;
    ReleaseSRWLockExclusive(&m_lock);
    if (m_wake)
    {
        SetEvent(m_wake);
    }
}

DWORD WINAPI FileSearch::ThreadProc(void* param)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<FileSearch*>(param)->Run();
    if (SUCCEEDED(hr))
    {
        CoUninitialize();
    }
    return 0;
}

void FileSearch::Run()
{
    Index index;
    unsigned answered = 0;
    while (!m_stop)
    {
        WaitForSingleObject(m_wake, INFINITE);
        for (;;)
        {
            if (m_stop)
            {
                return;
            }
            // Debounce: wait until typing pauses.
            AcquireSRWLockShared(&m_lock);
            unsigned id = m_id;
            ReleaseSRWLockShared(&m_lock);
            Sleep(150);
            AcquireSRWLockShared(&m_lock);
            bool settled = id == m_id;
            std::wstring query = m_query;
            size_t limit = m_limit;
            ReleaseSRWLockShared(&m_lock);
            if (!settled)
            {
                continue;
            }
            if (id == answered || query.size() < 2)
            {
                break;
            }
            auto* results = new FileResults{ id, index.Query(query, limit) };
            answered = id;
            AcquireSRWLockShared(&m_lock);
            bool current = id == m_id;
            ReleaseSRWLockShared(&m_lock);
            if (!current || !PostMessageW(m_notify, m_message, m_kind, reinterpret_cast<LPARAM>(results)))
            {
                delete results;
            }
            break;
        }
    }
}

} // namespace sm
