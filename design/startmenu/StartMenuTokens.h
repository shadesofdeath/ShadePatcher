#pragma once
// Start Menu design tokens — all metrics in DIPs (96 DPI). px = dip * dpi / 96
#include <d2d1_1.h>

namespace sm {

constexpr D2D1_COLOR_F Hex(unsigned rgb, float a = 1.f) {
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
}

namespace metric {
  constexpr float PanelW = 580, PanelMaxH = 600, PanelRadius = 16, PanelBorder = 1;
  constexpr float EdgeGap = 12;                          // from work-area bottom / left
  constexpr float ShadowPadL = 80, ShadowPadR = 80, ShadowPadT = 50, ShadowPadB = 110;

  constexpr float SearchX = 24, SearchY = 24, SearchW = 530, SearchH = 44, SearchRadius = 22;
  constexpr float SearchIconInset = 16, SearchIcon = 16, SearchTextX = 42, ClearBtn = 22;

  constexpr float HeaderY = 76, HeaderH = 48, HeaderPadX = 28;
  constexpr float ToggleH = 28, TogglePadX = 12;

  constexpr float ContentY = 124, ContentPadT = 4, ContentPadX = 16, ContentPadB = 12;
  constexpr int   GridCols = 5;
  constexpr float GridGap = 2, CellH = 96, CellRadius = 12;
  constexpr float Icon = 44, IconTop = 14, IconLabelGap = 9, LabelMaxW = 96;

  constexpr float ListHeadH = 30, ListRowH = 44, ListRowRadius = 10, ListPadX = 12;
  constexpr float ListIcon = 28, ListIconRadius = 8, ListIconGap = 12;

  constexpr float RecentH = 46, ChipH = 30, ChipPadX = 12, ChipGap = 8, ChipDot = 6, ChipDotGap = 7;

  constexpr float FooterH = 60, FooterPadL = 20, FooterPadR = 16;
  constexpr float AccountH = 40, Avatar = 28, AvatarGap = 10, PowerBtn = 40;
  constexpr float FlyoutW = 172, FlyoutPad = 6, FlyoutRadius = 12, FlyoutItemH = 36, FlyoutGapAbove = 8;

  constexpr float WheelStep = 48, ScrollMargin = 6;
}

namespace type {   // Figtree; fallback "Segoe UI Variable Text"
  struct Style { float size; DWRITE_FONT_WEIGHT weight; };
  constexpr Style Title     { 15.f,  DWRITE_FONT_WEIGHT_BOLD };
  constexpr Style Search    { 15.f,  DWRITE_FONT_WEIGHT_NORMAL };
  constexpr Style ListItem  { 14.f,  DWRITE_FONT_WEIGHT_MEDIUM };
  constexpr Style UserName  { 13.f,  DWRITE_FONT_WEIGHT_SEMI_BOLD };
  constexpr Style Button    { 13.f,  DWRITE_FONT_WEIGHT_MEDIUM };
  constexpr Style GridLabel { 12.5f, DWRITE_FONT_WEIGHT_MEDIUM };
  constexpr Style Letter    { 12.f,  DWRITE_FONT_WEIGHT_BOLD };
  constexpr Style Chip      { 12.f,  DWRITE_FONT_WEIGHT_NORMAL };
}

namespace motion {  // seconds; bezier = (x1,y1,x2,y2)
  struct Bezier { float x1, y1, x2, y2; };
  constexpr double OpenOpacity = 0.160, OpenTransform = 0.220;
  constexpr Bezier EaseOut  { 0.00f, 0.00f, 0.58f, 1.00f };
  constexpr Bezier Settle   { 0.20f, 0.90f, 0.30f, 1.10f };   // slight overshoot
  constexpr Bezier Ease     { 0.25f, 0.10f, 0.25f, 1.00f };
  constexpr float  ClosedOffsetY = 16.f, ClosedScale = 0.98f;
  constexpr double Highlight = 0.080, FieldBorder = 0.120, Press = 0.120;
  constexpr float  PressScale = 0.96f;
}

} // namespace sm
