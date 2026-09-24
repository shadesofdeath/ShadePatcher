#pragma once
//
// view.h - the Start menu window: Win32 for input, DirectComposition for the visual tree and animation,
// Direct2D for drawing, DirectWrite for text. The design is design/startmenu (README.md is the spec).
//
// Two windows
// -----------
// The panel casts a large soft shadow (80 DIPs to the sides, 110 below). A window big enough to hold that shadow
// would swallow every click in the shadow, including clicks on the taskbar right under the menu. So the menu is
// two windows that never move relative to each other:
//
//   * the visual window: panel + shadow margins, WS_EX_LAYERED | WS_EX_TRANSPARENT so the system skips it when
//     hit-testing, WS_EX_NOACTIVATE. Everything the user sees is drawn into it through DirectComposition.
//   * the input window: exactly the panel, WS_EX_NOREDIRECTIONBITMAP with no content, so it is invisible yet
//     takes the clicks, the keyboard focus and the activation. It is placed above the visual window on every
//     opening. It has no owner on purpose: hiding an owned window hands the activation to its owner.
//
// Visual tree (handoff section 4.3)
// ---------------------------------
//   Root
//   +- Container          open/close: scale + translate transform group, effect-group opacity
//   |  +- Shadow           two D2D1Shadow layers, redrawn only when size or DPI change
//   |  +- Panel            rounded rectangle clip, radius 16
//   |  |  +- Background    panel and footer fill, footer hairline
//   |  |  +- ScrollHost    clipped to the content area
//   |  |  |  +- ScrollLayer   offset = -scrollY; scrolling never redraws
//   |  |  |     +- Highlight  one hover/selection box, moved and faded in
//   |  |  |     +- Content    virtual surface, only a band around the viewport is drawn
//   |  |  |     +- Pressed    the pressed grid cell, scaled to 0.96
//   |  |  +- Chrome        search box, header, recent files, footer, panel border
//   |  |  +- Caret         blinks with GetCaretBlinkTime
//   |  +- Flyout           the power menu with its own shadow
//
#include <Windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "backdrop.h"
#include "catalog.h"
#include "filesearch.h"
#include "quick.h"
#include "search.h"
#include "text.h"

namespace sm {

using Microsoft::WRL::ComPtr;

// What asked for the menu. A click on the Start button while the menu has just closed because that same click
// took the focus away must not open it again.
enum class OpenSource : WPARAM
{
    StartButton = 1,
    Keyboard = 2,
    Preview = 3,
    MiddleClick = 4,     // a middle click on Start: what it does is ViewOptions::middleClick
};

// Messages the input window accepts from other threads.
constexpr UINT kMsgToggle = WM_APP + 1;     // wParam = OpenSource, lParam = HMONITOR to open on (0 = primary)
constexpr UINT kMsgLoaded = WM_APP + 2;     // from the Loader, see catalog.h
constexpr UINT kMsgClose  = WM_APP + 3;     // close without animation (settings turned the menu off)
constexpr UINT kMsgSetOptions = WM_APP + 4; // lParam = new ViewOptions, owned by the receiver
constexpr UINT kMsgResetPins = WM_APP + 5;  // put the default pinned apps back
constexpr UINT kMsgCommand = WM_APP + 6;    // wParam: kCommandShowHidden or kCommandClearHistory
constexpr WPARAM kCommandShowHidden = 1;    // bring back the apps hidden from the lists
constexpr WPARAM kCommandClearHistory = 2;  // forget the search history

enum class Placement { Auto = 0, Center = 1, Left = 2 };

// System follows Windows' light/dark setting; the rest are fixed palettes (tokens.h).
enum class ThemeMode { System = 0, Light = 1, Dark = 2, Midnight = 3, Graphite = 4, Sand = 5, Scheduled = 6 };
// Acrylic blurs what is behind the menu; Glass is the same translucent tint without the blur.
enum class Background { Opaque = 0, Acrylic = 1, Glass = 2 };
enum class Animation { System = 0, On = 1, Fast = 2, Off = 3 };

// Everything the settings window can change. Values are validated in the mod (custom_start_menu.cpp).
struct ViewOptions
{
    // Menu
    Placement placement = Placement::Auto;
    float edgeGap = metric::EdgeGap;            // DIPs between the panel and the taskbar / screen edge
    bool showRecent = true;
    bool startWithAllApps = false;              // open on "All apps" instead of the pinned grid
    bool forceRecent = false;   // preview only: list recent files even when Windows is told not to track them

    // Appearance
    ThemeMode theme = ThemeMode::System;
    Background background = Background::Opaque;
    int opacity = 80;                           // acrylic tint, percent (the handoff says 0.88; 80 shows the blur)
    int accent = 0;                             // index into accent::Presets
    float cornerRadius = metric::PanelRadius;
    bool shadow = true;
    int font = 0;                               // 0 Figtree, 1 Segoe UI Variable, 2 Segoe UI
    int scale = 100;                            // percent, on top of the monitor's DPI
    int fontScale = 100;                        // percent, text only
    int darkFrom = 19, lightFrom = 7;           // ThemeMode::Scheduled: the hours dark and light start
    std::wstring backgroundImage;               // a picture behind the panel (replaces opaque/acrylic)
    int imageBlur = 20;                         // DIPs of blur on it
    int imageTint = 55;                         // percent of the panel colour over it

    // Layout
    int columns = metric::GridCols;
    float iconSize = metric::Icon;
    float listIconSize = metric::ListIcon;
    bool labels = true;                         // names under the pinned icons
    float maxHeight = metric::PanelMaxH;

    // What is shown
    bool showSearch = true;                     // hidden: the box appears once something is typed
    bool showTitle = true;                      // "Pinned" / "All apps" (the All/Back button always stays)
    bool showFooter = true;
    bool showAccount = true;
    bool showUserName = true;
    bool showPower = true;

    // Extras
    bool showMostUsed = true;                   // "Most used" under the pinned grid
    int mostUsedCount = 4;
    bool searchSettings = true;                 // Settings pages in the search results
    bool searchCalculator = true;               // "12*7" shows the result
    bool searchCommands = true;                 // programs, paths, %VARIABLES%, shell: and URLs, like Run
    unsigned shortcuts = kDefaultShortcuts;     // footer buttons, bit (1 << Shortcut)
    bool searchFiles = true;                    // files from the Windows Search index
    int fileCount = 6;
    bool showNewApps = true;                    // mark apps installed in the last week
    bool powerUpdates = true;                   // "Update and restart/shut down" while updates wait
    int middleClick = 0;                        // 0 leave to Windows, 1 all apps, 2 File Explorer, 3 Task Manager, 4 Settings
    bool showQuick = true;                      // Wi-Fi, Bluetooth, volume and dark mode above the footer
    bool searchWindows = true;                  // open windows in the search results
    bool searchHistory = true;                  // recent searches when the empty search box is clicked

    // Behaviour
    Animation animation = Animation::System;
    bool slide = true;                          // open with the handoff's slide and scale; false = fade only
    unsigned powerItems = kDefaultPowerItems;   // bit (1 << PowerAction) for each entry of the power menu

    bool operator==(const ViewOptions&) const = default;
};

// The part of the design that follows the options. Names match the metric:: constants they replace.
struct Layout
{
    int GridCols;
    float PanelW, PanelMaxH, PanelRadius, EdgeGap, SearchW;
    float CellW, CellH, CellRadius, Icon, IconTop, IconRadius, LabelMaxW;
    float ListIcon, ListIconRadius, ListRowH;
    bool labels;
};

class MenuView
{
public:
    MenuView();
    ~MenuView();

    // Creates the windows, the device and the loader. UI thread; the thread needs a message loop and COM (STA).
    bool Create(const ViewOptions& options);
    void Destroy();

    HWND Window() const { return m_input; }
    bool IsOpen() const { return m_open; }

    void SetOptions(const ViewOptions& options);
    void Toggle(OpenSource source, HMONITOR monitor);
    void Open(HMONITOR monitor);
    // `restoreFocus`: give the foreground back to the window that had it before the menu opened. Not wanted when
    // something is being launched, which should come to the front instead.
    void Close(bool animate, bool restoreFocus = true);

    // Called once the close animation has finished and the windows are hidden.
    std::function<void()> onHidden;

    // How long the last Open() took, from the request to the first committed frame (milliseconds).
    double lastOpenMs = 0;

private:
    // --- layout -------------------------------------------------------------------------------------------------
    // Cell: a pinned or found app in the grid. Row: an app in a list. Result: a search result that is not an app.
    enum class ItemKind { Cell, Row, Result, Letter };
    struct Item
    {
        int app;                // index into m_apps->apps, or -1 for a Result
        D2D1_RECT_F rect;       // content coordinates (DIPs, y from the top of the content surface)
        ItemKind kind = ItemKind::Cell;
        int result = -1;        // index into Model::results, or into m_letterList for a Letter
    };
    struct Heading
    {
        std::wstring text;
        D2D1_RECT_F rect;
    };
    struct Model
    {
        bool grid = true;
        bool empty = false;     // a search with no results
        std::vector<Item> items;
        std::vector<Heading> headings;
        std::vector<Result> results;
        float height = 0;       // DIPs
    };

    enum class Target
    {
        None, Search, Clear, Toggle, Item, Chip, Account, Power, FlyoutItem, Flyout, Content, Shortcut, Heading, Quick,
    };
    struct Hit
    {
        Target target = Target::None;
        int index = -1;
        bool operator==(const Hit& o) const { return target == o.target && index == o.index; }
        bool operator!=(const Hit& o) const { return !(*this == o); }
    };

    // --- windows --------------------------------------------------------------------------------------------------
    static LRESULT CALLBACK InputProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool RegisterClasses();

    // --- device ---------------------------------------------------------------------------------------------------
    bool CreateDevice();
    void ReleaseDevice();
    bool DeviceLost(HRESULT hr);
    void RecoverDevice();
    bool BuildVisualTree();
    bool BuildSurfaces();            // everything whose size depends on DPI or panel height

    // --- style ----------------------------------------------------------------------------------------------------
    size_t ApplyStyle();             // m_lay, m_pal, m_acrylic, m_powerItems from the options and the system
    void CreateFormats();

    // --- geometry -------------------------------------------------------------------------------------------------
    bool PlaceOnMonitor(HMONITOR monitor);
    float S() const { return m_scale; }
    float Px(float dip) const { return dip * m_scale; }
    bool SearchShown() const { return m_options.showSearch || !m_query.empty(); }
    float TopShift() const { return SearchShown() ? 0.f : metric::HeaderY - 8; }   // the search area collapses
    float HeaderY() const { return metric::HeaderY - TopShift(); }
    float FooterH() const { return m_options.showFooter ? metric::FooterH : 0.f; }
    bool AccountShown() const { return m_options.showFooter && m_options.showAccount; }
    bool PowerShown() const { return m_options.showFooter && m_options.showPower; }
    float Snap(float dip) const;
    float ContentTop() const;
    float ContentBottom() const;
    float ContentHeight() const { return ContentBottom() - ContentTop(); }
    bool RecentVisible() const;
    D2D1_RECT_F SearchRect() const;
    D2D1_RECT_F FlyoutRect() const;        // panel coordinates
    D2D1_RECT_F FlyoutItemRect(int i) const;
    D2D1_RECT_F PowerRect() const;
    D2D1_RECT_F AccountRect() const;
    D2D1_RECT_F ToggleRect() const;
    D2D1_RECT_F ClearRect() const;

    // --- state ----------------------------------------------------------------------------------------------------
    void ResetState();
    void BuildModel();
    void SetQuery(std::wstring query);
    void SetSelection(int sel, bool fromKeyboard);
    void ScrollTo(float y);
    void EnsureVisible(int sel);
    void MoveSelection(int delta);
    void MoveSpatial(int dx, int dy);   // arrow keys: the nearest item in that direction, whatever the layout
    const App* SelectedApp() const;
    void SetHot(Hit hit);
    void SetFlyout(bool open);

    // --- drawing --------------------------------------------------------------------------------------------------
    struct Draw;   // BeginDraw/EndDraw scope, see view.cpp
    void RenderShadow();
    void RenderBackground();
    void RenderChrome();
    void RenderContent(bool force);
    void RenderContentBand(float top, float bottom);
    void RenderHighlightSurfaces();
    void RenderFlyout();
    void RenderCaret();
    void UpdateScrollClip();
    void Hide();
    void UpdateHighlight(bool animate);
    void UpdateCaret();
    void DrawItem(ID2D1DeviceContext* dc, const Item& item, bool skipPressed);
    void DrawIcon(ID2D1DeviceContext* dc, const App& app, D2D1_RECT_F rect, float radius);
    ID2D1Bitmap* BitmapFor(const PixelsPtr& pixels);
    ID2D1SolidColorBrush* Brush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& color);
    void Commit();

    // --- animation ------------------------------------------------------------------------------------------------
    void AnimateOpen(bool opening);
    void StartPress(int item);
    void EndPress(bool restoreNow);
    void StartFieldTransition();

    // --- input ----------------------------------------------------------------------------------------------------
    Hit HitTest(POINT px) const;
    void OnMouseMove(POINT px);
    void OnMouseDown(POINT px);
    void OnMouseUp(POINT px);
    void OnRightClick(POINT px, bool fromKeyboard);
    void OnWheel(int delta);
    bool OnKeyDown(WPARAM key);
    void OnChar(wchar_t c);
    void Activate(Hit hit);
    void LaunchSelected(int item);
    void DrawResult(ID2D1DeviceContext* dc, const Item& item);
    PixelsPtr ResultIcon(const Result& result);
    D2D1_RECT_F ShortcutRect(int i) const;
    bool ShortcutsShown() const { return m_options.showFooter && !m_shortcuts.empty(); }
    bool QuickVisible() const { return m_options.showQuick && m_query.empty() && !m_showAll && !m_historyMode; }
    float QuickH() const { return QuickVisible() ? 44.f : 0.f; }
    bool IsHidden(int app);
    void RunQuick(int which, int volumeSteps);
    void UpdateTooltips();
    void UpdateDrag(POINT px);
    void FinishDrag(bool drop);
    void RunPower(int index);
    void PositionIme();

    // --- data -----------------------------------------------------------------------------------------------------
    void OnLoaded(WPARAM kind, LPARAM object);
    void RequestData(bool force);
    int FindApp(const std::wstring& id) const;

    // ---------------------------------------------------------------------------------------------------------------
    ViewOptions m_options;
    HWND m_input = nullptr;
    HWND m_visual = nullptr;
    UINT m_taskbarCreated = 0;

    // Device
    ComPtr<ID3D11Device> m_d3d;
    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2d;
    ComPtr<ID2D1DeviceContext> m_resources;   // creates bitmaps and brushes shared by every surface
    ComPtr<IDCompositionDesktopDevice> m_dcomp;
    ComPtr<IDCompositionTarget> m_target;

    // Visuals
    ComPtr<IDCompositionVisual2> m_root, m_container, m_shadow, m_panel, m_background, m_scrollHost, m_scrollLayer,
                                 m_highlight, m_content, m_pressed, m_chrome, m_caret, m_flyout;
    ComPtr<IDCompositionEffectGroup> m_containerEffect;
    ComPtr<IDCompositionScaleTransform> m_openScale, m_pressScale;
    ComPtr<IDCompositionTranslateTransform> m_openTranslate;
    ComPtr<IDCompositionTransform> m_openGroup;
    ComPtr<IDCompositionRectangleClip> m_panelClip, m_scrollClip;

    // Surfaces
    ComPtr<IDCompositionSurface> m_shadowSurface, m_backgroundSurface, m_chromeSurface, m_hlGrid, m_hlList, m_hlLetter,
                                 m_pressedSurface, m_caretSurface, m_flyoutSurface;
    ComPtr<IDCompositionVirtualSurface> m_contentSurface;
    float m_bandTop = 0, m_bandBottom = 0;    // the part of the content surface that holds current pixels
    bool m_bandValid = false;
    UINT m_contentSurfaceH = 0;

    // Resources
    Fonts m_fonts;
    ComPtr<IDWriteTextFormat> m_fmtTitle, m_fmtSearch, m_fmtToggle, m_fmtGridLabel, m_fmtListItem, m_fmtLetter,
                              m_fmtChip, m_fmtUser, m_fmtInitial, m_fmtButton, m_fmtClear, m_fmtEmpty;
    std::unordered_map<UINT32, ComPtr<ID2D1SolidColorBrush>> m_brushes;
    std::unordered_map<const Pixels*, std::pair<PixelsPtr, ComPtr<ID2D1Bitmap>>> m_bitmaps;

    // Geometry of the current opening
    HMONITOR m_monitor = nullptr;
    UINT m_dpi = 96;
    float m_scale = 1;
    float m_panelH = metric::PanelMaxH;        // DIPs
    bool m_fromTop = false;                     // taskbar at the top: the menu hangs down and slides from above
    POINT m_panelPx = {};                       // screen position of the panel's top-left
    UINT m_builtDpi = 0;
    float m_builtH = 0;
    size_t m_builtStyle = 0;                    // hash of the palette and layout the surfaces were drawn with

    // Style of the current opening (ApplyStyle)
    Layout m_lay = {};
    Palette m_pal = LightPalette();
    bool m_acrylic = false;                     // blurred backdrop under a translucent tint
    bool m_translucent = false;                 // tinted background (acrylic or glass)
    std::vector<PowerAction> m_powerItems;
    Backdrop m_backdrop;
    int m_fontBuilt = -1;

    // Data
    Loader m_loader;
    std::shared_ptr<AppList> m_apps;
    std::unordered_map<std::wstring, int> m_appIndex;   // lower-case id -> index
    std::vector<RecentFile> m_recent;
    UserInfo m_user;
    PinStore m_pins;
    bool m_pinsLoaded = false;
    bool m_pinsSeeded = false;
    ULONGLONG m_appsRequestedAt = 0;

    // State (handoff section 10)
    bool m_open = false;
    bool m_visible = false;                     // windows shown; stays true during the close animation
    bool m_showAll = false;
    std::wstring m_query;
    bool m_selectAll = false;
    int m_sel = 0;
    bool m_powerOpen = false;
    int m_flyoutHot = -1;
    float m_scrollY = 0;
    Model m_model;

    Hit m_hot;                                  // the chrome element under the mouse
    Hit m_down;                                 // where the left button went down
    bool m_tracking = false;
    int m_pressedItem = -1;
    bool m_caretOn = true;
    float m_queryWidth = 0;

    // Animation clocks
    bool m_animations = true;
    LARGE_INTEGER m_animStart = {};
    bool m_animOpening = false;
    float m_animFrom[3] = { 0, 0, 0 };          // opacity, translate (DIPs), scale at the start of the animation
    float m_fieldT = 0, m_fieldFrom = 0, m_fieldTo = 0;
    ULONGLONG m_fieldStart = 0;
    ULONGLONG m_closedAt = 0;                   // when a deactivation closed the menu

    // Chrome geometry measured while drawing, for hit-testing
    std::vector<D2D1_RECT_F> m_chipRects;
    float m_toggleW = 0;
    float m_nameW = 0;

    ComPtr<ID2D1StrokeStyle> m_roundStroke;
    HWND m_prevForeground = nullptr;            // restored when the menu closes without launching anything
    bool m_inContextMenu = false;               // a context menu owns the focus; ignore the deactivation
    bool m_deviceLost = false;

    // Extras
    UsageStore m_usage;
    std::vector<ShortcutItem> m_shortcuts;
    unsigned m_shortcutsRequested = ~0u;
    UINT m_shortcutsPx = 0;
    HWND m_tooltip = nullptr;
    int m_tooltipCount = 0;
    std::unordered_map<std::wstring, PixelsPtr> m_resultIcons;   // by target, for Run/Open results

    // File search, the letter index, new apps, the background image
    FileSearch m_files;
    unsigned m_fileQuery = 0;
    std::vector<Result> m_fileResults;
    bool m_letters = false;                     // "All apps" shows the A-Z index instead of the list
    std::vector<std::wstring> m_letterList;
    NewApps m_newApps;
    std::wstring m_imagePath;
    PixelsPtr m_image;
    void JumpToLetter(const std::wstring& letter);

    // Quick settings, hidden apps, search history
    QuickSettings m_quick;
    QuickState m_quickState;
    bool m_quickKnown = false;
    std::vector<D2D1_RECT_F> m_quickRects;
    std::vector<int> m_quickKinds;              // what each chip is: 0 Wi-Fi, 1 Bluetooth, 2 volume, 3 dark mode
    LineStore m_hidden{ L"hidden.txt" };
    LineStore m_history{ L"history.txt" };
    bool m_historyMode = false;                 // the empty search box shows the recent searches
    ComPtr<IDWriteTextFormat> m_fmtIcon;        // Segoe Fluent Icons / MDL2 Assets glyphs

    // Drag and drop in the pinned grid
    POINT m_downPx = {};
    bool m_dragging = false;
    int m_dragFrom = -1;
    POINT m_dragGrab = {};                      // where in the cell it was picked up (pixels)
    bool m_releasingCapture = false;            // OnMouseUp is giving the capture up itself
};

// Runs the menu in the calling process with no shell hooks: opens it at once and returns when it closes.
// For the "rundll32 ShadePatcher.dll,ZZStartMenuPreview" entry point.
// `options` is the rundll32 command line: "all", "power", "left", "search=<text>" (ASCII), any combination.
int RunPreview(const char* options);

} // namespace sm
