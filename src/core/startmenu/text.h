#pragma once
//
// text.h - fonts, text formats and the menu's own strings.
//
// The design uses Figtree (SIL OFL, fonts/OFL.txt). The four weights are embedded in ShadePatcher.dll as RCDATA
// and handed to DirectWrite through an in-memory font loader, so nothing is installed on the system and nothing is
// read from disk. When that fails (DirectWrite older than 1703), the menu falls back to Segoe UI Variable Text and
// then Segoe UI, as the handoff asks.
//
#include <Windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "tokens.h"

namespace sm {

using Microsoft::WRL::ComPtr;

// One entry per string the menu draws or shows in its context menu. Text() picks the language.
enum class Str
{
    SearchPlaceholder,
    TitlePinned,
    TitleAllApps,
    TitleBestMatches,
    ToggleAll,
    ToggleBack,
    NoResults,          // printf-style, one %s: the query
    Lock,
    SignOut,
    Sleep,
    Hibernate,
    UpdateRestart,
    Restart,
    UpdateShutDown,
    ShutDown,
    AdvancedStartup,
    Firmware,
    PinToStart,
    UnpinFromStart,
    MoveToFront,
    SectionApps,
    SectionSettings,
    SectionCommands,
    SectionMostUsed,
    HintSettings,       // after a Settings result
    HintCopy,           // after a calculation
    HintOpen,           // after a path or location
    SectionFiles,
    SectionNew,
    BadgeNew,
    JumpRecent,         // heading of an app's recent files in its context menu
    SectionWindows,
    HintSwitch,         // after an open window in the results
    SectionHistory,
    HideApp,
    QuickDark,
    Count
};

// Text in the settings window's language: the "Language" value under HKCU\Software\ShadePatcher when set,
// otherwise the user's UI language. Turkish and English; anything else gets English.
const wchar_t* Text(Str id);

// Re-reads the language choice. Called when the settings change.
void RefreshLanguage();

// True when the menu speaks Turkish.
bool IsTurkish();

// Horizontal alignment and whether a label that does not fit ends in an ellipsis.
struct FormatSpec
{
    type::Style style;
    DWRITE_TEXT_ALIGNMENT align;
    DWRITE_PARAGRAPH_ALIGNMENT paragraph;
    bool ellipsis;
};

class Fonts
{
public:
    // `preferred`: 0 Figtree (embedded), 1 Segoe UI Variable Text, 2 Segoe UI. A missing choice falls back down
    // the same list.
    bool Initialize(int preferred = 0);
    void Shutdown();

    IDWriteFactory5* Factory() const { return m_factory.Get(); }

    // A text format for one of the handoff's type styles. Formats are cheap; view.cpp keeps the ones it uses.
    ComPtr<IDWriteTextFormat> CreateFormat(const FormatSpec& spec) const;

    // Multiplies every size CreateFormat is given (the "font size" setting).
    void SetSizeScale(float scale) { m_sizeScale = scale; }

    // Width in DIPs of a single line of text in the given format, trailing spaces included.
    float Measure(IDWriteTextFormat* format, const wchar_t* text, UINT32 length) const;

    bool UsingFigtree() const { return m_collection != nullptr; }

private:
    ComPtr<IDWriteFactory5> m_factory;
    ComPtr<IDWriteInMemoryFontFileLoader> m_loader;
    ComPtr<IDWriteFontCollection1> m_collection;   // Figtree, or null when the system fonts are used
    wchar_t m_family[64] = L"";
    float m_ascent = 0.95f;    // em fractions of the chosen family, for the 1.2x uniform line height
    float m_descent = 0.25f;
    float m_sizeScale = 1.f;
};

} // namespace sm
