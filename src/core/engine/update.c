//
// update.c - see update.h.
//
// The shape of this is deliberately small. One thread wakes up a little after the shell has settled, makes one
// request, and either finds nothing to say or puts a notification in the notification area. The notification is
// the ordinary Windows one, so it follows the user's Focus assist and notification settings like any other.
//
#include "update.h"

#include <shellapi.h>
#include <strsafe.h>
#include <tchar.h>
#include <winhttp.h>

#include "config.h"
#include "log.h"
#include "version.h"

#define TAG "update"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

#define SP_UPDATE_WINDOW_CLASS  L"" PRODUCT_NAME L"_UpdateNotice"
#define SP_UPDATE_ICON_ID       1
#define WM_SP_TRAY              (WM_APP + 71)

// Where to ask when the user has not named somewhere else. The releases endpoint answers with the newest
// release only, so the reply is small and needs no paging.
#define SP_UPDATE_DEFAULT_URL   L"https://api.github.com/repos/shadesofdeath/ShadePatcher/releases/latest"

extern HMODULE SP_GetCoreModule(void);

static HANDLE  g_thread = NULL;
static HANDLE  g_stopEvent = NULL;
static HWND    g_window = NULL;
static wchar_t g_releaseUrl[512] = L"" PRODUCT_URL L"/releases/latest";
static wchar_t g_version[64] = { 0 };

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

static DWORD ReadDword(const wchar_t* name, DWORD fallback)
{
    DWORD value = fallback;
    DWORD cb = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, _T(REGPATH), name, RRF_RT_REG_DWORD, NULL, &value, &cb) != ERROR_SUCCESS)
    {
        return fallback;
    }
    return value;
}

static BOOL ReadString(const wchar_t* name, wchar_t* buffer, DWORD cch)
{
    DWORD cb = cch * sizeof(wchar_t);
    return RegGetValueW(HKEY_CURRENT_USER, _T(REGPATH), name, RRF_RT_REG_SZ, NULL, buffer, &cb) == ERROR_SUCCESS &&
           buffer[0] != L'\0';
}

// ---------------------------------------------------------------------------------------------------------------
// Comparing versions
//
// A tag is written as "v1.2.3" or "1.2.3.4". Anything that is not a digit or a dot ends the number, so a tag
// with a suffix such as "1.2.3-beta" still compares as 1.2.3.
// ---------------------------------------------------------------------------------------------------------------

static void ParseVersion(const wchar_t* text, int parts[4])
{
    for (int i = 0; i < 4; ++i)
    {
        parts[i] = 0;
    }

    while (*text && (*text < L'0' || *text > L'9'))
    {
        text++;      // skip a leading "v" or "release-"
    }

    for (int i = 0; i < 4 && *text; ++i)
    {
        while (*text >= L'0' && *text <= L'9')
        {
            parts[i] = parts[i] * 10 + (*text - L'0');
            text++;
        }
        if (*text != L'.')
        {
            break;
        }
        text++;
    }
}

// Greater than zero when `candidate` is newer than what this build is.
static int CompareWithOurs(const wchar_t* candidate)
{
    int theirs[4];
    ParseVersion(candidate, theirs);

    const int ours[4] = { VER_MAJOR, VER_MINOR, VER_BUILD_HI, VER_BUILD_LO };

    for (int i = 0; i < 4; ++i)
    {
        if (theirs[i] != ours[i])
        {
            return theirs[i] - ours[i];
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Asking
// ---------------------------------------------------------------------------------------------------------------

// Pulls the value of a JSON string field out of a reply, without a JSON parser. The reply is a release object
// with one "tag_name" and one "html_url" at the top level, so a plain search is enough and cannot be confused by
// nesting that is not there.
static BOOL ExtractField(const char* json, const char* field, wchar_t* buffer, DWORD cch)
{
    char needle[64];
    if (FAILED(StringCchPrintfA(needle, ARRAYSIZE(needle), "\"%s\"", field)))
    {
        return FALSE;
    }

    const char* at = strstr(json, needle);
    if (!at)
    {
        return FALSE;
    }

    at = strchr(at + strlen(needle), ':');
    if (!at)
    {
        return FALSE;
    }
    at++;
    while (*at == ' ' || *at == '\t')
    {
        at++;
    }
    if (*at != '"')
    {
        return FALSE;
    }
    at++;

    const char* end = strchr(at, '"');
    if (!end || end == at)
    {
        return FALSE;
    }

    // One character is kept back for the terminator, which MultiByteToWideChar does not write when it is given
    // an explicit length.
    int written = MultiByteToWideChar(CP_UTF8, 0, at, (int)(end - at), buffer, (int)cch - 1);
    if (written <= 0)
    {
        return FALSE;
    }
    buffer[written] = L'\0';
    return TRUE;
}

// Returns the reply as a NUL-terminated UTF-8 buffer the caller frees, or NULL.
static char* Fetch(const wchar_t* url)
{
    URL_COMPONENTS parts;
    wchar_t wszHost[256] = { 0 };
    wchar_t wszPath[1024] = { 0 };

    ZeroMemory(&parts, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = wszHost;
    parts.dwHostNameLength = ARRAYSIZE(wszHost);
    parts.lpszUrlPath = wszPath;
    parts.dwUrlPathLength = ARRAYSIZE(wszPath);

    if (!WinHttpCrackUrl(url, 0, 0, &parts))
    {
        SP_LOG_ERR(TAG, L"The update address could not be read: %lu", GetLastError());
        return NULL;
    }

    HINTERNET hSession = WinHttpOpen(L"" PRODUCT_NAME L"/" VER_WITH_DOTS,
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
    {
        return NULL;
    }

    // The shell must never be held up by a slow network, so every stage is given a bound.
    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 10000);

    char* body = NULL;
    HINTERNET hConnect = WinHttpConnect(hSession, wszHost, parts.nPort, 0);
    HINTERNET hRequest = NULL;

    if (hConnect)
    {
        hRequest = WinHttpOpenRequest(hConnect, L"GET", wszPath, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }

    if (hRequest &&
        WinHttpSendRequest(hRequest, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL))
    {
        DWORD status = 0;
        DWORD cb = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &cb, NULL);

        if (status == 200)
        {
            DWORD capacity = 16384;
            DWORD used = 0;
            body = (char*)malloc(capacity);

            while (body)
            {
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, body + used, capacity - used - 1, &read) || read == 0)
                {
                    break;
                }
                used += read;

                if (used + 1 >= capacity)
                {
                    // A reply this large is not the one expected; stop rather than grow without bound.
                    break;
                }
            }

            if (body)
            {
                body[used] = '\0';
            }
        }
        else
        {
            SP_LOG_INF(TAG, L"The update check was answered with %lu", status);
        }
    }

    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return body;
}

// ---------------------------------------------------------------------------------------------------------------
// Saying so
//
// The notification needs a window to be delivered to, and that window needs a message loop. Both live on the
// checking thread, which stays alive until the notification is gone.
// ---------------------------------------------------------------------------------------------------------------

static void RememberAsSeen(void)
{
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, _T(REGPATH), 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, L"UpdateSkip", 0, REG_SZ, (const BYTE*)g_version,
                       (DWORD)((wcslen(g_version) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

static LRESULT CALLBACK UpdateWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_SP_TRAY)
    {
        switch (LOWORD(lParam))
        {
        case NIN_BALLOONUSERCLICK:
            // The user asked to see it, so the release page opens and the same version is not offered again.
            ShellExecuteW(NULL, L"open", g_releaseUrl, NULL, NULL, SW_SHOWNORMAL);
            RememberAsSeen();
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
            break;

        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONHIDE:
            // Dismissed or timed out. The version is not marked as seen, so it is offered again next time, which
            // is what makes this useful rather than a single easily-missed toast.
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
            break;
        }
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static void ShowNotice(void)
{
    HMODULE hModule = SP_GetCoreModule();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = UpdateWndProc;
    wc.hInstance = hModule;
    wc.lpszClassName = SP_UPDATE_WINDOW_CLASS;
    RegisterClassExW(&wc);

    g_window = CreateWindowExW(0, SP_UPDATE_WINDOW_CLASS, SP_UPDATE_WINDOW_CLASS, 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, hModule, NULL);
    if (!g_window)
    {
        SP_LOG_ERR(TAG, L"The notification window could not be created: %lu", GetLastError());
        return;
    }

    wchar_t wszText[256];
    StringCchPrintfW(wszText, ARRAYSIZE(wszText),
                     L"Sürüm %s yayımlandı. Ayrıntılar için buraya tıklayın.", g_version);

    NOTIFYICONDATAW ni;
    ZeroMemory(&ni, sizeof(ni));
    ni.cbSize = sizeof(ni);
    ni.hWnd = g_window;
    ni.uID = 1;
    ni.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    ni.uCallbackMessage = WM_SP_TRAY;
    ni.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    ni.hIcon = LoadIconW(hModule, MAKEINTRESOURCEW(SP_UPDATE_ICON_ID));
    ni.hBalloonIcon = ni.hIcon;
    StringCchCopyW(ni.szTip, ARRAYSIZE(ni.szTip), _T(PRODUCT_NAME));
    StringCchCopyW(ni.szInfoTitle, ARRAYSIZE(ni.szInfoTitle), _T(PRODUCT_NAME));
    StringCchCopyW(ni.szInfo, ARRAYSIZE(ni.szInfo), wszText);

    if (!Shell_NotifyIconW(NIM_ADD, &ni))
    {
        SP_LOG_ERR(TAG, L"The notification could not be shown: %lu", GetLastError());
        DestroyWindow(g_window);
        g_window = NULL;
        return;
    }

    SP_LOG_INF(TAG, L"Told the user about version %s", g_version);

    // The loop ends when the notification is clicked, dismissed or times out, and also when the engine stops.
    MSG msg;
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT)
    {
        if (!GetMessageW(&msg, NULL, 0, 0))
        {
            break;
        }
        if (msg.message == WM_QUIT)
        {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);

        if (!IsWindow(g_window))
        {
            break;
        }
    }

    Shell_NotifyIconW(NIM_DELETE, &ni);
    if (IsWindow(g_window))
    {
        DestroyWindow(g_window);
    }
    g_window = NULL;
}

// ---------------------------------------------------------------------------------------------------------------
// The thread
// ---------------------------------------------------------------------------------------------------------------

static DWORD WINAPI UpdateThread(LPVOID parameter)
{
    UNREFERENCED_PARAMETER(parameter);

    // Signing in is the busiest moment the machine has. The check waits for it to pass, and the wait is on the
    // stop event so that unloading the engine does not have to sit through it.
    DWORD delay = ReadDword(L"UpdateDelaySeconds", 30);
    if (delay > 3600)
    {
        delay = 3600;
    }
    if (WaitForSingleObject(g_stopEvent, delay * 1000) != WAIT_TIMEOUT)
    {
        return 0;
    }

    wchar_t wszUrl[1024];
    if (!ReadString(L"UpdateURL", wszUrl, ARRAYSIZE(wszUrl)))
    {
        StringCchCopyW(wszUrl, ARRAYSIZE(wszUrl), SP_UPDATE_DEFAULT_URL);
    }

    char* body = Fetch(wszUrl);
    if (!body)
    {
        SP_LOG_INF(TAG, L"The update check did not get an answer; nothing is shown");
        return 0;
    }

    wchar_t wszTag[64] = { 0 };
    if (!ExtractField(body, "tag_name", wszTag, ARRAYSIZE(wszTag)))
    {
        free(body);
        SP_LOG_INF(TAG, L"The answer did not name a version");
        return 0;
    }

    wchar_t wszPage[512] = { 0 };
    if (ExtractField(body, "html_url", wszPage, ARRAYSIZE(wszPage)))
    {
        StringCchCopyW(g_releaseUrl, ARRAYSIZE(g_releaseUrl), wszPage);
    }
    free(body);

    StringCchCopyW(g_version, ARRAYSIZE(g_version), wszTag);

    if (CompareWithOurs(wszTag) <= 0)
    {
        SP_LOG_INF(TAG, L"Version %s is not newer than this build; nothing is shown", wszTag);
        return 0;
    }

    // A version the user has already been shown and acted on is not brought up again.
    wchar_t wszSkip[64] = { 0 };
    if (ReadString(L"UpdateSkip", wszSkip, ARRAYSIZE(wszSkip)) && _wcsicmp(wszSkip, wszTag) == 0)
    {
        SP_LOG_INF(TAG, L"Version %s has been mentioned already", wszTag);
        return 0;
    }

    if (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT)
    {
        ShowNotice();
    }
    return 0;
}

void SP_UpdateStart(void)
{
    if (!ReadDword(L"UpdateCheck", 1))
    {
        SP_LOG_DBG(TAG, L"The update check is turned off");
        return;
    }

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent)
    {
        return;
    }

    g_thread = CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
    if (!g_thread)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
    }
}

void SP_UpdateStop(void)
{
    if (g_stopEvent)
    {
        SetEvent(g_stopEvent);
    }
    if (g_window)
    {
        // The thread is inside its message loop; a message of its own is what gets it out.
        PostMessageW(g_window, WM_CLOSE, 0, 0);
    }
    if (g_thread)
    {
        if (WaitForSingleObject(g_thread, 5000) == WAIT_TIMEOUT)
        {
            SP_LOG_ERR(TAG, L"The update thread did not stop in time");
        }
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stopEvent)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
    }
}
