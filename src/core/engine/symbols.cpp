#include "symbols.h"
#include "hooks.h"
#include "log.h"
#include "config.h"

#include <tchar.h>

#include <Shlobj.h>
#include <winhttp.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "Shell32.lib")

#define TAG "symbols"

namespace {

// ---------------------------------------------------------------------------------------------------------------
// dbghelp, bound at runtime
//
// dbghelp.dll is loaded from System32 only, and only when a PDB actually has to be read. Binding the handful of
// entry points by hand keeps it out of the import table, so a process that never needs symbols never loads it.
// ---------------------------------------------------------------------------------------------------------------

#define SYMOPT_EXACT_SYMBOLS_LOCAL 0x00000400
#define SYMOPT_FAIL_CRITICAL_ERRORS_LOCAL 0x00000200
#define SYMOPT_NO_PROMPTS_LOCAL 0x00080000
#define SYMOPT_UNDNAME_LOCAL 0x00000002

// DIA's get_undecoratedName uses these flags, and the mod symbol names were written against that output:
// UNDNAME_32_BIT_DECODE (0x800) | UNDNAME_NO_PTR64 (0x20000).
constexpr DWORD kUndecorateFlags = 0x20800;

struct SYMBOL_INFO_W
{
    ULONG   SizeOfStruct;
    ULONG   TypeIndex;
    ULONG64 Reserved[2];
    ULONG   Index;
    ULONG   Size;
    ULONG64 ModBase;
    ULONG   Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG   Register;
    ULONG   Scope;
    ULONG   Tag;
    ULONG   NameLen;
    ULONG   MaxNameLen;
    WCHAR   Name[1];
};

using PSYM_ENUMERATESYMBOLS_CALLBACK_W = BOOL(CALLBACK*)(SYMBOL_INFO_W*, ULONG, PVOID);

using SymSetOptions_t = DWORD(WINAPI*)(DWORD);
using SymInitializeW_t = BOOL(WINAPI*)(HANDLE, PCWSTR, BOOL);
using SymCleanup_t = BOOL(WINAPI*)(HANDLE);
using SymLoadModuleExW_t = DWORD64(WINAPI*)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD, PVOID, DWORD);
using SymUnloadModule64_t = BOOL(WINAPI*)(HANDLE, DWORD64);
using SymEnumSymbolsW_t = BOOL(WINAPI*)(HANDLE, ULONG64, PCWSTR, PSYM_ENUMERATESYMBOLS_CALLBACK_W, PVOID);
using UnDecorateSymbolNameW_t = DWORD(WINAPI*)(PCWSTR, PWSTR, DWORD, DWORD);

struct DbgHelp
{
    HMODULE module = nullptr;
    SymSetOptions_t SymSetOptions = nullptr;
    SymInitializeW_t SymInitializeW = nullptr;
    SymCleanup_t SymCleanup = nullptr;
    SymLoadModuleExW_t SymLoadModuleExW = nullptr;
    SymUnloadModule64_t SymUnloadModule64 = nullptr;
    SymEnumSymbolsW_t SymEnumSymbolsW = nullptr;
    UnDecorateSymbolNameW_t UnDecorateSymbolNameW = nullptr;

    bool Load()
    {
        if (module)
        {
            return true;
        }
        module = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
        {
            SP_LOG_ERR(TAG, L"dbghelp.dll could not be loaded: %lu", GetLastError());
            return false;
        }

        SymSetOptions = (SymSetOptions_t)GetProcAddress(module, "SymSetOptions");
        SymInitializeW = (SymInitializeW_t)GetProcAddress(module, "SymInitializeW");
        SymCleanup = (SymCleanup_t)GetProcAddress(module, "SymCleanup");
        SymLoadModuleExW = (SymLoadModuleExW_t)GetProcAddress(module, "SymLoadModuleExW");
        SymUnloadModule64 = (SymUnloadModule64_t)GetProcAddress(module, "SymUnloadModule64");
        SymEnumSymbolsW = (SymEnumSymbolsW_t)GetProcAddress(module, "SymEnumSymbolsW");
        UnDecorateSymbolNameW = (UnDecorateSymbolNameW_t)GetProcAddress(module, "UnDecorateSymbolNameW");

        if (!SymSetOptions || !SymInitializeW || !SymCleanup || !SymLoadModuleExW ||
            !SymUnloadModule64 || !SymEnumSymbolsW || !UnDecorateSymbolNameW)
        {
            SP_LOG_ERR(TAG, L"dbghelp.dll is missing an expected entry point");
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        return true;
    }
};

DbgHelp g_dbghelp;

// dbghelp keys everything by a "process handle" that only has to be unique. A private token is used rather than
// GetCurrentProcess(), so the engine cannot disturb, or be disturbed by, another component in explorer.exe that
// uses dbghelp on the real process handle.
HANDLE const kSymProcess = (HANDLE)(ULONG_PTR)0x53506174;   // 'SPat'

std::mutex g_symbolMutex;   // dbghelp is not safe to call from several threads at once

// ---------------------------------------------------------------------------------------------------------------
// Module identity
// ---------------------------------------------------------------------------------------------------------------

struct ModuleIdentity
{
    std::wstring fileName;      // "taskbar.dll", lowercase
    std::wstring pdbName;       // "taskbar.pdb"
    std::wstring pdbIdentity;   // "<32 hex GUID><hex age>", the symbol server's directory name
    DWORD        imageSize = 0;
    bool         valid = false;
};

// The CodeView record the linker leaves in the debug directory: "RSDS", a GUID, an age and the PDB file name.
#pragma pack(push, 1)
struct CvInfoPdb70
{
    DWORD signature;    // 'SDSR'
    GUID  guid;
    DWORD age;
    char  pdbFileName[1];
};
#pragma pack(pop)

// dbghelp leaves the "__ptr64" that every 64-bit pointer carries in its decorated form:
//     struct IShellBrowser * __ptr64,struct tagRECT * __ptr64
// DIA, which is what published symbol names are written against, drops it:
//     struct IShellBrowser *,struct tagRECT *
// The flag that should suppress it (UNDNAME_NO_PTR64) is a DIA flag, not a dbghelp one, so dbghelp ignores it.
// Removing the token here is what makes a name found in the PDB match the name a mod asks for.
void StripPtr64(wchar_t* text)
{
    static const wchar_t kToken[] = L" __ptr64";
    constexpr size_t kTokenLength = ARRAYSIZE(kToken) - 1;

    wchar_t* read = text;
    wchar_t* write = text;
    while (*read)
    {
        if (wcsncmp(read, kToken, kTokenLength) == 0)
        {
            read += kTokenLength;
            continue;
        }
        *write++ = *read++;
    }
    *write = L'\0';
}

std::wstring ToLower(std::wstring s)
{
    if (!s.empty())
    {
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), (int)s.size(),
                      s.data(), (int)s.size(), nullptr, nullptr, 0);
    }
    return s;
}

// modulePath may be null, in which case it is recovered from the handle. That fails for a module mapped with
// LOAD_LIBRARY_AS_IMAGE_RESOURCE, which is not in the process module list, so callers that map a file that way
// pass the path they opened.
ModuleIdentity GetModuleIdentity(HMODULE module, const wchar_t* modulePath)
{
    ModuleIdentity id;

    wchar_t wszPath[MAX_PATH];
    if (modulePath)
    {
        wcsncpy_s(wszPath, modulePath, _TRUNCATE);
    }
    else if (!GetModuleFileNameW(module, wszPath, MAX_PATH))
    {
        return id;
    }
    const wchar_t* pName = wcsrchr(wszPath, L'\\');
    id.fileName = ToLower(pName ? pName + 1 : wszPath);

    auto* dosHeader = (const IMAGE_DOS_HEADER*)module;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return id;
    }
    auto* ntHeader = (const IMAGE_NT_HEADERS*)((const BYTE*)dosHeader + dosHeader->e_lfanew);
    if (ntHeader->Signature != IMAGE_NT_SIGNATURE)
    {
        return id;
    }
    id.imageSize = ntHeader->OptionalHeader.SizeOfImage;

    if (ntHeader->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
    {
        return id;
    }
    const auto& dir = ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
    {
        return id;
    }
    // Everything below is read out of a mapped image, so each offset is bounded against SizeOfImage first.
    if (dir.VirtualAddress >= id.imageSize || dir.Size > id.imageSize - dir.VirtualAddress)
    {
        return id;
    }

    auto* debugDir = (const IMAGE_DEBUG_DIRECTORY*)((const BYTE*)module + dir.VirtualAddress);
    DWORD count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);

    for (DWORD i = 0; i < count; ++i)
    {
        if (debugDir[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW)
        {
            continue;
        }
        DWORD rva = debugDir[i].AddressOfRawData;
        DWORD size = debugDir[i].SizeOfData;
        if (!rva || size < sizeof(CvInfoPdb70) || rva >= id.imageSize || size > id.imageSize - rva)
        {
            continue;
        }

        auto* cv = (const CvInfoPdb70*)((const BYTE*)module + rva);
        if (cv->signature != 0x53445352)    // 'RSDS' little endian
        {
            continue;
        }

        // The name is NUL terminated inside the record; bound the search by the record size.
        size_t maxName = size - offsetof(CvInfoPdb70, pdbFileName);
        size_t nameLen = strnlen(cv->pdbFileName, maxName);
        if (nameLen == 0 || nameLen == maxName)
        {
            continue;
        }

        std::string pdbPath(cv->pdbFileName, nameLen);
        size_t slash = pdbPath.find_last_of("\\/");
        std::string pdbFile = (slash == std::string::npos) ? pdbPath : pdbPath.substr(slash + 1);

        int cch = MultiByteToWideChar(CP_UTF8, 0, pdbFile.c_str(), (int)pdbFile.size(), nullptr, 0);
        if (cch <= 0)
        {
            continue;
        }
        id.pdbName.resize(cch);
        MultiByteToWideChar(CP_UTF8, 0, pdbFile.c_str(), (int)pdbFile.size(), id.pdbName.data(), cch);

        wchar_t wszIdentity[64];
        swprintf_s(wszIdentity, L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
                   cv->guid.Data1, cv->guid.Data2, cv->guid.Data3,
                   cv->guid.Data4[0], cv->guid.Data4[1], cv->guid.Data4[2], cv->guid.Data4[3],
                   cv->guid.Data4[4], cv->guid.Data4[5], cv->guid.Data4[6], cv->guid.Data4[7],
                   cv->age);
        id.pdbIdentity = wszIdentity;
        id.valid = true;
        return id;
    }

    return id;
}

// ---------------------------------------------------------------------------------------------------------------
// The offset cache
//
// HKCU\Software\ShadePatcher\SymbolCache\<module file name>
//     pdb_<identity>  = "<symbol>|<offset>|<symbol>|<offset>|..."
//
// An offset of "-" records that the symbol genuinely does not exist in this build, so an optional symbol is not
// looked up again on every start.
// ---------------------------------------------------------------------------------------------------------------

constexpr wchar_t kCacheSeparator = L'|';

std::wstring CacheKeyPath(const ModuleIdentity& id)
{
    return std::wstring(_T(REGPATH)) + L"\\SymbolCache\\" + id.fileName;
}

std::wstring CacheValueName(const ModuleIdentity& id)
{
    return L"pdb_" + id.pdbIdentity;
}

std::vector<std::wstring> SplitCache(const std::wstring& text)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(kCacheSeparator, start);
        if (end == std::wstring::npos)
        {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// Reads the cache into a map of symbol name -> offset. An offset of -1 means "known to be absent".
std::map<std::wstring, LONGLONG> ReadCache(const ModuleIdentity& id)
{
    std::map<std::wstring, LONGLONG> result;

    std::wstring path = CacheKeyPath(id);
    std::wstring name = CacheValueName(id);

    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS ||
        cb <= sizeof(wchar_t))
    {
        return result;
    }

    std::wstring buffer(cb / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, buffer.data(), &cb) != ERROR_SUCCESS)
    {
        return result;
    }
    buffer.resize(wcslen(buffer.c_str()));

    auto parts = SplitCache(buffer);
    for (size_t i = 0; i + 1 < parts.size(); i += 2)
    {
        const std::wstring& symbol = parts[i];
        const std::wstring& offsetText = parts[i + 1];
        if (symbol.empty())
        {
            continue;
        }
        if (offsetText == L"-")
        {
            result[symbol] = -1;
            continue;
        }

        wchar_t* end = nullptr;
        unsigned long long offset = wcstoull(offsetText.c_str(), &end, 10);
        if (!end || *end != L'\0')
        {
            continue;
        }
        // The cache is writable by anyone; an offset outside the image would aim a hook at an arbitrary address.
        if (offset >= id.imageSize)
        {
            SP_LOG_ERR(TAG, L"Ignoring cached offset outside %s (%llu >= %lu)", id.fileName.c_str(), offset, id.imageSize);
            continue;
        }
        result[symbol] = (LONGLONG)offset;
    }

    return result;
}

void WriteCache(const ModuleIdentity& id, const std::map<std::wstring, LONGLONG>& entries)
{
    std::wstring text;
    for (const auto& [symbol, offset] : entries)
    {
        // A separator inside a symbol name would corrupt the record. No MSVC decorated or undecorated name uses
        // '|', but the guard costs nothing and keeps a malformed PDB from producing a malformed cache.
        if (symbol.find(kCacheSeparator) != std::wstring::npos)
        {
            continue;
        }
        if (!text.empty())
        {
            text += kCacheSeparator;
        }
        text += symbol;
        text += kCacheSeparator;
        text += (offset < 0) ? std::wstring(L"-") : std::to_wstring((unsigned long long)offset);
    }

    std::wstring path = CacheKeyPath(id);
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
    {
        return;
    }
    RegSetValueExW(hKey, CacheValueName(id).c_str(), 0, REG_SZ,
                   (const BYTE*)text.c_str(), (DWORD)((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------------------------------------------
// The PDB file
// ---------------------------------------------------------------------------------------------------------------

std::wstring SymbolFolder()
{
    wchar_t wszDir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, wszDir)))
    {
        return {};
    }
    std::wstring path = wszDir;
    path += L"\\" _T(PRODUCT_NAME);
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\symbols";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

// The symbol server lays files out as <pdb name>\<identity>\<pdb name>; the local cache mirrors that.
std::wstring LocalPdbPath(const ModuleIdentity& id, bool createDirectories)
{
    std::wstring root = SymbolFolder();
    if (root.empty())
    {
        return {};
    }
    std::wstring path = root + L"\\" + id.pdbName;
    if (createDirectories)
    {
        CreateDirectoryW(path.c_str(), nullptr);
    }
    path += L"\\" + id.pdbIdentity;
    if (createDirectories)
    {
        CreateDirectoryW(path.c_str(), nullptr);
    }
    path += L"\\" + id.pdbName;
    return path;
}

bool FileExists(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool DownloadPdb(const ModuleIdentity& id, const std::wstring& targetPath)
{
    SP_LOG_INF(TAG, L"Downloading %s for %s", id.pdbName.c_str(), id.fileName.c_str());

    HINTERNET hSession = WinHttpOpen(L"Microsoft-Symbol-Server/10.0.0.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
    {
        SP_LOG_ERR(TAG, L"WinHttpOpen failed: %lu", GetLastError());
        return false;
    }

    // A shell process must never block on the network; these caps bound the worst case.
    DWORD timeout = 30000;
    WinHttpSetTimeouts(hSession, 10000, 10000, timeout, timeout);

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, L"msdl.microsoft.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect)
    {
        std::wstring object = L"/download/symbols/" + id.pdbName + L"/" + id.pdbIdentity + L"/" + id.pdbName;

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", object.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest)
        {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr))
            {
                DWORD statusCode = 0;
                DWORD cb = sizeof(statusCode);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &cb, WINHTTP_NO_HEADER_INDEX);

                if (statusCode == 200)
                {
                    // Written to a temporary name and moved into place, so a failed download never leaves a
                    // truncated PDB that later runs would treat as valid.
                    std::wstring tempPath = targetPath + L".part";
                    HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hFile != INVALID_HANDLE_VALUE)
                    {
                        std::vector<BYTE> buffer(64 * 1024);
                        bool writeFailed = false;
                        DWORD dwRead = 0;
                        while (WinHttpReadData(hRequest, buffer.data(), (DWORD)buffer.size(), &dwRead) && dwRead > 0)
                        {
                            DWORD dwWritten = 0;
                            if (!WriteFile(hFile, buffer.data(), dwRead, &dwWritten, nullptr) || dwWritten != dwRead)
                            {
                                writeFailed = true;
                                break;
                            }
                        }
                        CloseHandle(hFile);

                        if (!writeFailed)
                        {
                            DeleteFileW(targetPath.c_str());
                            ok = MoveFileW(tempPath.c_str(), targetPath.c_str()) != FALSE;
                        }
                        if (!ok)
                        {
                            DeleteFileW(tempPath.c_str());
                        }
                    }
                }
                else
                {
                    SP_LOG_ERR(TAG, L"Symbol server returned %lu for %s", statusCode, id.pdbName.c_str());
                }
            }
            else
            {
                SP_LOG_ERR(TAG, L"The symbol server request failed: %lu", GetLastError());
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);

    if (ok)
    {
        SP_LOG_INF(TAG, L"Downloaded %s", id.pdbName.c_str());
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Enumerating a PDB
// ---------------------------------------------------------------------------------------------------------------

struct EnumContext
{
    const ModuleIdentity*             id;
    HMODULE                           module;
    const std::vector<std::wstring>*  wanted;      // names still to find
    std::map<std::wstring, LONGLONG>* resolved;    // name -> offset
    size_t                            remaining;
};

BOOL CALLBACK EnumSymbolsCallback(SYMBOL_INFO_W* info, ULONG /*size*/, PVOID context)
{
    auto* ctx = (EnumContext*)context;
    if (!info || info->NameLen == 0)
    {
        return TRUE;
    }

    ULONG64 address = info->Address;
    ULONG64 base = (ULONG64)(ULONG_PTR)ctx->module;
    if (address < base)
    {
        return TRUE;
    }
    ULONG64 offset = address - base;
    // A PDB in a world-writable folder is untrusted input, exactly like the registry cache.
    if (offset >= ctx->id->imageSize)
    {
        return TRUE;
    }

    std::wstring decorated(info->Name, info->NameLen);

    // Mods name functions the way DIA prints them, so the decorated name is undecorated here with the same flags.
    wchar_t wszUndecorated[2048];
    wszUndecorated[0] = 0;
    const wchar_t* candidates[2] = { nullptr, nullptr };
    int candidateCount = 0;

    if (g_dbghelp.UnDecorateSymbolNameW(decorated.c_str(), wszUndecorated, ARRAYSIZE(wszUndecorated), kUndecorateFlags) > 0)
    {
        StripPtr64(wszUndecorated);
        candidates[candidateCount++] = wszUndecorated;
    }
    // C functions and public symbols have no decoration, so the raw name has to be tried too.
    candidates[candidateCount++] = decorated.c_str();

    for (int c = 0; c < candidateCount; ++c)
    {
        for (const std::wstring& want : *ctx->wanted)
        {
            if (want != candidates[c])
            {
                continue;
            }
            auto it = ctx->resolved->find(want);
            if (it != ctx->resolved->end() && it->second >= 0)
            {
                continue;   // already found
            }
            (*ctx->resolved)[want] = (LONGLONG)offset;
            if (ctx->remaining > 0)
            {
                ctx->remaining--;
            }
            SP_LOG_DBG(TAG, L"Resolved +0x%llX: %s", offset, want.c_str());
            if (ctx->remaining == 0)
            {
                return FALSE;   // every wanted symbol has been found; stop early
            }
        }
    }

    return TRUE;
}

// Loads the PDB and fills `resolved` for the names in `wanted` that are not already there.
bool EnumerateFromPdb(const ModuleIdentity& id, HMODULE module,
                      const std::vector<std::wstring>& wanted,
                      std::map<std::wstring, LONGLONG>& resolved)
{
    std::wstring pdbPath = LocalPdbPath(id, true);
    if (pdbPath.empty())
    {
        return false;
    }
    if (!FileExists(pdbPath) && !DownloadPdb(id, pdbPath))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_symbolMutex);

    if (!g_dbghelp.Load())
    {
        return false;
    }

    g_dbghelp.SymSetOptions(SYMOPT_EXACT_SYMBOLS_LOCAL | SYMOPT_FAIL_CRITICAL_ERRORS_LOCAL | SYMOPT_NO_PROMPTS_LOCAL);

    if (!g_dbghelp.SymInitializeW(kSymProcess, nullptr, FALSE))
    {
        SP_LOG_ERR(TAG, L"SymInitializeW failed: %lu", GetLastError());
        return false;
    }

    bool ok = false;
    DWORD64 loadedBase = g_dbghelp.SymLoadModuleExW(kSymProcess, nullptr, pdbPath.c_str(), nullptr,
                                                    (DWORD64)(ULONG_PTR)module, id.imageSize, nullptr, 0);
    if (loadedBase)
    {
        size_t remaining = 0;
        for (const std::wstring& want : wanted)
        {
            auto it = resolved.find(want);
            if (it == resolved.end() || it->second < 0)
            {
                remaining++;
            }
        }

        EnumContext ctx{ &id, module, &wanted, &resolved, remaining };
        g_dbghelp.SymEnumSymbolsW(kSymProcess, loadedBase, L"*", EnumSymbolsCallback, &ctx);
        g_dbghelp.SymUnloadModule64(kSymProcess, loadedBase);
        ok = true;
    }
    else
    {
        SP_LOG_ERR(TAG, L"SymLoadModuleExW failed for %s: %lu", pdbPath.c_str(), GetLastError());
    }

    g_dbghelp.SymCleanup(kSymProcess);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Enumeration, for tooling
// ---------------------------------------------------------------------------------------------------------------

struct WalkContext
{
    const ModuleIdentity* id;
    HMODULE               module;
    SP_SymbolEnumProc     callback;
    void*                 context;
};

BOOL CALLBACK WalkSymbolsCallback(SYMBOL_INFO_W* info, ULONG /*size*/, PVOID context)
{
    auto* ctx = (WalkContext*)context;
    if (!info || info->NameLen == 0)
    {
        return TRUE;
    }

    ULONG64 base = (ULONG64)(ULONG_PTR)ctx->module;
    if (info->Address < base)
    {
        return TRUE;
    }
    ULONG64 offset = info->Address - base;
    if (offset >= ctx->id->imageSize)
    {
        return TRUE;
    }

    std::wstring decorated(info->Name, info->NameLen);

    wchar_t wszUndecorated[2048];
    wszUndecorated[0] = 0;
    if (g_dbghelp.UnDecorateSymbolNameW(decorated.c_str(), wszUndecorated, ARRAYSIZE(wszUndecorated),
                                        kUndecorateFlags) == 0)
    {
        wszUndecorated[0] = 0;
    }
    else
    {
        StripPtr64(wszUndecorated);
    }

    return ctx->callback(wszUndecorated, decorated.c_str(), offset, ctx->context);
}

bool WalkPdb(const ModuleIdentity& id, HMODULE module, SP_SymbolEnumProc callback, void* context)
{
    std::wstring pdbPath = LocalPdbPath(id, true);
    if (pdbPath.empty())
    {
        return false;
    }
    if (!FileExists(pdbPath) && !DownloadPdb(id, pdbPath))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_symbolMutex);

    if (!g_dbghelp.Load())
    {
        return false;
    }

    g_dbghelp.SymSetOptions(SYMOPT_EXACT_SYMBOLS_LOCAL | SYMOPT_FAIL_CRITICAL_ERRORS_LOCAL | SYMOPT_NO_PROMPTS_LOCAL);

    if (!g_dbghelp.SymInitializeW(kSymProcess, nullptr, FALSE))
    {
        return false;
    }

    bool ok = false;
    DWORD64 loadedBase = g_dbghelp.SymLoadModuleExW(kSymProcess, nullptr, pdbPath.c_str(), nullptr,
                                                    (DWORD64)(ULONG_PTR)module, id.imageSize, nullptr, 0);
    if (loadedBase)
    {
        WalkContext ctx{ &id, module, callback, context };
        g_dbghelp.SymEnumSymbolsW(kSymProcess, loadedBase, L"*", WalkSymbolsCallback, &ctx);
        g_dbghelp.SymUnloadModule64(kSymProcess, loadedBase);
        ok = true;
    }

    g_dbghelp.SymCleanup(kSymProcess);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------------------------------------------

// Fills in *pOriginal for every hook whose name is in `resolved`. Returns the number still unresolved.
size_t ApplyResolved(HMODULE module, const SP_SymbolHook* hooks, size_t hookCount,
                     const std::map<std::wstring, LONGLONG>& resolved,
                     std::vector<void*>& addresses)
{
    size_t unresolved = 0;

    for (size_t i = 0; i < hookCount; ++i)
    {
        if (addresses[i])
        {
            continue;
        }
        bool knownAbsent = true;
        for (size_t s = 0; s < hooks[i].symbolCount; ++s)
        {
            auto it = resolved.find(hooks[i].symbols[s]);
            if (it == resolved.end())
            {
                knownAbsent = false;    // never looked up, so nothing is known about it yet
                continue;
            }
            if (it->second >= 0)
            {
                addresses[i] = (BYTE*)module + it->second;
                break;
            }
        }
        if (!addresses[i])
        {
            // An optional symbol that every candidate name says is absent counts as settled, not missing.
            if (!(hooks[i].optional && knownAbsent))
            {
                unresolved++;
            }
        }
    }

    return unresolved;
}

bool ResolveSymbols(const char* owner, HMODULE module, const wchar_t* modulePath,
                    const SP_SymbolHook* hooks, size_t hookCount, std::vector<void*>& addresses)
{
    ModuleIdentity id = GetModuleIdentity(module, modulePath);
    if (!id.valid)
    {
        SP_LOG_ERR(TAG, L"%S: no PDB identity for the target module", owner);
        return false;
    }

    addresses.assign(hookCount, nullptr);

    // 1. What the cache already knows.
    std::map<std::wstring, LONGLONG> resolved = ReadCache(id);
    size_t unresolved = ApplyResolved(module, hooks, hookCount, resolved, addresses);
    if (unresolved == 0)
    {
        SP_LOG_DBG(TAG, L"%S: every symbol came from the cache for %s", owner, id.fileName.c_str());
        return true;
    }

    // 2. The PDB, for whatever is left.
    std::vector<std::wstring> wanted;
    for (size_t i = 0; i < hookCount; ++i)
    {
        if (addresses[i])
        {
            continue;
        }
        for (size_t s = 0; s < hooks[i].symbolCount; ++s)
        {
            wanted.emplace_back(hooks[i].symbols[s]);
        }
    }

    SP_LOG_INF(TAG, L"%S: looking up %zu symbol name(s) in %s", owner, wanted.size(), id.pdbName.c_str());

    if (!EnumerateFromPdb(id, module, wanted, resolved))
    {
        SP_LOG_ERR(TAG, L"%S: %s could not be read", owner, id.pdbName.c_str());
        return false;
    }

    // A name that the PDB did not contain is recorded as absent, so the next start does not read the PDB again.
    for (const std::wstring& want : wanted)
    {
        if (resolved.find(want) == resolved.end())
        {
            resolved[want] = -1;
        }
    }

    unresolved = ApplyResolved(module, hooks, hookCount, resolved, addresses);
    WriteCache(id, resolved);

    if (unresolved > 0)
    {
        SP_LOG_ERR(TAG, L"%S: %zu required symbol(s) were not found in %s", owner, unresolved, id.pdbName.c_str());
        return false;
    }
    return true;
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------------------------------------------

extern "C" void SP_SymbolsInitialize(void)
{
    // The symbol folder is created on first use; nothing has to happen at startup.
}

extern "C" void SP_SymbolsShutdown(void)
{
    std::lock_guard<std::mutex> lock(g_symbolMutex);
    if (g_dbghelp.module)
    {
        FreeLibrary(g_dbghelp.module);
        g_dbghelp.module = nullptr;
    }
}

extern "C" BOOL SP_ResolveSymbolsAtOwned(const char* owner, HMODULE module, const wchar_t* modulePath,
                                         const SP_SymbolHook* hooks, size_t hookCount)
{
    if (!module || !hooks || hookCount == 0)
    {
        return FALSE;
    }

    std::vector<void*> addresses;
    if (!ResolveSymbols(owner, module, modulePath, hooks, hookCount, addresses))
    {
        return FALSE;
    }

    for (size_t i = 0; i < hookCount; ++i)
    {
        if (hooks[i].pOriginal)
        {
            *hooks[i].pOriginal = addresses[i];
        }
    }
    return TRUE;
}

extern "C" BOOL SP_ResolveSymbolsOwned(const char* owner, HMODULE module, const SP_SymbolHook* hooks, size_t hookCount)
{
    return SP_ResolveSymbolsAtOwned(owner, module, nullptr, hooks, hookCount);
}

extern "C" BOOL SP_EnumSymbolsAtOwned(const char* owner, HMODULE module, const wchar_t* modulePath,
                                      SP_SymbolEnumProc callback, void* context)
{
    if (!module || !callback)
    {
        return FALSE;
    }

    ModuleIdentity id = GetModuleIdentity(module, modulePath);
    if (!id.valid)
    {
        SP_LOG_ERR(TAG, L"%S: no PDB identity for the target module", owner);
        return FALSE;
    }

    return WalkPdb(id, module, callback, context) ? TRUE : FALSE;
}

extern "C" BOOL SP_HookSymbolsOwned(const char* owner, HMODULE module, const SP_SymbolHook* hooks, size_t hookCount)
{
    if (!module || !hooks || hookCount == 0)
    {
        return FALSE;
    }

    std::vector<void*> addresses;
    if (!ResolveSymbols(owner, module, nullptr, hooks, hookCount, addresses))
    {
        return FALSE;
    }

    if (!SP_HookBegin())
    {
        return FALSE;
    }

    BOOL allOk = TRUE;
    for (size_t i = 0; i < hookCount; ++i)
    {
        if (!addresses[i])
        {
            // Only an optional symbol reaches this point; leave the mod's pointer NULL so it can tell.
            if (hooks[i].pOriginal)
            {
                *hooks[i].pOriginal = nullptr;
            }
            continue;
        }

        if (!hooks[i].hookFunction)
        {
            if (hooks[i].pOriginal)
            {
                *hooks[i].pOriginal = addresses[i];
            }
            continue;
        }

        if (!SP_SetFunctionHookOwned(owner, addresses[i], hooks[i].hookFunction, hooks[i].pOriginal))
        {
            allOk = FALSE;
            break;
        }
    }

    if (!allOk)
    {
        SP_HookAbort();
        return FALSE;
    }

    return SP_HookCommit();
}
