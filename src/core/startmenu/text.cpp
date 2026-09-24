//
// text.cpp - see text.h.
//
// Non-ASCII characters are written as \x escapes: the sources are compiled in the system code page.
//
#include "text.h"
#include "fontres.h"

#include <config.h>

#include <atomic>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace sm {

// ---------------------------------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------------------------------

namespace {

const wchar_t* const kEnglish[] =
{
    L"Search",
    L"Pinned",
    L"All apps",
    L"Best matches",
    L"All \x203A",
    L"\x2039 Back",
    L"No results for \x201C%s\x201D",
    L"Lock",
    L"Sign out",
    L"Sleep",
    L"Hibernate",
    L"Update and restart",
    L"Restart",
    L"Update and shut down",
    L"Shut down",
    L"Advanced startup",
    L"Restart to BIOS/UEFI",
    L"Pin to Start",
    L"Unpin from Start",
    L"Move to front",
    L"Apps",
    L"Settings",
    L"Run",
    L"Most used",
    L"Settings",
    L"Enter to copy",
    L"Open",
    L"Files",
    L"New",
    L"New",
    L"Recent",
    L"Windows",
    L"Switch to",
    L"Recent searches",
    L"Hide from the list",
    L"Dark mode",
};

const wchar_t* const kTurkish[] =
{
    L"Ara",
    L"Sabitlenmi\x015F",
    L"T\x00FCm uygulamalar",
    L"En iyi e\x015Fle\x015Fmeler",
    L"T\x00FCm\x00FC \x203A",
    L"\x2039 Geri",
    L"\x201C%s\x201D i\x00E7in sonu\x00E7 yok",
    L"Kilitle",
    L"Oturumu kapat",
    L"Uyku",
    L"Haz\x0131rda beklet",
    L"G\x00FCncelle ve yeniden ba\x015Flat",
    L"Yeniden ba\x015Flat",
    L"G\x00FCncelle ve kapat",
    L"Kapat",
    L"Geli\x015Fmi\x015F ba\x015Flang\x0131\x00E7",
    L"BIOS/UEFI'ye yeniden ba\x015Flat",
    L"Ba\x015Flat'a sabitle",
    L"Ba\x015Flat'tan kald\x0131r",
    L"Ba\x015F" L"a ta\x015F\x0131",
    L"Uygulamalar",
    L"Ayarlar",
    L"\x00C7" L"al\x0131\x015Ft\x0131r",
    L"En \x00E7ok kullan\x0131lanlar",
    L"Ayarlar",
    L"Kopyalamak i\x00E7in Enter",
    L"A\x00E7",
    L"Dosyalar",
    L"Yeni",
    L"Yeni",
    L"Son kullan\x0131lanlar",
    L"Pencereler",
    L"Ge\x00E7i\x015F yap",
    L"Son aramalar",
    L"Listeden gizle",
    L"Koyu mod",
};

static_assert(ARRAYSIZE(kEnglish) == (size_t)Str::Count, "every string needs an English text");
static_assert(ARRAYSIZE(kTurkish) == (size_t)Str::Count, "every string needs a Turkish text");

std::atomic<bool> g_turkish{ false };
std::atomic<bool> g_languageRead{ false };

bool ReadTurkish()
{
    DWORD language = 0, size = sizeof(language);
    if (RegGetValueW(HKEY_CURRENT_USER, TEXT(REGPATH), L"Language", RRF_RT_REG_DWORD, nullptr, &language, &size) !=
            ERROR_SUCCESS ||
        language == 0)
    {
        language = GetUserDefaultUILanguage();
    }
    return PRIMARYLANGID((LANGID)language) == LANG_TURKISH;
}

} // namespace

void RefreshLanguage()
{
    g_turkish.store(ReadTurkish(), std::memory_order_relaxed);
    g_languageRead.store(true, std::memory_order_release);
}

bool IsTurkish()
{
    if (!g_languageRead.load(std::memory_order_acquire))
    {
        RefreshLanguage();
    }
    return g_turkish.load(std::memory_order_relaxed);
}

const wchar_t* Text(Str id)
{
    if (!g_languageRead.load(std::memory_order_acquire))
    {
        RefreshLanguage();
    }
    size_t index = (size_t)id;
    if (index >= (size_t)Str::Count)
    {
        return L"";
    }
    return g_turkish.load(std::memory_order_relaxed) ? kTurkish[index] : kEnglish[index];
}

// ---------------------------------------------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------------------------------------------

namespace {

bool FamilyExists(IDWriteFactory* factory, const wchar_t* family)
{
    ComPtr<IDWriteFontCollection> system;
    if (FAILED(factory->GetSystemFontCollection(&system, FALSE)))
    {
        return false;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(system->FindFamilyName(family, &index, &exists)) && exists;
}

// Ascent and descent of the regular face, as fractions of the em. Used to place the baseline of the uniform
// 1.2x line box the handoff asks for.
void ReadMetrics(IDWriteFontCollection* collection, const wchar_t* family, float* ascent, float* descent)
{
    UINT32 index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> fontFamily;
    ComPtr<IDWriteFont> font;
    if (FAILED(collection->FindFamilyName(family, &index, &exists)) || !exists ||
        FAILED(collection->GetFontFamily(index, &fontFamily)) ||
        FAILED(fontFamily->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, &font)))
    {
        return;
    }
    DWRITE_FONT_METRICS metrics = {};
    font->GetMetrics(&metrics);
    if (metrics.designUnitsPerEm)
    {
        *ascent = (float)metrics.ascent / metrics.designUnitsPerEm;
        *descent = (float)metrics.descent / metrics.designUnitsPerEm;
    }
}

} // namespace

bool Fonts::Initialize(int preferred)
{
    if (m_factory)
    {
        return true;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                   reinterpret_cast<IUnknown**>(m_factory.GetAddressOf()))))
    {
        return false;
    }

    // Figtree from the DLL's resources. The resource bytes live as long as the DLL, which is never unloaded
    // while the menu exists, so the loader can reference them without copying.
    HMODULE self = reinterpret_cast<HMODULE>(&__ImageBase);
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (SUCCEEDED(m_factory->CreateInMemoryFontFileLoader(&m_loader)) &&
        SUCCEEDED(m_factory->RegisterFontFileLoader(m_loader.Get())))
    {
        ComPtr<IDWriteFontSetBuilder> plain;
        if (SUCCEEDED(m_factory->CreateFontSetBuilder(&plain)))
        {
            plain.As(&builder);
        }
    }
    else
    {
        m_loader.Reset();
    }

    if (builder && preferred == 0)
    {
        const int ids[] = { IDR_FONT_FIGTREE_REGULAR, IDR_FONT_FIGTREE_MEDIUM, IDR_FONT_FIGTREE_SEMIBOLD,
                            IDR_FONT_FIGTREE_BOLD };
        int added = 0;
        for (int id : ids)
        {
            HRSRC info = FindResourceW(self, MAKEINTRESOURCEW(id), RT_RCDATA);
            HGLOBAL data = info ? LoadResource(self, info) : nullptr;
            const void* bytes = data ? LockResource(data) : nullptr;
            DWORD size = info ? SizeofResource(self, info) : 0;
            ComPtr<IDWriteFontFile> file;
            if (bytes && size &&
                SUCCEEDED(m_loader->CreateInMemoryFontFileReference(m_factory.Get(), bytes, size, nullptr, &file)) &&
                SUCCEEDED(builder->AddFontFile(file.Get())))
            {
                ++added;
            }
        }
        ComPtr<IDWriteFontSet> set;
        if (added == ARRAYSIZE(ids) && SUCCEEDED(builder->CreateFontSet(&set)) &&
            SUCCEEDED(m_factory->CreateFontCollectionFromFontSet(set.Get(), &m_collection)))
        {
            wcscpy_s(m_family, L"Figtree");
            ReadMetrics(m_collection.Get(), m_family, &m_ascent, &m_descent);
        }
        else
        {
            m_collection.Reset();
        }
    }

    if (!m_collection)
    {
        wcscpy_s(m_family, preferred <= 1 && FamilyExists(m_factory.Get(), L"Segoe UI Variable Text")
                               ? L"Segoe UI Variable Text"
                               : L"Segoe UI");
        ComPtr<IDWriteFontCollection> system;
        if (SUCCEEDED(m_factory->GetSystemFontCollection(&system, FALSE)))
        {
            ReadMetrics(system.Get(), m_family, &m_ascent, &m_descent);
        }
    }
    return true;
}

void Fonts::Shutdown()
{
    m_collection.Reset();
    if (m_loader && m_factory)
    {
        m_factory->UnregisterFontFileLoader(m_loader.Get());
    }
    m_loader.Reset();
    m_factory.Reset();
}

ComPtr<IDWriteTextFormat> Fonts::CreateFormat(const FormatSpec& requested) const
{
    FormatSpec spec = requested;
    spec.style.size *= m_sizeScale;
    ComPtr<IDWriteTextFormat> format;
    if (!m_factory ||
        FAILED(m_factory->CreateTextFormat(m_family, m_collection.Get(), spec.style.weight, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, spec.style.size, L"", &format)))
    {
        return nullptr;
    }
    format->SetTextAlignment(spec.align);
    format->SetParagraphAlignment(spec.paragraph);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // One line box of 1.2 x the font size, with the glyphs centred in it the way a browser centres them.
    float line = spec.style.size * type::LineHeight;
    float content = (m_ascent + m_descent) * spec.style.size;
    float baseline = (line - content) / 2 + m_ascent * spec.style.size;
    format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, baseline);

    if (spec.ellipsis)
    {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(m_factory->CreateEllipsisTrimmingSign(format.Get(), &sign)))
        {
            DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            format->SetTrimming(&trimming, sign.Get());
        }
    }
    return format;
}

float Fonts::Measure(IDWriteTextFormat* format, const wchar_t* text, UINT32 length) const
{
    ComPtr<IDWriteTextLayout> layout;
    if (!m_factory || !format ||
        FAILED(m_factory->CreateTextLayout(text, length, format, 100000.f, 100.f, &layout)))
    {
        return 0;
    }
    DWRITE_TEXT_METRICS metrics = {};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

} // namespace sm
