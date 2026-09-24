#pragma once
//
// tokens.h - the Start menu's design tokens.
//
// Taken from the design handoff (design/startmenu/StartMenuTokens.h, README.md sections 5-8). Every metric is in
// DIPs (96 DPI); the renderer multiplies by dpi / 96. Layout coordinates in the handoff are relative to the inside
// of the panel's 1 DIP border, which is how metric::PanelBorder is used in view.cpp.
//
// Change a value here, not in the drawing code: nothing in view.cpp hard-codes a colour or a size the design owns.
//
#include <d2d1_1.h>
#include <dwrite.h>

namespace sm {

constexpr D2D1_COLOR_F Hex(unsigned rgb, float a = 1.f)
{
    return { ((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a };
}

namespace color {
    constexpr auto TextPrimary     = Hex(0x1D2320);
    constexpr auto TextLabel       = Hex(0x2B312E);
    constexpr auto TextSecondary   = Hex(0x4A514D);
    constexpr auto TextChip        = Hex(0x3A413D);
    constexpr auto TextMuted       = Hex(0x6E7571);
    constexpr auto TextPlaceholder = Hex(0x8B918E);
    constexpr auto Accent          = Hex(0x1E9E78);
    constexpr auto Selection       = Hex(0x1E9E78, 0.22f);   // Ctrl+A in the search box (not in the handoff)

    constexpr auto PanelAcrylic    = Hex(0xFAFAF8, 0.88f);
    constexpr auto PanelOpaque     = Hex(0xF7F8F6);
    constexpr auto FooterAcrylic   = Hex(0xECEFEC, 0.60f);
    constexpr auto FooterOpaque    = Hex(0xEFF1EF);
    constexpr auto Field           = Hex(0xFFFFFF);
    constexpr auto ClearBtn        = Hex(0xEEF0EE);

    constexpr auto Ink05 = Hex(0x1D2320, 0.05f);   // toggle bg, account/flyout hover
    constexpr auto Ink06 = Hex(0x1D2320, 0.06f);   // selection / hover, hairlines
    constexpr auto Ink07 = Hex(0x1D2320, 0.07f);   // chip border
    constexpr auto Ink08 = Hex(0x1D2320, 0.08f);   // search border, power pressed
    constexpr auto Ink09 = Hex(0x1D2320, 0.09f);   // toggle hover
    constexpr auto Ink18 = Hex(0x1D2320, 0.18f);   // chip border hover
    constexpr auto PanelBorder = Hex(0xFFFFFF, 0.90f);

    constexpr auto Shadow1  = Hex(0x1E2D28, 0.20f);
    constexpr auto Shadow2  = Hex(0x1E2D28, 0.08f);
    constexpr auto ShadowFl = Hex(0x1E2D28, 0.16f);

    constexpr auto AvatarA = Hex(0xF2B38B);
    constexpr auto AvatarB = Hex(0xE27D6A);

    // Dot colours of the recent-file chips, by file kind (the second stop of the handoff's icon gradients).
    constexpr auto KindDocument = Hex(0x3A86E0);   // sky
    constexpr auto KindImage    = Hex(0xE0624A);   // coral
    constexpr auto KindCode     = Hex(0x1E9E78);   // mint
    constexpr auto KindMedia    = Hex(0x8465D8);   // plum
    constexpr auto KindArchive  = Hex(0xEBA02C);   // sun
    constexpr auto KindOther    = Hex(0x2A302D);   // ink

    // Placeholder tile drawn while an app's real icon is still loading (mint gradient of the handoff).
    constexpr auto TileA = Hex(0x3CC79B);
    constexpr auto TileB = Hex(0x1E9E78);
}

namespace metric {
    constexpr float PanelW = 580, PanelMaxH = 600, PanelRadius = 16, PanelBorder = 1;
    constexpr float EdgeGap = 12;                          // from work-area bottom / left
    constexpr float ShadowPadL = 80, ShadowPadR = 80, ShadowPadT = 50, ShadowPadB = 110;

    constexpr float SearchX = 24, SearchY = 24, SearchW = 530, SearchH = 44, SearchRadius = 22;
    constexpr float SearchIconInset = 16, SearchIcon = 16, SearchTextX = 42, ClearBtn = 22;
    constexpr float CaretW = 1.5f, CaretH = 18;

    constexpr float HeaderY = 76, HeaderH = 48, HeaderPadX = 28, HeaderPadT = 16, HeaderPadB = 4;
    constexpr float ToggleH = 28, TogglePadX = 12;

    constexpr float ContentY = 124, ContentPadT = 4, ContentPadX = 16, ContentPadB = 12;
    constexpr int   GridCols = 5;
    constexpr float GridGap = 2, CellH = 96, CellRadius = 12;
    constexpr float Icon = 44, IconRadius = 13, IconTop = 14, IconLabelGap = 9, LabelMaxW = 96;
    constexpr float EmptyPadT = 60;

    constexpr float ListHeadH = 30, ListHeadPadB = 4, ListRowH = 44, ListRowRadius = 10, ListPadX = 12;
    constexpr float ListIcon = 28, ListIconRadius = 8, ListIconGap = 12;

    constexpr float RecentH = 46, RecentPadX = 24, ChipH = 30, ChipPadX = 12, ChipGap = 8, ChipDot = 6, ChipDotGap = 7;
    constexpr float ChipTextMax = 180;   // not in the handoff: a long file name is cut with an ellipsis

    constexpr float FooterH = 60, FooterPadL = 20, FooterPadR = 16;
    constexpr float AccountH = 40, AccountPadL = 6, AccountPadR = 10, Avatar = 28, AvatarGap = 10, PowerBtn = 40;
    constexpr float PowerIcon = 16;
    constexpr float FlyoutW = 172, FlyoutPad = 6, FlyoutRadius = 12, FlyoutItemH = 36, FlyoutItemRadius = 8;
    constexpr float FlyoutItemPadX = 12, FlyoutGapAbove = 8;
    constexpr float FlyoutShadowY = 12, FlyoutShadowBlur = 32;

    constexpr float WheelStep = 48, ScrollMargin = 6;
}

namespace type {   // Figtree; fallback "Segoe UI Variable Text"
    struct Style { float size; DWRITE_FONT_WEIGHT weight; };
    constexpr Style Title     { 15.f,  DWRITE_FONT_WEIGHT_BOLD };
    constexpr Style Search    { 15.f,  DWRITE_FONT_WEIGHT_NORMAL };
    constexpr Style ListItem  { 14.f,  DWRITE_FONT_WEIGHT_MEDIUM };
    constexpr Style Empty     { 14.f,  DWRITE_FONT_WEIGHT_NORMAL };
    constexpr Style UserName  { 13.f,  DWRITE_FONT_WEIGHT_SEMI_BOLD };
    constexpr Style Initial   { 13.f,  DWRITE_FONT_WEIGHT_BOLD };
    constexpr Style Button    { 13.f,  DWRITE_FONT_WEIGHT_MEDIUM };
    constexpr Style Clear     { 13.f,  DWRITE_FONT_WEIGHT_NORMAL };
    constexpr Style GridLabel { 12.5f, DWRITE_FONT_WEIGHT_MEDIUM };
    constexpr Style Letter    { 12.f,  DWRITE_FONT_WEIGHT_BOLD };
    constexpr Style Chip      { 12.f,  DWRITE_FONT_WEIGHT_NORMAL };
    constexpr float LineHeight = 1.2f;   // single-line labels: font size x 1.2
}

namespace motion {  // seconds; bezier = (x1,y1,x2,y2)
    struct Bezier { float x1, y1, x2, y2; };
    constexpr double OpenOpacity = 0.160, OpenTransform = 0.220;
    constexpr Bezier EaseOut  { 0.00f, 0.00f, 0.58f, 1.00f };
    constexpr Bezier Settle   { 0.20f, 0.90f, 0.30f, 1.10f };   // slight overshoot
    constexpr Bezier Ease     { 0.25f, 0.10f, 0.25f, 1.00f };
    constexpr Bezier Linear   { 0.00f, 0.00f, 1.00f, 1.00f };
    constexpr float  ClosedOffsetY = 16.f, ClosedScale = 0.98f;
    constexpr double Highlight = 0.080, FieldBorder = 0.120, Press = 0.120;
    constexpr float  PressScale = 0.96f;
}

// ---------------------------------------------------------------------------------------------------------------
// Themes
//
// The handoff defines the light theme only; the dark one keeps its structure (the same ink-over-surface alphas,
// the same hierarchy of text tones) with the surfaces and inks swapped. view.cpp draws with a Palette and never
// with the color:: constants directly, so a theme is one value.
// ---------------------------------------------------------------------------------------------------------------

struct Palette
{
    D2D1_COLOR_F TextPrimary, TextLabel, TextSecondary, TextChip, TextMuted, TextPlaceholder;
    D2D1_COLOR_F Accent, Selection;
    D2D1_COLOR_F PanelOpaque, FooterOpaque;     // opaque background
    D2D1_COLOR_F PanelTint, FooterTint;         // acrylic background: colour only, the alpha comes from the setting
    float FooterTintAlpha;                      // of the footer layer over the panel tint
    D2D1_COLOR_F Field, ClearBtn;
    D2D1_COLOR_F Ink05, Ink06, Ink07, Ink08, Ink09, Ink18;
    D2D1_COLOR_F PanelBorder;
    D2D1_COLOR_F Shadow1, Shadow2, ShadowFl;
    bool dark;
};

inline Palette LightPalette()
{
    Palette p;
    p.TextPrimary = color::TextPrimary;   p.TextLabel = color::TextLabel;   p.TextSecondary = color::TextSecondary;
    p.TextChip = color::TextChip;         p.TextMuted = color::TextMuted;   p.TextPlaceholder = color::TextPlaceholder;
    p.Accent = color::Accent;             p.Selection = color::Selection;
    p.PanelOpaque = color::PanelOpaque;   p.FooterOpaque = color::FooterOpaque;
    p.PanelTint = Hex(0xFAFAF8);          p.FooterTint = Hex(0xECEFEC);     p.FooterTintAlpha = 0.60f;
    p.Field = color::Field;               p.ClearBtn = color::ClearBtn;
    p.Ink05 = color::Ink05; p.Ink06 = color::Ink06; p.Ink07 = color::Ink07; p.Ink08 = color::Ink08;
    p.Ink09 = color::Ink09; p.Ink18 = color::Ink18;
    p.PanelBorder = color::PanelBorder;
    p.Shadow1 = color::Shadow1; p.Shadow2 = color::Shadow2; p.ShadowFl = color::ShadowFl;
    p.dark = false;
    return p;
}

inline Palette DarkPalette()
{
    Palette p;
    p.TextPrimary = Hex(0xECEFED);        p.TextLabel = Hex(0xD9DEDB);      p.TextSecondary = Hex(0xB7BEBA);
    p.TextChip = Hex(0xC9CFCC);           p.TextMuted = Hex(0x9AA29E);      p.TextPlaceholder = Hex(0x7E8682);
    p.Accent = Hex(0x3CC79B);             p.Selection = Hex(0x3CC79B, 0.30f);
    p.PanelOpaque = Hex(0x202523);        p.FooterOpaque = Hex(0x1A1E1C);
    p.PanelTint = Hex(0x1E2321);          p.FooterTint = Hex(0x141816);     p.FooterTintAlpha = 0.50f;
    p.Field = Hex(0x2A302D);              p.ClearBtn = Hex(0x39403C);
    p.Ink05 = Hex(0xFFFFFF, 0.05f); p.Ink06 = Hex(0xFFFFFF, 0.07f); p.Ink07 = Hex(0xFFFFFF, 0.08f);
    p.Ink08 = Hex(0xFFFFFF, 0.10f); p.Ink09 = Hex(0xFFFFFF, 0.11f); p.Ink18 = Hex(0xFFFFFF, 0.22f);
    p.PanelBorder = Hex(0xFFFFFF, 0.08f);
    p.Shadow1 = Hex(0x000000, 0.45f); p.Shadow2 = Hex(0x000000, 0.25f); p.ShadowFl = Hex(0x000000, 0.40f);
    p.dark = true;
    return p;
}

// Fixed colour themes. Each starts from the light or dark palette and changes the surfaces, the text tone and
// the accent; the ink alphas and the structure stay those of the design.
inline Palette MidnightPalette()
{
    Palette p = DarkPalette();
    p.TextPrimary = Hex(0xE8ECF7);  p.TextLabel = Hex(0xD3D9EA);  p.TextSecondary = Hex(0xAEB7CF);
    p.TextChip = Hex(0xC3CBE0);     p.TextMuted = Hex(0x929CB8);  p.TextPlaceholder = Hex(0x76809C);
    p.Accent = Hex(0x6BB8F5);       p.Selection = Hex(0x6BB8F5, 0.30f);
    p.PanelOpaque = Hex(0x151C2E);  p.FooterOpaque = Hex(0x111728);
    p.PanelTint = Hex(0x131A2C);    p.FooterTint = Hex(0x0C1120);
    p.Field = Hex(0x1F283F);        p.ClearBtn = Hex(0x2C3754);
    return p;
}

inline Palette GraphitePalette()
{
    Palette p = DarkPalette();
    p.TextPrimary = Hex(0xEDEDED);  p.TextLabel = Hex(0xD6D6D6);  p.TextSecondary = Hex(0xB3B3B3);
    p.TextChip = Hex(0xC8C8C8);     p.TextMuted = Hex(0x9A9A9A);  p.TextPlaceholder = Hex(0x7D7D7D);
    p.Accent = Hex(0xE0E0E0);       p.Selection = Hex(0xFFFFFF, 0.25f);
    p.PanelOpaque = Hex(0x262626);  p.FooterOpaque = Hex(0x1F1F1F);
    p.PanelTint = Hex(0x242424);    p.FooterTint = Hex(0x1A1A1A);
    p.Field = Hex(0x333333);        p.ClearBtn = Hex(0x404040);
    return p;
}

inline Palette SandPalette()
{
    Palette p = LightPalette();
    p.TextPrimary = Hex(0x2E2921);  p.TextLabel = Hex(0x3A342B);  p.TextSecondary = Hex(0x5A5246);
    p.TextChip = Hex(0x4A4339);     p.TextMuted = Hex(0x7D7466);  p.TextPlaceholder = Hex(0x9A9183);
    p.Accent = Hex(0xC27C2C);       p.Selection = Hex(0xC27C2C, 0.22f);
    p.PanelOpaque = Hex(0xF6F1E7);  p.FooterOpaque = Hex(0xEEE6D6);
    p.PanelTint = Hex(0xF8F3EA);    p.FooterTint = Hex(0xEADFCB);
    p.Field = Hex(0xFFFDF8);        p.ClearBtn = Hex(0xEFE8DA);
    p.Ink05 = Hex(0x2E2921, 0.05f); p.Ink06 = Hex(0x2E2921, 0.06f); p.Ink07 = Hex(0x2E2921, 0.07f);
    p.Ink08 = Hex(0x2E2921, 0.08f); p.Ink09 = Hex(0x2E2921, 0.09f); p.Ink18 = Hex(0x2E2921, 0.18f);
    return p;
}

// Accent choices offered in the settings (index = the "Accent" value). 0 is the theme's own, 1 is the Windows
// accent colour (read at run time), the rest are the handoff's icon-gradient hues.
namespace accent {
    constexpr unsigned Presets[] = { 0, 0, 0x3A86E0, 0x8465D8, 0xE0624A, 0xD8638B, 0xEBA02C, 0x2C9EA8 };
    constexpr int Count = sizeof(Presets) / sizeof(Presets[0]);
}

} // namespace sm
