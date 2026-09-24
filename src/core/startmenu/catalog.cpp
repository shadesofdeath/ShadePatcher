//
// catalog.cpp - see catalog.h.
//
#include "catalog.h"

#define SECURITY_WIN32
#include <security.h>
#include <sddl.h>
#include <powrprof.h>
#include <shellapi.h>
#include <Shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <unordered_map>

#include "engine/log.h"
#include "filesearch.h"
#include "quick.h"

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

namespace sm {

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kTag[] = "custom-start-menu";

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

std::wstring TakeCoString(wchar_t* text)
{
    std::wstring result = text ? text : L"";
    CoTaskMemFree(text);
    return result;
}

bool EqualsNoCase(const std::wstring& a, const wchar_t* b)
{
    return CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
}

// Premultiplied BGRA from a 32-bit DIB section, as IShellItemImageFactory returns them. The shell's bitmaps are
// usually premultiplied already; a bitmap whose colour exceeds its alpha is not, and one with no alpha at all is
// opaque.
PixelsPtr PixelsFromBitmap(HBITMAP bitmap)
{
    DIBSECTION dib = {};
    if (!GetObjectW(bitmap, sizeof(dib), &dib) || dib.dsBm.bmBitsPixel != 32 || !dib.dsBm.bmBits ||
        dib.dsBm.bmWidth <= 0 || dib.dsBm.bmHeight <= 0)
    {
        return nullptr;
    }
    auto pixels = std::make_shared<Pixels>();
    UINT width = (UINT)dib.dsBm.bmWidth, height = (UINT)dib.dsBm.bmHeight;
    pixels->width = width;
    pixels->height = height;
    pixels->bgra.resize((size_t)width * height * 4);
    bool bottomUp = dib.dsBmih.biHeight > 0;
    const BYTE* src = static_cast<const BYTE*>(dib.dsBm.bmBits);
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* row = src + (size_t)(bottomUp ? height - 1 - y : y) * dib.dsBm.bmWidthBytes;
        memcpy(&pixels->bgra[(size_t)y * width * 4], row, (size_t)width * 4);
    }

    bool anyAlpha = false, straight = false;
    for (size_t i = 0; i < pixels->bgra.size(); i += 4)
    {
        BYTE a = pixels->bgra[i + 3];
        if (a)
        {
            anyAlpha = true;
        }
        if (pixels->bgra[i] > a || pixels->bgra[i + 1] > a || pixels->bgra[i + 2] > a)
        {
            straight = true;
        }
    }
    for (size_t i = 0; i < pixels->bgra.size(); i += 4)
    {
        BYTE* p = &pixels->bgra[i];
        if (!anyAlpha)
        {
            p[3] = 255;
        }
        else if (straight)
        {
            p[0] = (BYTE)(p[0] * p[3] / 255);
            p[1] = (BYTE)(p[1] * p[3] / 255);
            p[2] = (BYTE)(p[2] * p[3] / 255);
        }
    }
    return pixels;
}

} // namespace

PixelsPtr PixelsFromHBitmap(HBITMAP bitmap)
{
    return PixelsFromBitmap(bitmap);
}

PixelsPtr PixelsFromIcon(HICON icon, UINT px)
{
    if (!icon || !px)
    {
        return nullptr;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = (LONG)px;
    info.bmiHeader.biHeight = -(LONG)px;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    PixelsPtr pixels;
    if (bitmap)
    {
        HGDIOBJ old = SelectObject(dc, bitmap);
        DrawIconEx(dc, 0, 0, icon, (int)px, (int)px, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        pixels = PixelsFromBitmap(bitmap);
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    return pixels;
}

namespace {

PixelsPtr LoadAppIcon(PCIDLIST_ABSOLUTE pidl, UINT px)
{
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&factory))))
    {
        return nullptr;
    }
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage(SIZE{ (LONG)px, (LONG)px }, SIIGBF_ICONONLY, &bitmap)) || !bitmap)
    {
        return nullptr;
    }
    PixelsPtr pixels = PixelsFromBitmap(bitmap);
    DeleteObject(bitmap);
    return pixels;
}

PixelsPtr LoadImageFile(const wchar_t* path, UINT maxSide = 2048)
{
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
    {
        return nullptr;
    }
    UINT sourceW = 0, sourceH = 0;
    frame->GetSize(&sourceW, &sourceH);
    if (!sourceW || !sourceH)
    {
        return nullptr;
    }
    IWICBitmapSource* source = frame.Get();
    if (std::max(sourceW, sourceH) > maxSide)
    {
        double k = (double)maxSide / std::max(sourceW, sourceH);
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), std::max(1u, (UINT)(sourceW * k)), std::max(1u, (UINT)(sourceH * k)),
                                      WICBitmapInterpolationModeFant)))
        {
            return nullptr;
        }
        source = scaler.Get();
    }
    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
    {
        return nullptr;
    }
    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    if (!width || !height)
    {
        return nullptr;
    }
    auto pixels = std::make_shared<Pixels>();
    pixels->width = width;
    pixels->height = height;
    pixels->bgra.resize((size_t)width * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, (UINT)pixels->bgra.size(), pixels->bgra.data())))
    {
        return nullptr;
    }
    return pixels;
}

// ---------------------------------------------------------------------------------------------------------------
// Sorting and headings
// ---------------------------------------------------------------------------------------------------------------

bool SortsBefore(const App& a, const App& b)
{
    int order = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                                a.name.c_str(), (int)a.name.size(), b.name.c_str(), (int)b.name.size(),
                                nullptr, nullptr, 0);
    if (order != CSTR_EQUAL)
    {
        return order == CSTR_LESS_THAN;
    }
    return a.id < b.id;
}

// The locale's view of "same letter" for headings: case and accents ignored, except where the language treats
// the accented letter as a letter of its own (Turkish keeps C and \x00C7 apart but puts i under \x0130).
bool SameLetter(wchar_t a, wchar_t b)
{
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_LINGUISTIC_CASING,
                           &a, 1, &b, 1, nullptr, nullptr, 0) == CSTR_EQUAL;
}

wchar_t Upper(wchar_t c)
{
    wchar_t out[4] = {};
    if (LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, &c, 1, out, 4, nullptr,
                      nullptr, 0) == 1)
    {
        return out[0];
    }
    return c;
}

std::wstring HeadingFor(const std::wstring& name)
{
    if (name.empty() || IS_HIGH_SURROGATE(name[0]) || !IsCharAlphaW(name[0]))
    {
        return L"#";
    }
    wchar_t c = name[0];
    // Use the unaccented letter when the locale files the accented one under it (English "\x00C9" under "E").
    wchar_t folded[4] = {};
    if (FoldStringW(MAP_COMPOSITE, &c, 1, folded, 4) > 0 && folded[0] != c && SameLetter(folded[0], c))
    {
        c = folded[0];
    }
    return std::wstring(1, Upper(c));
}

void AssignHeadings(std::vector<App>& apps)
{
    for (size_t i = 0; i < apps.size(); ++i)
    {
        App& app = apps[i];
        if (i > 0 && !app.name.empty() && !apps[i - 1].name.empty() && apps[i - 1].heading != L"#" &&
            IsCharAlphaW(app.name[0]) && SameLetter(apps[i - 1].name[0], app.name[0]))
        {
            app.heading = apps[i - 1].heading;
        }
        else
        {
            app.heading = HeadingFor(app.name);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------------------------------------------

std::vector<App> EnumerateApps(volatile LONG* stop)
{
    std::vector<App> apps;
    ComPtr<IShellItem> folder;
    ComPtr<IEnumShellItems> items;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder))) ||
        FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items))))
    {
        SP_LOG_ERR(kTag, L"The Apps folder could not be enumerated");
        return apps;
    }
    ComPtr<IShellItem> item;
    while (!*stop && items->Next(1, &item, nullptr) == S_OK)
    {
        App app;
        wchar_t* text = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &text)))
        {
            app.name = TakeCoString(text);
        }
        text = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &text)))
        {
            app.id = TakeCoString(text);
        }
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (!app.name.empty() && !app.id.empty() && SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)))
        {
            app.pidl = std::make_shared<const IdList>(pidl);
            apps.push_back(std::move(app));
        }
        item.Reset();
    }
    std::sort(apps.begin(), apps.end(), SortsBefore);
    AssignHeadings(apps);
    return apps;
}

FileKind KindOf(const std::wstring& path)
{
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    static const struct { FileKind kind; const wchar_t* list; } kKinds[] =
    {
        { FileKind::Document, L".doc.docx.odt.rtf.txt.pdf.xls.xlsx.ods.csv.ppt.pptx.odp.md.epub." },
        { FileKind::Image,    L".jpg.jpeg.png.gif.bmp.webp.heic.heif.tif.tiff.svg.ico.raw.psd." },
        { FileKind::Code,     L".c.cc.cpp.cxx.h.hpp.cs.py.js.ts.tsx.jsx.java.kt.rs.go.rb.php.html.htm.css.json."
                              L"xml.yml.yaml.ps1.bat.cmd.sh.sql.lua.swift.reg.ini.toml." },
        { FileKind::Media,    L".mp3.wav.flac.aac.ogg.m4a.wma.mp4.mkv.avi.mov.wmv.webm." },
        { FileKind::Archive,  L".zip.rar.7z.tar.gz.bz2.xz.cab.iso.msi." },
    };
    if (!ext || !*ext || wcslen(ext) > 12)
    {
        return FileKind::Other;
    }
    wchar_t probe[16];
    swprintf_s(probe, L"%s.", ext);
    CharLowerW(probe);
    for (const auto& k : kKinds)
    {
        if (wcsstr(k.list, probe))
        {
            return k.kind;
        }
    }
    return FileKind::Other;
}

bool RecentTrackingAllowed()
{
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                 L"Start_TrackDocs", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

std::vector<RecentFile> LoadRecent(int maxCount, bool ignoreTrackingSetting)
{
    std::vector<RecentFile> result;
    if (!ignoreTrackingSetting && !RecentTrackingAllowed())
    {
        return result;
    }
    wchar_t* folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DEFAULT, nullptr, &folder)))
    {
        return result;
    }
    std::wstring dir = TakeCoString(folder);

    struct Link { std::wstring path; ULONGLONG time; };
    std::vector<Link> links;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW((dir + L"\\*.lnk").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                ULONGLONG time = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) |
                                 data.ftLastWriteTime.dwLowDateTime;
                links.push_back({ dir + L"\\" + data.cFileName, time });
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) { return a.time > b.time; });

    int examined = 0;
    for (const Link& link : links)
    {
        if ((int)result.size() >= maxCount || ++examined > maxCount * 5)
        {
            break;
        }
        ComPtr<IShellLinkW> shellLink;
        ComPtr<IPersistFile> file;
        wchar_t target[MAX_PATH] = L"";
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))) ||
            FAILED(shellLink.As(&file)) || FAILED(file->Load(link.path.c_str(), STGM_READ)) ||
            shellLink->GetPath(target, MAX_PATH, nullptr, 0) != S_OK || !target[0])
        {
            continue;
        }
        // Network paths are skipped rather than probed: a disconnected share would stall the loader.
        if (PathIsNetworkPathW(target))
        {
            continue;
        }
        DWORD attributes = GetFileAttributesW(target);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            continue;
        }
        RecentFile recent;
        recent.path = target;
        recent.name = PathFindFileNameW(target);
        recent.kind = KindOf(recent.path);
        result.push_back(std::move(recent));
    }
    return result;
}

UserInfo LoadUser()
{
    UserInfo user;
    wchar_t name[256] = L"";
    ULONG cch = ARRAYSIZE(name);
    if (GetUserNameExW(NameDisplay, name, &cch) && name[0])
    {
        user.name = name;
    }
    else
    {
        DWORD size = ARRAYSIZE(name);
        if (GetUserNameW(name, &size))
        {
            user.name = name;
        }
    }

    // The account picture Windows shows on the sign-in screen, per user SID under HKLM.
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        BYTE buffer[256];
        DWORD size = 0;
        wchar_t* sid = nullptr;
        if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid))
        {
            std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\";
            key += sid;
            LocalFree(sid);
            const wchar_t* values[] = { L"Image192", L"Image208", L"Image240", L"Image96", L"Image64", L"Image448" };
            for (const wchar_t* value : values)
            {
                wchar_t path[MAX_PATH] = L"";
                DWORD bytes = sizeof(path);
                if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_SZ, nullptr, path, &bytes) ==
                        ERROR_SUCCESS &&
                    path[0])
                {
                    user.picture = LoadImageFile(path);
                    if (user.picture)
                    {
                        break;
                    }
                }
            }
        }
        CloseHandle(token);
    }
    return user;
}

std::vector<ShortcutItem> LoadShortcuts(unsigned which, UINT px)
{
    std::vector<ShortcutItem> result;
    for (int i = 0; i < (int)Shortcut::Count; ++i)
    {
        Shortcut s = (Shortcut)i;
        if (!(which & ShortcutBit(s)))
        {
            continue;
        }
        ComPtr<IShellItem> item;
        HRESULT hr = E_FAIL;
        switch (s)
        {
        case Shortcut::Explorer:
            hr = SHCreateItemFromParsingName(L"shell:AppsFolder\\Microsoft.Windows.Explorer", nullptr,
                                             IID_PPV_ARGS(&item));
            break;
        case Shortcut::Settings:
            hr = SHCreateItemFromParsingName(
                L"shell:AppsFolder\\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel",
                nullptr, IID_PPV_ARGS(&item));
            break;
        default:
        {
            static const KNOWNFOLDERID* const kFolders[] = { nullptr, &FOLDERID_Documents, &FOLDERID_Downloads,
                                                             &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos,
                                                             &FOLDERID_Profile };
            hr = SHGetKnownFolderItem(*kFolders[i], KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item));
            break;
        }
        }
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(hr) || FAILED(SHGetIDListFromObject(item.Get(), &pidl)))
        {
            continue;
        }
        ShortcutItem shortcut;
        shortcut.which = s;
        shortcut.pidl = std::make_shared<const IdList>(pidl);
        wchar_t* name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
        {
            shortcut.name = TakeCoString(name);
        }
        shortcut.icon = LoadAppIcon(pidl, px);
        result.push_back(std::move(shortcut));
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------------------------------------------

bool Loader::Start(HWND notify, UINT message)
{
    m_notify = notify;
    m_message = message;
    m_stop = 0;
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_thread = m_wake ? CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr) : nullptr;
    return m_thread != nullptr;
}

void Loader::Stop()
{
    if (m_thread)
    {
        InterlockedExchange(&m_stop, 1);
        SetEvent(m_wake);
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_wake)
    {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
}

void Loader::RequestApps(UINT iconPx, std::vector<std::wstring> firstIds)
{
    AcquireSRWLockExclusive(&m_lock);
    m_wantApps = true;
    m_iconPx = iconPx;
    m_firstIds = std::move(firstIds);
    ReleaseSRWLockExclusive(&m_lock);
    SetEvent(m_wake);
}

void Loader::RequestRecent(int maxCount, bool ignoreTrackingSetting)
{
    AcquireSRWLockExclusive(&m_lock);
    m_wantRecent = true;
    m_recentMax = maxCount;
    m_recentForce = ignoreTrackingSetting;
    ReleaseSRWLockExclusive(&m_lock);
    SetEvent(m_wake);
}

void Loader::RequestShortcuts(unsigned which, UINT iconPx)
{
    AcquireSRWLockExclusive(&m_lock);
    m_wantShortcuts = true;
    m_shortcuts = which;
    m_shortcutPx = iconPx;
    ReleaseSRWLockExclusive(&m_lock);
    SetEvent(m_wake);
}

void Loader::RequestUser()
{
    AcquireSRWLockExclusive(&m_lock);
    m_wantUser = true;
    ReleaseSRWLockExclusive(&m_lock);
    SetEvent(m_wake);
}

DWORD WINAPI Loader::ThreadProc(void* param)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    static_cast<Loader*>(param)->Run();
    if (SUCCEEDED(hr))
    {
        CoUninitialize();
    }
    return 0;
}

void Loader::Run()
{
    // Icons already decoded, by app id, so a refresh only decodes what is new. Dropped when the size changes.
    std::unordered_map<std::wstring, PixelsPtr> iconCache;
    UINT cachePx = 0;

    auto post = [this](LoadResult kind, void* object) {
        if (!PostMessageW(m_notify, m_message, (WPARAM)kind, (LPARAM)object))
        {
            FreeLoadResult((WPARAM)kind, (LPARAM)object);
        }
    };

    while (!m_stop)
    {
        WaitForSingleObject(m_wake, INFINITE);
        if (m_stop)
        {
            break;
        }
        AcquireSRWLockExclusive(&m_lock);
        bool apps = m_wantApps, recent = m_wantRecent, user = m_wantUser, shortcuts = m_wantShortcuts;
        m_wantApps = m_wantRecent = m_wantUser = m_wantShortcuts = false;
        unsigned shortcutSet = m_shortcuts;
        UINT shortcutPx = m_shortcutPx;
        UINT iconPx = m_iconPx;
        int recentMax = m_recentMax;
        bool recentForce = m_recentForce;
        std::vector<std::wstring> firstIds = std::move(m_firstIds);
        m_firstIds.clear();
        ReleaseSRWLockExclusive(&m_lock);

        if (user)
        {
            post(LoadResult::User, new UserInfo(LoadUser()));
        }
        if (shortcuts)
        {
            post(LoadResult::Shortcuts, new std::vector<ShortcutItem>(LoadShortcuts(shortcutSet, shortcutPx)));
        }
        if (recent)
        {
            post(LoadResult::Recent, new std::vector<RecentFile>(LoadRecent(recentMax, recentForce)));
        }
        if (apps && !m_stop)
        {
            ULONGLONG start = GetTickCount64();
            AppList list;
            list.apps = EnumerateApps(&m_stop);
            list.iconPx = iconPx;
            if (cachePx != iconPx)
            {
                iconCache.clear();
                cachePx = iconPx;
            }
            bool missing = false;
            for (App& app : list.apps)
            {
                auto it = iconCache.find(app.id);
                if (it != iconCache.end())
                {
                    app.icon = it->second;
                }
                else
                {
                    missing = true;
                }
            }
            list.iconsComplete = !missing;
            post(LoadResult::Apps, new AppList(list));
            SP_LOG_INF(kTag, L"%u apps listed in %llu ms", (unsigned)list.apps.size(), GetTickCount64() - start);

            if (missing && !m_stop)
            {
                // The pinned apps first, then the rest in list order.
                std::vector<size_t> order;
                order.reserve(list.apps.size());
                for (const std::wstring& id : firstIds)
                {
                    for (size_t i = 0; i < list.apps.size(); ++i)
                    {
                        if (!list.apps[i].icon && EqualsNoCase(list.apps[i].id, id.c_str()))
                        {
                            order.push_back(i);
                        }
                    }
                }
                size_t pinnedCount = order.size();
                for (size_t i = 0; i < list.apps.size(); ++i)
                {
                    if (!list.apps[i].icon && std::find(order.begin(), order.end(), i) == order.end())
                    {
                        order.push_back(i);
                    }
                }
                size_t loaded = 0;
                for (size_t index : order)
                {
                    if (m_stop)
                    {
                        break;
                    }
                    App& app = list.apps[index];
                    app.icon = LoadAppIcon(app.pidl->value, iconPx);
                    iconCache[app.id] = app.icon;
                    // Post the pinned apps' icons as soon as they are in, so the default grid fills in first.
                    if (++loaded == pinnedCount && loaded < order.size())
                    {
                        post(LoadResult::Apps, new AppList(list));
                    }
                }
                if (!m_stop)
                {
                    list.iconsComplete = true;
                    post(LoadResult::Apps, new AppList(std::move(list)));
                    SP_LOG_INF(kTag, L"Icons ready after %llu ms", GetTickCount64() - start);
                }
            }
        }
    }
}

void FreeLoadResult(WPARAM kind, LPARAM object)
{
    switch ((LoadResult)kind)
    {
    case LoadResult::Apps:   delete reinterpret_cast<AppList*>(object); break;
    case LoadResult::Recent: delete reinterpret_cast<std::vector<RecentFile>*>(object); break;
    case LoadResult::User:   delete reinterpret_cast<UserInfo*>(object); break;
    case LoadResult::Shortcuts: delete reinterpret_cast<std::vector<ShortcutItem>*>(object); break;
    case LoadResult::Files: delete reinterpret_cast<FileResults*>(object); break;
    case LoadResult::Quick: delete reinterpret_cast<QuickState*>(object); break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// PinStore
// ---------------------------------------------------------------------------------------------------------------

namespace {

std::wstring PinFilePath()
{
    wchar_t* local = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)))
    {
        path = TakeCoString(local) + L"\\ShadePatcher";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\pinned.json";
    }
    return path;
}

// A JSON array of strings, the only shape pinned.json ever has. Written by Save; anything else is rejected.
bool ParseStringArray(const std::wstring& text, std::vector<std::wstring>* out)
{
    size_t i = 0;
    auto skip = [&] { while (i < text.size() && iswspace(text[i])) ++i; };
    skip();
    if (i >= text.size() || text[i++] != L'[')
    {
        return false;
    }
    skip();
    if (i < text.size() && text[i] == L']')
    {
        return true;
    }
    while (i < text.size())
    {
        skip();
        if (text[i++] != L'"')
        {
            return false;
        }
        std::wstring value;
        while (i < text.size() && text[i] != L'"')
        {
            wchar_t c = text[i++];
            if (c == L'\\' && i < text.size())
            {
                wchar_t e = text[i++];
                switch (e)
                {
                case L'n': c = L'\n'; break;
                case L't': c = L'\t'; break;
                case L'u':
                    if (i + 4 > text.size())
                    {
                        return false;
                    }
                    c = (wchar_t)wcstoul(text.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    break;
                default: c = e; break;
                }
            }
            value.push_back(c);
        }
        if (i >= text.size())
        {
            return false;
        }
        ++i;   // closing quote
        out->push_back(std::move(value));
        skip();
        if (i < text.size() && text[i] == L',')
        {
            ++i;
            continue;
        }
        skip();
        return i < text.size() && text[i] == L']';
    }
    return false;
}

} // namespace

bool PinStore::Load()
{
    m_ids.clear();
    std::wstring path = PinFilePath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    std::string bytes;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 1024 * 1024)
    {
        bytes.resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, nullptr))
        {
            read = 0;
        }
        bytes.resize(read);
    }
    CloseHandle(file);

    size_t skip = bytes.size() >= 3 && (BYTE)bytes[0] == 0xEF && (BYTE)bytes[1] == 0xBB && (BYTE)bytes[2] == 0xBF ? 3 : 0;
    int cch = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, (int)(bytes.size() - skip), nullptr, 0);
    std::wstring text(cch, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, (int)(bytes.size() - skip), text.data(), cch);
    if (!ParseStringArray(text, &m_ids))
    {
        SP_LOG_ERR(kTag, L"pinned.json is not a list of strings; starting from the defaults");
        m_ids.clear();
        return false;
    }
    return true;
}

bool PinStore::Save() const
{
    std::wstring text = L"[\n";
    for (size_t i = 0; i < m_ids.size(); ++i)
    {
        text += L"  \"";
        for (wchar_t c : m_ids[i])
        {
            if (c == L'"' || c == L'\\')
            {
                text.push_back(L'\\');
                text.push_back(c);
            }
            else if (c < 0x20)
            {
                wchar_t escape[8];
                swprintf_s(escape, L"\\u%04x", c);
                text += escape;
            }
            else
            {
                text.push_back(c);
            }
        }
        text += i + 1 < m_ids.size() ? L"\",\n" : L"\"\n";
    }
    text += L"]\n";

    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), bytes, nullptr, nullptr);

    // Write beside the file and swap it in, so a crash mid-write never leaves a truncated list.
    std::wstring path = PinFilePath();
    std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr) && written == utf8.size();
    CloseHandle(file);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void PinStore::Delete()
{
    std::wstring path = PinFilePath();
    if (!path.empty())
    {
        DeleteFileW(path.c_str());
    }
}

bool PinStore::IsPinned(const std::wstring& id) const
{
    for (const std::wstring& pinned : m_ids)
    {
        if (EqualsNoCase(pinned, id.c_str()))
        {
            return true;
        }
    }
    return false;
}

void PinStore::Pin(const std::wstring& id)
{
    if (!IsPinned(id))
    {
        m_ids.push_back(id);
    }
}

void PinStore::Unpin(const std::wstring& id)
{
    m_ids.erase(std::remove_if(m_ids.begin(), m_ids.end(),
                               [&](const std::wstring& pinned) { return EqualsNoCase(pinned, id.c_str()); }),
                m_ids.end());
}

void PinStore::MoveToFront(const std::wstring& id)
{
    Unpin(id);
    m_ids.insert(m_ids.begin(), id);
}

void PinStore::MoveTo(const std::wstring& id, size_t index)
{
    Unpin(id);
    m_ids.insert(m_ids.begin() + std::min(index, m_ids.size()), id);
}

// ---------------------------------------------------------------------------------------------------------------
// UsageStore
// ---------------------------------------------------------------------------------------------------------------

namespace {

std::wstring UsageFilePath()
{
    std::wstring path = PinFilePath();   // ...\ShadePatcher\pinned.json
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1) + L"usage.txt";
}

} // namespace

void UsageStore::Load()
{
    m_loaded = true;
    m_counts.clear();
    FILE* file = nullptr;
    if (_wfopen_s(&file, UsageFilePath().c_str(), L"rt, ccs=UTF-8") != 0 || !file)
    {
        return;
    }
    wchar_t line[1024];
    while (fgetws(line, ARRAYSIZE(line), file))
    {
        wchar_t* tab = wcschr(line, L'\t');
        if (!tab)
        {
            continue;
        }
        *tab = 0;
        std::wstring id = tab + 1;
        while (!id.empty() && (id.back() == L'\n' || id.back() == L'\r'))
        {
            id.pop_back();
        }
        unsigned count = wcstoul(line, nullptr, 10);
        if (!id.empty() && count)
        {
            m_counts.emplace_back(id, count);
        }
    }
    fclose(file);
}

void UsageStore::Save() const
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, UsageFilePath().c_str(), L"wt, ccs=UTF-8") != 0 || !file)
    {
        return;
    }
    for (const auto& entry : m_counts)
    {
        fwprintf(file, L"%u\t%s\n", entry.second, entry.first.c_str());
    }
    fclose(file);
}

void UsageStore::Record(const std::wstring& id)
{
    if (!m_loaded)
    {
        Load();
    }
    auto it = std::find_if(m_counts.begin(), m_counts.end(),
                           [&](const auto& entry) { return EqualsNoCase(entry.first, id.c_str()); });
    if (it == m_counts.end())
    {
        m_counts.emplace_back(id, 1);
    }
    else
    {
        ++it->second;
    }
    // Keep the file small: the 200 most used.
    std::stable_sort(m_counts.begin(), m_counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (m_counts.size() > 200)
    {
        m_counts.resize(200);
    }
    Save();
}

std::vector<std::wstring> UsageStore::Top(size_t count, const PinStore& exclude) const
{
    std::vector<std::wstring> result;
    for (const auto& entry : m_counts)   // kept sorted by Record; Load keeps the file's order, which is sorted
    {
        if (result.size() >= count)
        {
            break;
        }
        if (!exclude.IsPinned(entry.first))
        {
            result.push_back(entry.first);
        }
    }
    return result;
}

void PinStore::SeedDefaults(const AppList& list)
{
    static const wchar_t* const kDefaults[] =
    {
        L"MSEdge",
        L"Chrome",
        L"308046B0AF4A39CB",                                          // Firefox
        L"Microsoft.Windows.Explorer",
        L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App",
        L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel",
        L"Microsoft.WindowsStore_8wekyb3d8bbwe!App",
        L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App",
        L"Microsoft.Windows.Photos_8wekyb3d8bbwe!App",
        L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App",
        L"Microsoft.Paint_8wekyb3d8bbwe!App",
        L"Microsoft.ScreenSketch_8wekyb3d8bbwe!App",
        L"Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows",
        L"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic",
        L"Microsoft.WindowsAlarms_8wekyb3d8bbwe!App",
        L"Microsoft.BingWeather_8wekyb3d8bbwe!App",
        L"Microsoft.Todos_8wekyb3d8bbwe!App",
        L"Microsoft.WindowsCamera_8wekyb3d8bbwe!App",
        L"Microsoft.Windows.ControlPanel",
        L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\Taskmgr.exe",
        L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\cmd.exe",
    };
    m_ids.clear();
    for (const wchar_t* id : kDefaults)
    {
        if (m_ids.size() >= 15)
        {
            break;
        }
        for (const App& app : list.apps)
        {
            if (EqualsNoCase(app.id, id))
            {
                m_ids.push_back(app.id);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------------------------------------------

std::vector<int> SearchApps(const AppList& list, const std::wstring& query)
{
    // Rank 0: the name starts with the query. 1: a word in it does. 2: it contains the query. 3-5: the same, with
    // accents ignored as well ("cizim" still finds "\x00C7izim").
    std::vector<int> ranks[6];
    const DWORD flags[2] = { LINGUISTIC_IGNORECASE, LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC };
    for (int i = 0; i < (int)list.apps.size(); ++i)
    {
        const std::wstring& name = list.apps[i].name;
        for (int pass = 0; pass < 2; ++pass)
        {
            int at = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | flags[pass], name.c_str(),
                                     (int)name.size(), query.c_str(), (int)query.size(), nullptr, nullptr, nullptr, 0);
            if (at < 0)
            {
                continue;
            }
            int rank = 2;
            if (at == 0)
            {
                rank = 0;
            }
            else
            {
                // Any later occurrence at a word start also counts as rank 1.
                for (int pos = at; pos >= 0 && pos < (int)name.size();)
                {
                    wchar_t before = name[pos - 1];
                    if (iswspace(before) || iswpunct(before))
                    {
                        rank = 1;
                        break;
                    }
                    int next = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | flags[pass],
                                               name.c_str() + pos + 1, (int)name.size() - pos - 1, query.c_str(),
                                               (int)query.size(), nullptr, nullptr, nullptr, 0);
                    pos = next < 0 ? -1 : pos + 1 + next;
                }
            }
            ranks[pass * 3 + rank].push_back(i);
            break;
        }
    }
    std::vector<int> result;
    for (const auto& rank : ranks)
    {
        result.insert(result.end(), rank.begin(), rank.end());
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------------------------

bool LaunchApp(const App& app, bool asAdmin)
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_ASYNCOK;
    info.lpVerb = asAdmin ? L"runas" : nullptr;
    info.lpIDList = app.pidl->value;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
    {
        SP_LOG_ERR(kTag, L"Launching %s failed (%lu)", app.id.c_str(), GetLastError());
        return false;
    }
    return true;
}

bool OpenIdList(PCIDLIST_ABSOLUTE pidl)
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_ASYNCOK;
    info.lpIDList = const_cast<LPITEMIDLIST>(pidl);
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

bool OpenPath(const std::wstring& path)
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_ASYNCOK;
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

bool OpenAccountSettings()
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_ASYNCOK;
    info.lpFile = L"ms-settings:yourinfo";
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

namespace {

bool EnableShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        return false;
    }
    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

// "Fast startup" in Power Options: when it is on, Windows' own Shut down is a hybrid shutdown.
bool FastStartupEnabled()
{
    DWORD value = 0, size = sizeof(value);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
                 L"HiberbootEnabled", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

DWORD WINAPI SuspendThread(void* hibernate)
{
    // Let the menu finish closing before the machine goes down.
    Sleep(300);
    SetSuspendState(hibernate != nullptr, FALSE, FALSE);
    return 0;
}

} // namespace

void RunPowerAction(PowerAction action)
{
    switch (action)
    {
    case PowerAction::Lock:
        LockWorkStation();
        break;
    case PowerAction::SignOut:
        ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        break;
    case PowerAction::Sleep:
    case PowerAction::Hibernate:
        if (HANDLE thread = CreateThread(nullptr, 0, SuspendThread,
                                         action == PowerAction::Hibernate ? (void*)1 : nullptr, 0, nullptr))
        {
            CloseHandle(thread);
        }
        break;
    case PowerAction::AdvancedStartup:
    case PowerAction::Firmware:
    {
        // shutdown.exe knows both; they need administrator rights, so Windows asks.
        SHELLEXECUTEINFOW info = { sizeof(info) };
        info.fMask = SEE_MASK_ASYNCOK;
        info.lpVerb = L"runas";
        info.lpFile = L"shutdown.exe";
        info.lpParameters = action == PowerAction::Firmware ? L"/r /fw /t 0" : L"/r /o /t 0";
        info.nShow = SW_HIDE;
        ShellExecuteExW(&info);
        break;
    }
    case PowerAction::UpdateRestart:
    case PowerAction::UpdateShutDown:
    case PowerAction::Restart:
    case PowerAction::ShutDown:
    {
        EnableShutdownPrivilege();
        DWORD reason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
        bool restart = action == PowerAction::Restart || action == PowerAction::UpdateRestart;
        DWORD flags = restart ? SHUTDOWN_RESTART : SHUTDOWN_POWEROFF;
        if (action == PowerAction::UpdateRestart || action == PowerAction::UpdateShutDown)
        {
            flags |= SHUTDOWN_INSTALL_UPDATES;
        }
        if (!restart && FastStartupEnabled())
        {
            flags |= SHUTDOWN_HYBRID;
        }
        DWORD error = InitiateShutdownW(nullptr, nullptr, 0, flags, reason);
        if (error != ERROR_SUCCESS && (flags & SHUTDOWN_HYBRID))
        {
            error = InitiateShutdownW(nullptr, nullptr, 0, flags & ~SHUTDOWN_HYBRID, reason);
        }
        if (error != ERROR_SUCCESS)
        {
            SP_LOG_ERR(kTag, L"InitiateShutdown failed (%lu)", error);
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------------------------------------------

namespace {

// The shell menu on screen, for HandleContextMenuMessage. UI thread only.
ComPtr<IContextMenu2> g_menu2;
ComPtr<IContextMenu3> g_menu3;

void RemoveStartPinEntries(HMENU menu, IContextMenu* contextMenu)
{
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i)
    {
        UINT id = GetMenuItemID(menu, i);
        if (id == (UINT)-1 || id < kShellFirstId)
        {
            continue;
        }
        wchar_t verb[64] = L"";
        if (SUCCEEDED(contextMenu->GetCommandString(id - kShellFirstId, GCS_VERBW, nullptr,
                                                    reinterpret_cast<char*>(verb), ARRAYSIZE(verb))) &&
            (_wcsicmp(verb, L"pintostartscreen") == 0 || _wcsicmp(verb, L"unpinfromstartscreen") == 0))
        {
            DeleteMenu(menu, i, MF_BYPOSITION);
        }
    }
    // Collapse the separators the removal may have left doubled or at an edge.
    bool lastWasSeparator = true;
    for (int i = 0; i < GetMenuItemCount(menu);)
    {
        MENUITEMINFOW info = { sizeof(info), MIIM_FTYPE };
        GetMenuItemInfoW(menu, i, TRUE, &info);
        bool separator = (info.fType & MFT_SEPARATOR) != 0;
        if (separator && lastWasSeparator)
        {
            DeleteMenu(menu, i, MF_BYPOSITION);
            continue;
        }
        lastWasSeparator = separator;
        ++i;
    }
    int count = GetMenuItemCount(menu);
    if (count > 0)
    {
        MENUITEMINFOW info = { sizeof(info), MIIM_FTYPE };
        GetMenuItemInfoW(menu, count - 1, TRUE, &info);
        if (info.fType & MFT_SEPARATOR)
        {
            DeleteMenu(menu, count - 1, MF_BYPOSITION);
        }
    }
}

} // namespace

int ShowItemContextMenu(HWND owner, POINT screen, PCIDLIST_ABSOLUTE pidl, const std::vector<MenuExtra>& extras)
{
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    ComPtr<IContextMenu> contextMenu;
    if (pidl)
    {
        if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child)))
        {
            parent->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr, &contextMenu);
        }
    }

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return -1;
    }
    UINT position = 0;
    for (const MenuExtra& extra : extras)
    {
        if (extra.text)
        {
            InsertMenuW(menu, position++, MF_BYPOSITION | MF_STRING | extra.flags, extra.id, extra.text);
        }
        else
        {
            InsertMenuW(menu, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        }
    }
    if (contextMenu)
    {
        if (position)
        {
            InsertMenuW(menu, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        }
        if (SUCCEEDED(contextMenu->QueryContextMenu(menu, position, kShellFirstId, 0x7FFF, CMF_NORMAL)))
        {
            RemoveStartPinEntries(menu, contextMenu.Get());
        }
        contextMenu.As(&g_menu2);
        contextMenu.As(&g_menu3);
    }

    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, owner, nullptr);
    g_menu2.Reset();
    g_menu3.Reset();

    int result = -1;
    if (command && command < kShellFirstId)
    {
        result = (int)command;
    }
    else if (command >= kShellFirstId && contextMenu)
    {
        CMINVOKECOMMANDINFOEX invoke = { sizeof(invoke) };
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
        invoke.hwnd = owner;
        invoke.lpVerb = MAKEINTRESOURCEA(command - kShellFirstId);
        invoke.lpVerbW = MAKEINTRESOURCEW(command - kShellFirstId);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screen;
        if (GetKeyState(VK_CONTROL) < 0) invoke.fMask |= CMIC_MASK_CONTROL_DOWN;
        if (GetKeyState(VK_SHIFT) < 0) invoke.fMask |= CMIC_MASK_SHIFT_DOWN;
        HRESULT hr = contextMenu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&invoke));
        if (FAILED(hr))
        {
            SP_LOG_ERR(kTag, L"Context menu command %u failed (0x%08X)", command - kShellFirstId, hr);
        }
        result = 0;
    }
    DestroyMenu(menu);
    return result;
}

PixelsPtr LoadImagePixels(const std::wstring& path, UINT maxSide)
{
    return path.empty() ? nullptr : LoadImageFile(path.c_str(), maxSide);
}

std::vector<JumpItem> RecentItemsOf(const std::wstring& appId, size_t max)
{
    std::vector<JumpItem> items;
    ComPtr<IApplicationDocumentLists> lists;
    ComPtr<IObjectArray> array;
    if (appId.empty() ||
        FAILED(CoCreateInstance(CLSID_ApplicationDocumentLists, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&lists))) ||
        FAILED(lists->SetAppID(appId.c_str())) ||
        FAILED(lists->GetList(ADLT_RECENT, (UINT)max, IID_PPV_ARGS(&array))))
    {
        return items;
    }
    UINT count = 0;
    array->GetCount(&count);
    for (UINT i = 0; i < count && items.size() < max; ++i)
    {
        ComPtr<IShellItem> item;
        PIDLIST_ABSOLUTE pidl = nullptr;
        wchar_t* name = nullptr;
        if (FAILED(array->GetAt(i, IID_PPV_ARGS(&item))) || FAILED(SHGetIDListFromObject(item.Get(), &pidl)))
        {
            continue;   // a task (IShellLink) rather than a document
        }
        JumpItem jump;
        jump.pidl = std::make_shared<const IdList>(pidl);
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
        {
            jump.name = TakeCoString(name);
        }
        if (jump.name.size() > 60)
        {
            jump.name = jump.name.substr(0, 57) + L"...";
        }
        // "&" would make the next letter a menu accelerator.
        for (size_t at = jump.name.find(L'&'); at != std::wstring::npos; at = jump.name.find(L'&', at + 2))
        {
            jump.name.insert(at, 1, L'&');
        }
        items.push_back(std::move(jump));
    }
    return items;
}

// ---------------------------------------------------------------------------------------------------------------
// LineStore
// ---------------------------------------------------------------------------------------------------------------

namespace {

std::wstring StoreFilePath(const wchar_t* fileName)
{
    std::wstring path = PinFilePath();
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1) + fileName;
}

} // namespace

void LineStore::Load()
{
    if (m_loaded)
    {
        return;
    }
    m_loaded = true;
    FILE* file = nullptr;
    if (_wfopen_s(&file, StoreFilePath(m_fileName).c_str(), L"rt, ccs=UTF-8") != 0 || !file)
    {
        return;
    }
    wchar_t line[1024];
    while (fgetws(line, ARRAYSIZE(line), file))
    {
        std::wstring text = line;
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
        {
            text.pop_back();
        }
        if (!text.empty())
        {
            m_lines.push_back(std::move(text));
        }
    }
    fclose(file);
}

void LineStore::Save() const
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, StoreFilePath(m_fileName).c_str(), L"wt, ccs=UTF-8") != 0 || !file)
    {
        return;
    }
    for (const std::wstring& line : m_lines)
    {
        fwprintf(file, L"%s\n", line.c_str());
    }
    fclose(file);
}

const std::vector<std::wstring>& LineStore::Lines()
{
    Load();
    return m_lines;
}

bool LineStore::Contains(const std::wstring& line)
{
    Load();
    for (const std::wstring& existing : m_lines)
    {
        if (EqualsNoCase(existing, line.c_str()))
        {
            return true;
        }
    }
    return false;
}

void LineStore::Add(const std::wstring& line, size_t keep)
{
    Load();
    std::wstring clean;
    for (wchar_t c : line)
    {
        clean.push_back(c < 0x20 ? L' ' : c);
    }
    m_lines.erase(std::remove_if(m_lines.begin(), m_lines.end(),
                                 [&](const std::wstring& existing) { return EqualsNoCase(existing, clean.c_str()); }),
                  m_lines.end());
    m_lines.insert(m_lines.begin(), clean);
    if (m_lines.size() > keep)
    {
        m_lines.resize(keep);
    }
    Save();
}

void LineStore::Remove(const std::wstring& line)
{
    Load();
    m_lines.erase(std::remove_if(m_lines.begin(), m_lines.end(),
                                 [&](const std::wstring& existing) { return EqualsNoCase(existing, line.c_str()); }),
                  m_lines.end());
    Save();
}

void LineStore::Clear()
{
    m_lines.clear();
    m_loaded = true;
    Save();
}

void LineStore::Delete(const wchar_t* fileName)
{
    DeleteFileW(StoreFilePath(fileName).c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// NewApps
// ---------------------------------------------------------------------------------------------------------------

namespace {

std::wstring KnownFilePath()
{
    std::wstring path = PinFilePath();
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1) + L"known.txt";
}

ULONGLONG NowFileTime()
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
}

std::wstring Lower(const std::wstring& text)
{
    std::wstring lower = text;
    CharLowerBuffW(lower.data(), (DWORD)lower.size());
    return lower;
}

constexpr ULONGLONG kWeek = 7ull * 24 * 3600 * 10000000;

} // namespace

void NewApps::Update(const AppList& list)
{
    bool baseline = false;
    if (!m_loaded)
    {
        m_loaded = true;
        FILE* file = nullptr;
        if (_wfopen_s(&file, KnownFilePath().c_str(), L"rt, ccs=UTF-8") == 0 && file)
        {
            wchar_t line[1024];
            while (fgetws(line, ARRAYSIZE(line), file))
            {
                wchar_t* tab = wcschr(line, L'\t');
                if (!tab)
                {
                    continue;
                }
                *tab = 0;
                std::wstring id = tab + 1;
                while (!id.empty() && (id.back() == L'\n' || id.back() == L'\r'))
                {
                    id.pop_back();
                }
                m_seen[Lower(id)] = _wcstoui64(line, nullptr, 10);
            }
            fclose(file);
        }
        else
        {
            baseline = true;   // first run: everything installed now is simply known
        }
    }
    bool changed = baseline;
    ULONGLONG now = NowFileTime();
    for (const App& app : list.apps)
    {
        std::wstring key = Lower(app.id);
        if (!m_seen.count(key))
        {
            m_seen[key] = baseline ? 0 : now;
            changed = true;
        }
    }
    if (changed)
    {
        Save();
    }
}

bool NewApps::IsNew(const std::wstring& id) const
{
    auto it = m_seen.find(Lower(id));
    return it != m_seen.end() && it->second && NowFileTime() - it->second < kWeek;
}

bool NewApps::Any() const
{
    ULONGLONG now = NowFileTime();
    for (const auto& entry : m_seen)
    {
        if (entry.second && now - entry.second < kWeek)
        {
            return true;
        }
    }
    return false;
}

void NewApps::Clear(const std::wstring& id)
{
    auto it = m_seen.find(Lower(id));
    if (it != m_seen.end() && it->second)
    {
        it->second = 0;
        Save();
    }
}

void NewApps::Save() const
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, KnownFilePath().c_str(), L"wt, ccs=UTF-8") != 0 || !file)
    {
        return;
    }
    for (const auto& entry : m_seen)
    {
        fwprintf(file, L"%llu\t%s\n", entry.second, entry.first.c_str());
    }
    fclose(file);
}

bool UpdatesPending()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired", 0,
                      KEY_READ, &key) == ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return true;
    }
    return false;
}

bool HandleContextMenuMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    if (g_menu3)
    {
        return SUCCEEDED(g_menu3->HandleMenuMsg2(message, wParam, lParam, result));
    }
    if (g_menu2 && message != WM_MENUCHAR)
    {
        *result = 0;
        return SUCCEEDED(g_menu2->HandleMenuMsg(message, wParam, lParam));
    }
    return false;
}

} // namespace sm
