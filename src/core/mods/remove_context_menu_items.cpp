//
// remove-context-menu-items - hide unwanted rows from the File Explorer and desktop context menus.
//
// What it does
// ------------
// Windows and the apps installed on it keep adding rows to the right-click menu: "Ask Copilot", "Move to
// OneDrive", "Scan with Microsoft Defender...", "Share", "Open in Terminal", "Edit with Clipchamp" and so on.
// This mod takes a fixed set of on/off switches, one per such row, plus a free-form list of labels, and removes
// the matching rows just before a menu is shown. Rows are recognised by their visible text, in the eleven
// languages the original mod knows, so nothing depends on command ids or on which handler added the row.
//
// Two menus, two hooks
// --------------------
// Windows 11 shows two different context menus and they are built by different code:
//
//   * The WinUI menu (the default right-click menu, with the icon strip at the top) is a Microsoft.UI.Xaml
//     CommandBarFlyout built in Windows.UI.FileExplorer.dll by ContextMenuPresenter. desktop_menu_entry.cpp
//     already reaches the finished flyout by hooking the presenter's RegisterTappedOnShowMoreOptions /
//     HandleDuplicateAccessKeys / SetAccessKeyScope members, which are handed the flyout once every row is in
//     place; the same six optional hooks are used here. The rows are AppBarButtons in the flyout's
//     PrimaryCommands (the icon strip) and SecondaryCommands (the labelled list), and a row with a sub-menu
//     carries a MenuFlyout whose MenuFlyoutItems are checked too. A matching row is removed from its
//     collection and separators left doubled, leading or trailing by that removal are cleaned up.
//
//   * The classic menu ("Show more options", Shift+F10, Shift+right-click, and every menu on a build or a
//     setup where the WinUI menu is off) is an HMENU shown through TrackPopupMenu(Ex). Both exports are
//     hooked; a menu is only touched when its owner window belongs to a shell view (the desktop's
//     SHELLDLL_DefView, a folder window's view or its navigation pane), so taskbar, tray and Start menus are
//     left alone. The classic shell fills sub-menus such as "Send to" and "Open with" lazily, on
//     WM_INITMENUPOPUP, so for the duration of one TrackPopupMenu call a thread-local WH_CALLWNDPROCRET hook
//     re-checks each sub-menu as it is populated; a sub-menu detached by RemoveMenu is destroyed once the
//     tracking call has returned, when nothing can still refer to it.
//
// Settings
// --------
// Eighteen dword switches, HideShare, HideCopyAsPath, ..., HideEditInNotepad (see kOptions), and one string,
// CustomLabels: labels separated by semicolons, compared case-insensitively after the same normalisation the
// built-in table gets; a label ending in '*' matches every row that starts with the text before it. The
// original's per-extension filtering and Alt-to-bypass were not carried over; see the port notes.
//
#define SP_MOD_ID "remove-context-menu-items"
#include "engine/modapi.h"

#include <unknwn.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace winrt::Microsoft::UI::Xaml::Controls;
using winrt::Windows::Foundation::Collections::IObservableVector;
using winrt::Windows::Foundation::Collections::IVector;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// The switches
// ---------------------------------------------------------------------------------------------------------------

enum Option
{
    kShare = 0,
    kCopyAsPath,
    kOpenInTerminal,
    kAddToFavorites,
    kCopilot,                   // "Ask Copilot" and "Ask Microsoft 365 Copilot"
    kRotate,                    // "Rotate left" and "Rotate right"
    kSetAsBackground,
    kGiveAccessTo,
    kRestorePreviousVersions,
    kSendTo,
    kOpenWith,
    kOneDrive,                  // "Move to OneDrive", "Always keep on this device", "Free up space"
    kDefender,
    kDesigner,
    kClipchamp,
    kPinToStart,
    kPinToQuickAccess,
    kEditInNotepad,             // "Edit in Notepad" and "Edit with Notepad++"
    kOptionCount
};

struct OptionInfo
{
    const wchar_t* setting;
    int            defaultValue;
};

// The defaults follow the original: the rows Microsoft adds for its own services are hidden out of the box,
// everything that is part of the shell proper is kept until the user asks.
const OptionInfo kOptions[kOptionCount] =
{
    { L"HideShare",                   0 },
    { L"HideCopyAsPath",              0 },
    { L"HideOpenInTerminal",          0 },
    { L"HideAddToFavorites",          0 },
    { L"HideCopilot",                 1 },
    { L"HideRotate",                  0 },
    { L"HideSetAsBackground",         0 },
    { L"HideGiveAccessTo",            0 },
    { L"HideRestorePreviousVersions", 0 },
    { L"HideSendTo",                  0 },
    { L"HideOpenWith",                0 },
    { L"HideOneDrive",                1 },
    { L"HideDefender",                1 },
    { L"HideDesigner",                1 },
    { L"HideClipchamp",               1 },
    { L"HidePinToStart",              0 },
    { L"HidePinToQuickAccess",        0 },
    { L"HideEditInNotepad",           0 },
};

// ---------------------------------------------------------------------------------------------------------------
// The labels
// ---------------------------------------------------------------------------------------------------------------

// Every spelling of every row the switches cover, taken from the original mod's table: en-US/GB/AU, ja-JP,
// pt-BR, pt-PT, es-MX, cs-CZ, de-DE, tr-TR, pl-PL, fr-FR and ru-RU. Characters outside ASCII are written as
// hex escapes; a literal is split where the next character would otherwise be read as part of the escape.
struct LabelEntry
{
    const wchar_t* text;
    Option         option;
};

const LabelEntry kLabels[] =
{
    // kOneDrive
    { L"Move to OneDrive", kOneDrive },  // en-US, en-GB, en-AU
    { L"OneDrive \x306B\x79FB\x52D5", kOneDrive },  // ja-JP
    { L"Mover para o OneDrive", kOneDrive },  // pt-BR, pt-PT
    { L"Mover a OneDrive", kOneDrive },  // es-MX
    { L"P\x0159" L"esunout na OneDrive", kOneDrive },  // cs-CZ
    { L"Auf OneDrive verschieben", kOneDrive },  // de-DE
    { L"OneDrive'a ta\x015F\x0131", kOneDrive },  // tr-TR
    { L"Przenie\x015B do us\x0142ugi OneDrive", kOneDrive },  // pl-PL
    { L"D\x00E9placer vers OneDrive", kOneDrive },  // fr-FR
    { L"Always keep on this device", kOneDrive },  // en-US, en-GB, en-AU
    { L"\x3053\x306E\x30C7\x30D0\x30A4\x30B9\x4E0A\x306B\x5E38\x306B\x4FDD\x6301\x3059\x308B", kOneDrive },  // ja-JP
    { L"Sempre manter neste dispositivo", kOneDrive },  // pt-BR
    { L"Manter sempre neste dispositivo", kOneDrive },  // pt-PT
    { L"Mantener siempre en este dispositivo", kOneDrive },  // es-MX
    { L"V\x017E" L"dy ponechat na tomto za\x0159\x00EDzen\x00ED", kOneDrive },  // cs-CZ
    { L"Immer auf diesem Ger\x00E4t behalten", kOneDrive },  // de-DE
    { L"Her zaman bu cihazda tut", kOneDrive },  // tr-TR
    { L"Zawsze przechowuj na tym urz\x0105" L"dzeniu", kOneDrive },  // pl-PL
    { L"Toujours conserver sur cet appareil", kOneDrive },  // fr-FR
    { L"Free up space", kOneDrive },  // en-US, en-GB, en-AU
    { L"\x7A7A\x304D\x9818\x57DF\x3092\x5897\x3084\x3059", kOneDrive },  // ja-JP
    { L"Liberar espa\x00E7o", kOneDrive },  // pt-BR
    { L"Libertar espa\x00E7o", kOneDrive },  // pt-PT
    { L"Liberar espacio", kOneDrive },  // es-MX
    { L"Uvolnit m\x00EDsto", kOneDrive },  // cs-CZ
    { L"Bereinigen", kOneDrive },  // de-DE
    { L"Alan bo\x015F" L"alt", kOneDrive },  // tr-TR
    { L"Zwolnij miejsce", kOneDrive },  // pl-PL
    { L"Lib\x00E9rer de l'espace", kOneDrive },  // fr-FR

    // kCopilot
    { L"Ask Copilot", kCopilot },  // en-US, en-GB, en-AU
    { L"Copilot \x3068\x30C1\x30E3\x30C3\x30C8\x3059\x308B", kCopilot },  // ja-JP
    { L"Perguntar ao Copilot", kCopilot },  // pt-BR, pt-PT
    { L"Preguntar a Copilot", kCopilot },  // es-MX
    { L"Zeptat se Copilota", kCopilot },  // cs-CZ
    { L"Copilot fragen", kCopilot },  // de-DE
    { L"Copilot'a sor", kCopilot },  // tr-TR
    { L"Zapytaj aplikacj\x0119 Copilot", kCopilot },  // pl-PL
    { L"Demander \x00E0 Copilot", kCopilot },  // fr-FR
    { L"Ask Microsoft 365 Copilot", kCopilot },  // en-US, en-GB, en-AU, ja-JP
    { L"Perguntar ao Microsoft 365 Copilot", kCopilot },  // pt-BR, pt-PT
    { L"Preguntar a Microsoft 365 Copilot", kCopilot },  // es-MX
    { L"Zeptat se Microsoft 365 Copilota", kCopilot },  // cs-CZ
    { L"Microsoft 365 Copilot fragen", kCopilot },  // de-DE
    { L"Microsoft 365 Copilot'a sor", kCopilot },  // tr-TR
    { L"Zapytaj aplikacj\x0119 Microsoft 365 Copilot", kCopilot },  // pl-PL
    { L"Demander \x00E0 Microsoft 365 Copilot", kCopilot },  // fr-FR

    // kDefender
    { L"Scan with Microsoft Defender...", kDefender },  // en-US, en-GB, en-AU
    { L"Microsoft Defender\x3067\x30B9\x30AD\x30E3\x30F3\x3059\x308B...", kDefender },  // ja-JP
    { L"Verificar com o Microsoft Defender...", kDefender },  // pt-BR
    { L"Analisar com o Microsoft Defender...", kDefender },  // pt-PT
    { L"Analizar con Microsoft Defender...", kDefender },  // es-MX
    { L"Prohledat pomoc\x00ED programu Microsoft Defender...", kDefender },  // cs-CZ
    { L"Mit Microsoft Defender \x00FC" L"berpr\x00FC" L"fen...", kDefender },  // de-DE
    { L"Microsoft Defender ile tara...", kDefender },  // tr-TR
    { L"Skanuj za pomoc\x0105 programu Microsoft Defender...", kDefender },  // pl-PL
    { L"Analyser avec Microsoft Defender...", kDefender },  // fr-FR
    { L"\x041F\x0440\x043E\x0432\x0435\x0440\x043A\x0430 \x0441 \x0438\x0441\x043F\x043E\x043B\x044C\x0437\x043E\x0432\x0430\x043D\x0438\x0435\x043C Microsoft Defender...", kDefender },  // ru-RU

    // kDesigner
    { L"Create with Designer", kDesigner },  // en-US, en-GB, en-AU
    { L"Designer\x3067\x4F5C\x6210", kDesigner },  // ja-JP
    { L"Criar com o Designer", kDesigner },  // pt-BR, pt-PT
    { L"Crear con Designer", kDesigner },  // es-MX
    { L"Vytvo\x0159it pomoc\x00ED Designeru", kDesigner },  // cs-CZ
    { L"Mit Designer erstellen", kDesigner },  // de-DE
    { L"Designer ile olu\x015Ftur", kDesigner },  // tr-TR
    { L"Utw\x00F3rz za pomoc\x0105 aplikacji Designer", kDesigner },  // pl-PL
    { L"Cr\x00E9" L"er avec Designer", kDesigner },  // fr-FR

    // kClipchamp
    { L"Edit with Clipchamp", kClipchamp },  // en-US, en-GB, en-AU
    { L"Clipchamp\x3067\x7DE8\x96C6", kClipchamp },  // ja-JP
    { L"Editar com o Clipchamp", kClipchamp },  // pt-BR, pt-PT
    { L"Editar con Clipchamp", kClipchamp },  // es-MX
    { L"Upravit pomoc\x00ED Clipchampu", kClipchamp },  // cs-CZ
    { L"Mit Clipchamp bearbeiten", kClipchamp },  // de-DE
    { L"Clipchamp ile d\x00FCzenle", kClipchamp },  // tr-TR
    { L"Edytuj za pomoc\x0105 aplikacji Clipchamp", kClipchamp },  // pl-PL
    { L"Modifier avec Clipchamp", kClipchamp },  // fr-FR
    { L"\x0420\x0435\x0434\x0430\x043A\x0442\x0438\x0440\x043E\x0432\x0430\x0442\x044C \x0432 Clipchamp", kClipchamp },  // ru-RU

    // kOpenWith
    { L"Open with", kOpenWith },  // en-US, en-GB, en-AU
    { L"\x30D7\x30ED\x30B0\x30E9\x30E0\x304B\x3089\x958B\x304F", kOpenWith },  // ja-JP
    { L"Abrir com", kOpenWith },  // pt-BR, pt-PT
    { L"Abrir con", kOpenWith },  // es-MX
    { L"Otev\x0159\x00EDt v aplikaci", kOpenWith },  // cs-CZ
    { L"\x00D6" L"ffnen mit", kOpenWith },  // de-DE
    { L"Birlikte a\x00E7", kOpenWith },  // tr-TR
    { L"Otw\x00F3rz za pomoc\x0105", kOpenWith },  // pl-PL
    { L"Ouvrir avec", kOpenWith },  // fr-FR
    { L"\x041E\x0442\x043A\x0440\x044B\x0442\x044C \x0441 \x043F\x043E\x043C\x043E\x0449\x044C\x044E", kOpenWith },  // ru-RU

    // kSendTo
    { L"Send to", kSendTo },  // en-US, en-GB, en-AU
    { L"\x9001\x308B", kSendTo },  // ja-JP
    { L"Enviar para", kSendTo },  // pt-BR, pt-PT
    { L"Enviar a", kSendTo },  // es-MX
    { L"Odeslat do", kSendTo },  // cs-CZ
    { L"Senden an", kSendTo },  // de-DE
    { L"G\x00F6nder", kSendTo },  // tr-TR
    { L"Wy\x015Blij do", kSendTo },  // pl-PL
    { L"Envoyer vers", kSendTo },  // fr-FR
    { L"\x041E\x0442\x043F\x0440\x0430\x0432\x0438\x0442\x044C", kSendTo },  // ru-RU

    // kShare
    { L"Share", kShare },  // en-US, en-GB, en-AU
    { L"\x5171\x6709", kShare },  // ja-JP
    { L"Compartilhar", kShare },  // pt-BR
    { L"Partilhar", kShare },  // pt-PT
    { L"Compartir", kShare },  // es-MX
    { L"Sd\x00EDlet", kShare },  // cs-CZ
    { L"Freigabe", kShare },  // de-DE
    { L"Payla\x015F", kShare },  // tr-TR
    { L"Udost\x0119pnij", kShare },  // pl-PL
    { L"Partager", kShare },  // fr-FR
    { L"\x041F\x043E\x0434\x0435\x043B\x0438\x0442\x044C\x0441\x044F", kShare },  // ru-RU

    // kCopyAsPath
    { L"Copy as path", kCopyAsPath },  // en-US, en-GB, en-AU
    { L"\x30D1\x30B9\x306E\x30B3\x30D4\x30FC", kCopyAsPath },  // ja-JP
    { L"Copiar como caminho", kCopyAsPath },  // pt-BR, pt-PT
    { L"Copiar como ruta de acceso", kCopyAsPath },  // es-MX
    { L"Kop\x00EDrovat jako cestu", kCopyAsPath },  // cs-CZ
    { L"Als Pfad kopieren", kCopyAsPath },  // de-DE
    { L"Yolu kopyala", kCopyAsPath },  // tr-TR
    { L"Yol olarak kopyala", kCopyAsPath },  // tr-TR (build 26200 spelling)
    { L"Kopiuj jako \x015B" L"cie\x017Ck\x0119", kCopyAsPath },  // pl-PL
    { L"Copier comme chemin", kCopyAsPath },  // fr-FR
    { L"\x041A\x043E\x043F\x0438\x0440\x043E\x0432\x0430\x0442\x044C \x043A\x0430\x043A \x043F\x0443\x0442\x044C", kCopyAsPath },  // ru-RU

    // kAddToFavorites
    { L"Add to Favorites", kAddToFavorites },  // en-US
    { L"\x304A\x6C17\x306B\x5165\x308A\x306B\x8FFD\x52A0", kAddToFavorites },  // ja-JP
    { L"Add to Favourites", kAddToFavorites },  // en-GB, en-AU
    { L"Adicionar aos Favoritos", kAddToFavorites },  // pt-BR, pt-PT
    { L"Agregar a favoritos", kAddToFavorites },  // es-MX
    { L"P\x0159idat k obl\x00ED" L"ben\x00FDm polo\x017Ek\x00E1m", kAddToFavorites },  // cs-CZ
    { L"Zu Favoriten hinzuf\x00FCgen", kAddToFavorites },  // de-DE
    { L"S\x0131k kullan\x0131lanlara ekle", kAddToFavorites },  // tr-TR
    { L"Dodaj do ulubionych", kAddToFavorites },  // pl-PL
    { L"Ajouter aux favoris", kAddToFavorites },  // fr-FR
    { L"\x0414\x043E\x0431\x0430\x0432\x0438\x0442\x044C \x0432 \x0438\x0437\x0431\x0440\x0430\x043D\x043D\x043E\x0435", kAddToFavorites },  // ru-RU

    // kPinToQuickAccess
    { L"Pin to Quick access", kPinToQuickAccess },  // en-US, en-GB, en-AU
    { L"\x30AF\x30A4\x30C3\x30AF \x30A2\x30AF\x30BB\x30B9\x306B\x30D4\x30F3\x7559\x3081\x3059\x308B", kPinToQuickAccess },  // ja-JP
    { L"Fixar no Acesso R\x00E1pido", kPinToQuickAccess },  // pt-BR
    { L"Afixar no Acesso r\x00E1pido", kPinToQuickAccess },  // pt-PT
    { L"Anclar a Acceso r\x00E1pido", kPinToQuickAccess },  // es-MX
    { L"P\x0159ipnout na Rychl\x00FD p\x0159\x00EDstup", kPinToQuickAccess },  // cs-CZ
    { L"An Schnellzugriff anheften", kPinToQuickAccess },  // de-DE
    { L"H\x0131zl\x0131 eri\x015Fime sabitle", kPinToQuickAccess },  // tr-TR
    { L"Przypnij do obszaru Szybki dost\x0119p", kPinToQuickAccess },  // pl-PL
    { L"\x00C9pingler \x00E0 Acc\x00E8s rapide", kPinToQuickAccess },  // fr-FR
    { L"\x0417\x0430\x043A\x0440\x0435\x043F\x0438\x0442\x044C \x043D\x0430 \x043F\x0430\x043D\x0435\x043B\x0438 \x0431\x044B\x0441\x0442\x0440\x043E\x0433\x043E \x0434\x043E\x0441\x0442\x0443\x043F\x0430", kPinToQuickAccess },  // ru-RU

    // kPinToStart
    { L"Pin to Start", kPinToStart },  // en-US, en-GB, en-AU
    { L"\x30B9\x30BF\x30FC\x30C8\x306B\x30D4\x30F3\x7559\x3081\x3059\x308B", kPinToStart },  // ja-JP
    { L"Fixar em Iniciar", kPinToStart },  // pt-BR
    { L"Afixar no Iniciar", kPinToStart },  // pt-PT
    { L"Anclar a Inicio", kPinToStart },  // es-MX
    { L"P\x0159ipnout na Start", kPinToStart },  // cs-CZ
    { L"An \\\"Start\\\" anheften", kPinToStart },  // de-DE
    { L"Ba\x015Flat'a sabitle", kPinToStart },  // tr-TR
    { L"Ba\x015Flang\x0131\x00E7'a sabitle", kPinToStart },  // tr-TR (build 26200 spelling)
    { L"Przypnij do menu Start", kPinToStart },  // pl-PL
    { L"\x00C9pingler au menu D\x00E9marrer", kPinToStart },  // fr-FR
    { L"\x0417\x0430\x043A\x0440\x0435\x043F\x0438\x0442\x044C \x0432 \x043C\x0435\x043D\x044E \\\"\x041F\x0443\x0441\x043A\\\"", kPinToStart },  // ru-RU

    // kGiveAccessTo
    { L"Give access to", kGiveAccessTo },  // en-US, en-GB, en-AU
    { L"\x30A2\x30AF\x30BB\x30B9\x3092\x8A31\x53EF\x3059\x308B", kGiveAccessTo },  // ja-JP
    { L"Conceder acesso a", kGiveAccessTo },  // pt-BR, pt-PT
    { L"Dar acceso a", kGiveAccessTo },  // es-MX
    { L"Poskytnout p\x0159\x00EDstup k", kGiveAccessTo },  // cs-CZ
    { L"Freigeben f\x00FCr", kGiveAccessTo },  // de-DE
    { L"Eri\x015Fim ver", kGiveAccessTo },  // tr-TR
    { L"Udziel dost\x0119pu do", kGiveAccessTo },  // pl-PL
    { L"Donner l'acc\x00E8s \x00E0", kGiveAccessTo },  // fr-FR
    { L"\x041F\x0440\x0435\x0434\x043E\x0441\x0442\x0430\x0432\x0438\x0442\x044C \x0434\x043E\x0441\x0442\x0443\x043F \x043A", kGiveAccessTo },  // ru-RU

    // kRestorePreviousVersions
    { L"Restore previous versions", kRestorePreviousVersions },  // en-US, en-GB, en-AU
    { L"\x4EE5\x524D\x306E\x30D0\x30FC\x30B8\x30E7\x30F3\x306E\x5FA9\x5143", kRestorePreviousVersions },  // ja-JP
    { L"Restaurar vers\x00F5" L"es anteriores", kRestorePreviousVersions },  // pt-BR, pt-PT
    { L"Restaurar versiones anteriores", kRestorePreviousVersions },  // es-MX
    { L"Obnovit p\x0159" L"edchoz\x00ED verze", kRestorePreviousVersions },  // cs-CZ
    { L"Vorg\x00E4ngerversionen wiederhestellen", kRestorePreviousVersions },  // de-DE
    { L"\x00D6nceki s\x00FCr\x00FCmleri geri y\x00FCkle", kRestorePreviousVersions },  // tr-TR
    { L"Przywr\x00F3\x0107 poprzednie wersje", kRestorePreviousVersions },  // pl-PL
    { L"Restaurer les versions pr\x00E9" L"c\x00E9" L"dentes", kRestorePreviousVersions },  // fr-FR
    { L"\x0412\x043E\x0441\x0441\x0442\x0430\x043D\x043E\x0432\x0438\x0442\x044C \x043F\x0440\x0435\x0436\x043D\x044E\x044E \x0432\x0435\x0440\x0441\x0438\x044E", kRestorePreviousVersions },  // ru-RU

    // kRotate
    { L"Rotate right", kRotate },  // en-US, en-GB, en-AU
    { L"\x53F3\x306B\x56DE\x8EE2", kRotate },  // ja-JP
    { L"Girar para a direita", kRotate },  // pt-BR
    { L"Rodar para a direita", kRotate },  // pt-PT
    { L"Girar a la derecha", kRotate },  // es-MX
    { L"Oto\x010Dit doprava", kRotate },  // cs-CZ
    { L"Nach rechts drehen", kRotate },  // de-DE
    { L"Sa\x011F" L"a d\x00F6nd\x00FCr", kRotate },  // tr-TR
    { L"Obr\x00F3\x0107 w prawo", kRotate },  // pl-PL
    { L"Faire pivoter vers la droite", kRotate },  // fr-FR
    { L"\x041F\x043E\x0432\x0435\x0440\x043D\x0443\x0442\x044C \x0432\x043F\x0440\x0430\x0432\x043E", kRotate },  // ru-RU
    { L"Rotate left", kRotate },  // en-US, en-GB, en-AU
    { L"\x5DE6\x306B\x56DE\x8EE2", kRotate },  // ja-JP
    { L"Girar para a esquerda", kRotate },  // pt-BR
    { L"Rodar para a esquerda", kRotate },  // pt-PT
    { L"Girar a la izquierda", kRotate },  // es-MX
    { L"Oto\x010Dit doleva", kRotate },  // cs-CZ
    { L"Nach links drehen", kRotate },  // de-DE
    { L"Sola d\x00F6nd\x00FCr", kRotate },  // tr-TR
    { L"Obr\x00F3\x0107 w lewo", kRotate },  // pl-PL
    { L"Faire pivoter vers la gauche", kRotate },  // fr-FR
    { L"\x041F\x043E\x0432\x0435\x0440\x043D\x0443\x0442\x044C \x0432\x043B\x0435\x0432\x043E", kRotate },  // ru-RU

    // kSetAsBackground
    { L"Set as desktop background", kSetAsBackground },  // en-US, en-GB, en-AU
    { L"\x30C7\x30B9\x30AF\x30C8\x30C3\x30D7\x306E\x80CC\x666F\x3068\x3057\x3066\x8A2D\x5B9A", kSetAsBackground },  // ja-JP
    { L"Definir como plano de fundo da \x00E1rea de trabalho", kSetAsBackground },  // pt-BR
    { L"Definir como fundo do ambiente de trabalho", kSetAsBackground },  // pt-PT
    { L"Establecer como fondo de escritorio", kSetAsBackground },  // es-MX
    { L"Nastavit jako pozad\x00ED plochy", kSetAsBackground },  // cs-CZ
    { L"Als Desktophintergrund festlegen", kSetAsBackground },  // de-DE
    { L"Masa\x00FCst\x00FC arka plan\x0131 olarak ayarla", kSetAsBackground },  // tr-TR
    { L"Ustaw jako t\x0142o pulpitu", kSetAsBackground },  // pl-PL
    { L"D\x00E9" L"finir comme arri\x00E8re-plan du Bureau", kSetAsBackground },  // fr-FR
    { L"\x0421\x0434\x0435\x043B\x0430\x0442\x044C \x0444\x043E\x043D\x043E\x043C \x0440\x0430\x0431\x043E\x0447\x0435\x0433\x043E \x0441\x0442\x043E\x043B\x0430", kSetAsBackground },  // ru-RU

    // kEditInNotepad
    { L"Edit in Notepad", kEditInNotepad },  // en-US, en-GB, en-AU
    { L"\x30E1\x30E2\x5E33\x3067\x7DE8\x96C6", kEditInNotepad },  // ja-JP
    { L"Editar no Bloco de Notas", kEditInNotepad },  // pt-BR, pt-PT
    { L"Editar en el Bloc de notas", kEditInNotepad },  // es-MX
    { L"Upravit v Pozn\x00E1mkov\x00E9m bloku", kEditInNotepad },  // cs-CZ
    { L"Im Editor bearbeiten", kEditInNotepad },  // de-DE
    { L"Not Defteri'nde d\x00FCzenle", kEditInNotepad },  // tr-TR
    { L"Not Defteri'nde d\x00FCzenleme", kEditInNotepad },  // tr-TR (build 26200 spelling)
    { L"Edytuj w Notatniku", kEditInNotepad },  // pl-PL
    { L"Modifier dans le Bloc-notes", kEditInNotepad },  // fr-FR
    { L"\x0418\x0437\x043C\x0435\x043D\x0438\x0442\x044C \x0432 \x0411\x043B\x043E\x043A\x043D\x043E\x0442\x0435", kEditInNotepad },  // ru-RU
    { L"Edit with Notepad++", kEditInNotepad },  // en-US, en-GB, en-AU, ja-JP
    { L"Editar no Notepad++", kEditInNotepad },  // pt-BR, pt-PT
    { L"Editar con Notepad++", kEditInNotepad },  // es-MX
    { L"Upravit v aplikaci Notepad++", kEditInNotepad },  // cs-CZ
    { L"Mit Notepad++ bearbeiten", kEditInNotepad },  // de-DE
    { L"Notepad++ ile d\x00FCzenle", kEditInNotepad },  // tr-TR
    { L"Edytuj w Notepad++", kEditInNotepad },  // pl-PL
    { L"Modifier avec Notepad++", kEditInNotepad },  // fr-FR
    { L"\x0420\x0435\x0434\x0430\x043A\x0442\x0438\x0440\x043E\x0432\x0430\x0442\x044C \x0432 Notepad++", kEditInNotepad },  // ru-RU

    // kOpenInTerminal
    { L"Open in Terminal", kOpenInTerminal },  // en-US, en-GB, en-AU, ja-JP
    { L"Abrir no Terminal", kOpenInTerminal },  // pt-BR, pt-PT
    { L"Abrir en Terminal", kOpenInTerminal },  // es-MX
    { L"Otev\x0159\x00EDt v termin\x00E1lu", kOpenInTerminal },  // cs-CZ
    { L"In Terminal \x00F6" L"ffnen", kOpenInTerminal },  // de-DE
    { L"Terminal'de a\x00E7", kOpenInTerminal },  // tr-TR
    { L"Terminalde a\x00E7", kOpenInTerminal },  // tr-TR (build 26200 spelling)
    { L"Otw\x00F3rz w Terminalu", kOpenInTerminal },  // pl-PL
    { L"Ouvrir dans le Terminal", kOpenInTerminal },  // fr-FR
    { L"\x041E\x0442\x043A\x0440\x044B\x0442\x044C \x0432 \x0422\x0435\x0440\x043C\x0438\x043D\x0430\x043B\x0435", kOpenInTerminal },  // ru-RU
};

// Normalised label -> option, built once in Init from kLabels.
std::unordered_map<std::wstring, int> g_labelIndex;

// A menu's text is not compared as-is: a classic item may carry an "&" access-key marker and a "\tCtrl+C"
// accelerator column, the shell spells "..." both as three dots and as U+2026, and the case of the first letter
// varies between the two menus. Everything is brought to one form before comparing, table and menu alike.
// `menuText` is TRUE for text read from an HMENU, where a lone '&' is a marker rather than a character.
std::wstring Normalize(const wchar_t* text, bool menuText)
{
    std::wstring out;
    if (!text)
    {
        return out;
    }

    for (const wchar_t* p = text; *p && *p != L'\t'; ++p)
    {
        wchar_t c = *p;
        if (menuText && c == L'&')
        {
            if (p[1] == L'&')
            {
                out += L'&';
                ++p;
            }
            continue;
        }
        if (c == L'\x2026')
        {
            out += L"...";
            continue;
        }
        out += c;
    }

    // CharLowerBuffW covers the whole of Unicode; towlower in the C locale only folds A-Z, which would leave the
    // Turkish, Polish and Czech rows unmatched.
    if (!out.empty())
    {
        CharLowerBuffW(&out[0], (DWORD)out.size());
    }

    size_t start = out.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos)
    {
        return std::wstring();
    }
    size_t end = out.find_last_not_of(L" \t\r\n");
    return out.substr(start, end - start + 1);
}

void BuildLabelIndex()
{
    g_labelIndex.clear();
    g_labelIndex.reserve(ARRAYSIZE(kLabels));
    for (const LabelEntry& entry : kLabels)
    {
        std::wstring key = Normalize(entry.text, false);
        if (!key.empty())
        {
            // The first spelling wins when two languages share a label; they always map to the same option.
            g_labelIndex.emplace(std::move(key), (int)entry.option);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The rules in force
// ---------------------------------------------------------------------------------------------------------------

struct Rules
{
    bool                      hide[kOptionCount] = {};
    std::vector<std::wstring> custom;      // normalised; a trailing '*' means "starts with"
};

std::mutex        g_rulesLock;
Rules             g_rules;
std::atomic<bool> g_anything{ false };    // FALSE when no switch is on and no label is listed: nothing to do

// A copy for the duration of one menu, so a settings change half-way through a menu cannot make the top level
// and its sub-menus disagree, and the lock is never held while XAML or user32 is called.
Rules SnapshotRules()
{
    std::lock_guard<std::mutex> lock(g_rulesLock);
    return g_rules;
}

bool MatchesPattern(const std::wstring& label, const std::wstring& pattern)
{
    if (pattern.empty())
    {
        return false;
    }
    if (pattern.back() == L'*')
    {
        size_t n = pattern.size() - 1;
        return label.size() >= n && label.compare(0, n, pattern, 0, n) == 0;
    }
    return label == pattern;
}

// The user's own labels are checked first: they are additive and apply even to a row whose switch is off.
bool ShouldHide(const std::wstring& normalized, const Rules& rules)
{
    if (normalized.empty())
    {
        return false;
    }
    for (const std::wstring& pattern : rules.custom)
    {
        if (MatchesPattern(normalized, pattern))
        {
            return true;
        }
    }
    auto it = g_labelIndex.find(normalized);
    return it != g_labelIndex.end() && rules.hide[it->second];
}

void LoadSettings()
{
    Rules rules;
    int switchedOn = 0;
    for (int i = 0; i < kOptionCount; ++i)
    {
        rules.hide[i] = SP_GetIntSetting(kOptions[i].setting, kOptions[i].defaultValue) != 0;
        if (rules.hide[i])
        {
            ++switchedOn;
        }
    }

    // "Share;Open in Terminal;Pin to*" -> three patterns. A bare "*" would empty every menu, which is never what
    // was meant, so it is refused rather than obeyed.
    wchar_t buffer[4096];
    SP_GetStringSetting(L"CustomLabels", buffer, ARRAYSIZE(buffer), L"");
    wchar_t* piece = buffer;
    for (wchar_t* p = buffer; ; ++p)
    {
        if (*p == L';' || *p == L'\0')
        {
            bool last = (*p == L'\0');
            *p = L'\0';
            std::wstring pattern = Normalize(piece, false);
            if (pattern == L"*")
            {
                SP_LogError(L"A custom label of just '*' would hide every row; it is ignored");
            }
            else if (!pattern.empty())
            {
                rules.custom.push_back(std::move(pattern));
            }
            if (last)
            {
                break;
            }
            piece = p + 1;
        }
    }

    const unsigned customCount = (unsigned)rules.custom.size();
    const bool anything = switchedOn > 0 || customCount > 0;
    {
        std::lock_guard<std::mutex> lock(g_rulesLock);
        g_rules = std::move(rules);
    }
    g_anything.store(anything, std::memory_order_relaxed);

    SP_Log(L"%d of %d built-in rows hidden, %u custom label(s)%s", switchedOn, (int)kOptionCount,
           customCount, anything ? L"" : L"; nothing to hide, menus are left alone");
}

// ---------------------------------------------------------------------------------------------------------------
// The WinUI menu
// ---------------------------------------------------------------------------------------------------------------

// The text of a row. The Label is what the presenter sets on most builds; a row that carries its text only as
// its automation name (which is what the screen reader and UI Automation report, and what build 26200's presenter
// leaves behind for some rows) is read through that instead.
std::wstring LabelOf(ICommandBarElement const& element)
{
    std::wstring label;
    if (auto button = element.try_as<AppBarButton>())
    {
        label = button.Label();
    }
    else if (auto toggle = element.try_as<AppBarToggleButton>())
    {
        label = toggle.Label();
    }
    if (label.empty())
    {
        if (auto dependency = element.try_as<winrt::Microsoft::UI::Xaml::DependencyObject>())
        {
            label = winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::GetName(dependency);
        }
    }
    return label;
}

// Separators left touching each other, or at either end, after rows between them went away.
template <typename Collection, typename IsSeparator>
void TidySeparators(Collection const& items, IsSeparator isSeparator)
{
    bool previousWasSeparator = false;
    for (int i = (int)items.Size() - 1; i >= 0; --i)
    {
        bool separator = isSeparator(items.GetAt(i));
        if (separator && previousWasSeparator)
        {
            items.RemoveAt(i);
            continue;
        }
        previousWasSeparator = separator;
    }
    if (items.Size() > 0 && isSeparator(items.GetAt(0)))
    {
        items.RemoveAt(0);
    }
    if (items.Size() > 0 && isSeparator(items.GetAt(items.Size() - 1)))
    {
        items.RemoveAt(items.Size() - 1);
    }
}

// The rows of a sub-menu ("Open with", "Send to" and whatever else carries a MenuFlyout).
int FilterFlyoutItems(IVector<MenuFlyoutItemBase> const& items, const Rules& rules, int depth)
{
    if (!items || depth > 4)
    {
        return 0;
    }

    int removed = 0;
    for (int i = (int)items.Size() - 1; i >= 0; --i)
    {
        MenuFlyoutItemBase item = items.GetAt(i);
        std::wstring label;
        MenuFlyoutSubItem sub{ nullptr };

        if (auto plain = item.try_as<MenuFlyoutItem>())
        {
            label = plain.Text();
        }
        else if ((sub = item.try_as<MenuFlyoutSubItem>()))
        {
            label = sub.Text();
        }

        if (!label.empty() && ShouldHide(Normalize(label.c_str(), false), rules))
        {
            SP_LogDebug(L"Hiding sub-menu row \"%s\"", label.c_str());
            items.RemoveAt(i);
            ++removed;
            continue;
        }
        if (sub)
        {
            removed += FilterFlyoutItems(sub.Items(), rules, depth + 1);
        }
    }

    if (removed)
    {
        TidySeparators(items, [](MenuFlyoutItemBase const& e) { return e.try_as<MenuFlyoutSeparator>() != nullptr; });
    }
    return removed;
}

bool IsCommandSeparator(ICommandBarElement const& e)
{
    return e.try_as<AppBarSeparator>() != nullptr;
}

// A row whose handler is still answering shows a placeholder ("Loading...") when the menu is handed over; the
// real text lands in its Label a moment later, after the hooks have run. Every row that was not hidden is
// watched for that, and taken out the moment its text turns into one of the hidden labels. The collection is
// held weakly so the watch cannot keep a menu alive, and the watch is dropped once it has done its work.
void WatchLateLabel(IObservableVector<ICommandBarElement> const& commands, AppBarButton const& button)
{
    auto weakCommands = winrt::make_weak(commands);
    auto token = std::make_shared<int64_t>(0);
    *token = button.RegisterPropertyChangedCallback(AppBarButton::LabelProperty(),
        [weakCommands, token](winrt::Microsoft::UI::Xaml::DependencyObject const& sender,
                              winrt::Microsoft::UI::Xaml::DependencyProperty const&)
        {
            try
            {
                auto row = sender.as<AppBarButton>();
                std::wstring label{ row.Label() };
                if (label.empty())
                {
                    return;
                }
                if (!ShouldHide(Normalize(label.c_str(), false), SnapshotRules()))
                {
                    return;     // a placeholder may change more than once; keep watching
                }

                auto commands = weakCommands.get();
                uint32_t index = 0;
                if (commands && commands.IndexOf(row.as<ICommandBarElement>(), index))
                {
                    SP_Log(L"Late row \"%s\" hidden from the WinUI menu", label.c_str());
                    commands.RemoveAt(index);
                    TidySeparators(commands, IsCommandSeparator);
                }
                row.UnregisterPropertyChangedCallback(AppBarButton::LabelProperty(), *token);
            }
            catch (...)
            {
            }
        });
}

// One of the flyout's two collections: the icon strip or the labelled list.
int FilterCommands(IObservableVector<ICommandBarElement> const& commands, const Rules& rules)
{
    if (!commands)
    {
        return 0;
    }

    int removed = 0;
    for (int i = (int)commands.Size() - 1; i >= 0; --i)
    {
        ICommandBarElement element = commands.GetAt(i);
        std::wstring label = LabelOf(element);
        if (SP_LogEnabled(SP_LOG_DEBUG))
        {
            SP_LogDebug(L"WinUI row %d: %s \"%s\"", i, winrt::get_class_name(element).c_str(), label.c_str());
        }

        if (!label.empty() && ShouldHide(Normalize(label.c_str(), false), rules))
        {
            SP_LogDebug(L"Hiding row \"%s\"", label.c_str());
            commands.RemoveAt(i);
            ++removed;
            continue;
        }

        if (auto button = element.try_as<AppBarButton>())
        {
            if (auto flyout = button.Flyout())
            {
                if (auto menu = flyout.try_as<MenuFlyout>())
                {
                    removed += FilterFlyoutItems(menu.Items(), rules, 0);
                }
            }
            if (g_anything.load(std::memory_order_relaxed))
            {
                WatchLateLabel(commands, button);
            }
        }
    }

    if (removed)
    {
        TidySeparators(commands, IsCommandSeparator);
    }
    return removed;
}

// A C++/WinRT object is one pointer wide but has a destructor, so the x64 ABI hands it over as a pointer to a
// copy the caller made rather than in a register. Which of the two the argument actually is cannot be told by
// looking, so both readings are tried and the one that answers like a menu wins.
bool TryReadFlyout(void* candidate, CommandBarFlyout& out)
{
    if (!candidate)
    {
        return false;
    }
    try
    {
        CommandBarFlyout flyout{ nullptr };
        winrt::copy_from_abi(flyout, candidate);
        if (!flyout || !flyout.SecondaryCommands())
        {
            return false;
        }
        out = flyout;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void OnMenuReady(void* flyoutArg)
{
    if (!g_anything.load(std::memory_order_relaxed))
    {
        return;
    }

    CommandBarFlyout menu{ nullptr };
    if (!TryReadFlyout(flyoutArg ? *(void**)flyoutArg : nullptr, menu) && !TryReadFlyout(flyoutArg, menu))
    {
        SP_LogDebug(L"The argument was not a menu this build understands");
        return;
    }

    try
    {
        const Rules rules = SnapshotRules();
        int removed = FilterCommands(menu.PrimaryCommands(), rules);
        removed += FilterCommands(menu.SecondaryCommands(), rules);
        if (removed)
        {
            SP_Log(L"%d row(s) hidden from the WinUI menu; %u remain in the list", removed,
                   menu.SecondaryCommands().Size());
        }
        else
        {
            SP_LogDebug(L"Nothing to hide in this WinUI menu");
        }
    }
    catch (const winrt::hresult_error& error)
    {
        SP_LogError(L"The WinUI menu could not be filtered: %s", error.message().c_str());
    }
    catch (...)
    {
        SP_LogError(L"The WinUI menu could not be filtered");
    }
}

// This build ships two implementations of the presenter side by side, ContextMenuPresenter and
// ContextMenuPresenter_Old, and which one serves a given menu is decided at run time. Each has three members
// that are handed the finished menu; all six are hooked, every one optional, and a second pass over a menu that
// was already filtered removes nothing, so it does not matter how many of them fire.
using MenuHandler_t = void(__fastcall*)(void* self, void* flyout);

constexpr int kPresenterHookCount = 6;
MenuHandler_t g_presenterOriginals[kPresenterHookCount] = {};

#define SP_PRESENTER_HOOK(index)                                              \
    void __fastcall PresenterHook##index(void* self, void* flyout)            \
    {                                                                         \
        OnMenuReady(flyout);                                                  \
        if (g_presenterOriginals[index])                                      \
        {                                                                     \
            g_presenterOriginals[index](self, flyout);                        \
        }                                                                     \
    }
SP_PRESENTER_HOOK(0)
SP_PRESENTER_HOOK(1)
SP_PRESENTER_HOOK(2)
SP_PRESENTER_HOOK(3)
SP_PRESENTER_HOOK(4)
SP_PRESENTER_HOOK(5)
#undef SP_PRESENTER_HOOK

void InstallPresenterHooks(HMODULE hFileExplorer)
{
    static const wchar_t* const kNames[kPresenterHookCount][1] =
    {
        { L"private: void __cdecl ContextMenuPresenter::RegisterTappedOnShowMoreOptions(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter::HandleDuplicateAccessKeys(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter::SetAccessKeyScope(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter_Old::RegisterTappedOnShowMoreOptions(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter_Old::HandleDuplicateAccessKeys(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter_Old::SetAccessKeyScope(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
    };

    static void* const kHookFunctions[kPresenterHookCount] =
    {
        (void*)PresenterHook0, (void*)PresenterHook1, (void*)PresenterHook2,
        (void*)PresenterHook3, (void*)PresenterHook4, (void*)PresenterHook5,
    };

    SP_SymbolHook hooks[kPresenterHookCount] = {};
    for (int i = 0; i < kPresenterHookCount; ++i)
    {
        hooks[i].symbols      = kNames[i];
        hooks[i].symbolCount  = 1;
        hooks[i].pOriginal    = (void**)&g_presenterOriginals[i];
        hooks[i].hookFunction = kHookFunctions[i];
        hooks[i].optional     = TRUE;
    }

    if (!SP_HookSymbols(hFileExplorer, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The WinUI menu builder could not be hooked; only the classic menu is filtered");
        return;
    }

    int found = 0;
    for (int i = 0; i < kPresenterHookCount; ++i)
    {
        if (g_presenterOriginals[i])
        {
            ++found;
        }
    }
    if (found == 0)
    {
        SP_LogError(L"No ContextMenuPresenter member was found in this build; only the classic menu is filtered");
    }
    else
    {
        SP_Log(L"WinUI menu: %d of %d presenter members hooked", found, kPresenterHookCount);
    }
}

void OnFileExplorerLoaded(HMODULE hModule, void*)
{
    InstallPresenterHooks(hModule);
}

// ---------------------------------------------------------------------------------------------------------------
// The classic menu: which windows count
// ---------------------------------------------------------------------------------------------------------------

enum class OwnerKind
{
    Other,          // taskbar, tray, Start, a toolbar: not a file menu
    Desktop,
    FileView,       // the file list of a folder window
    NavigationPane,
};

// The desktop's root is the shell window itself, or a top-level Progman/WorkerW that hosts the desktop's
// SHELLDLL_DefView (a wallpaper tool can re-parent the view onto a WorkerW). Other WorkerW/Progman windows the
// shell reuses elsewhere have no such child.
bool IsDesktopOwner(HWND hwnd)
{
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root)
    {
        return false;
    }
    if (root == GetShellWindow())
    {
        return true;
    }
    wchar_t cls[64] = {};
    GetClassNameW(root, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0)
    {
        return false;
    }
    return FindWindowExW(root, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
}

// The desktop check comes first: its icon view is itself a SHELLDLL_DefView, so the walk below would call it
// a folder view.
OwnerKind ClassifyOwner(HWND hwnd)
{
    if (!hwnd)
    {
        return OwnerKind::Other;
    }
    if (IsDesktopOwner(hwnd))
    {
        return OwnerKind::Desktop;
    }
    for (HWND h = hwnd; h; h = GetAncestor(h, GA_PARENT))
    {
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        if (wcscmp(cls, L"SHELLDLL_DefView") == 0)
        {
            return OwnerKind::FileView;
        }
        if (wcscmp(cls, L"NamespaceTreeControl") == 0)
        {
            return OwnerKind::NavigationPane;
        }
    }
    return OwnerKind::Other;
}

// ---------------------------------------------------------------------------------------------------------------
// The classic menu: one tracking session per thread
// ---------------------------------------------------------------------------------------------------------------

// TrackPopupMenu is implemented on top of TrackPopupMenuEx, so one menu can pass through both hooks; the depth
// counter makes the outermost call own the session. Plain data only: a thread_local with a destructor is
// avoided in a DLL that is loaded and unloaded by hand.
struct Session
{
    int   depth;
    HHOOK messageHook;      // the WH_CALLWNDPROCRET hook that sees WM_INITMENUPOPUP for sub-menus
    bool  recurse;          // TRUE when sub-menus must be walked here because that hook will not see them
    int   detachedCount;
    HMENU detached[64];     // sub-menus taken out with RemoveMenu, destroyed when the session ends
};

thread_local Session t_session = {};

// Every message hook still installed, so BeforeUninit can take them down while the thread that owns each is
// sitting in a menu's modal loop: a hook procedure that outlives its DLL is a crash.
std::mutex         g_messageHooksLock;
std::vector<HHOOK> g_messageHooks;
bool               g_unloading = false;

void StashDetached(HMENU hSubMenu)
{
    Session& s = t_session;
    if (s.depth == 0)
    {
        // Not inside a tracking call: nothing else can refer to it, so it can go right away.
        DestroyMenu(hSubMenu);
        return;
    }
    if (s.detachedCount < (int)ARRAYSIZE(s.detached))
    {
        s.detached[s.detachedCount++] = hSubMenu;
    }
    else
    {
        SP_LogDebug(L"Too many detached sub-menus in one session; one is leaked");
    }
}

// Removes every matching row from one HMENU, and with `recurse` from its sub-menus too. Returns how many rows
// this menu lost, sub-menus included.
int ProcessMenu(HMENU hMenu, const Rules& rules, bool recurse, int depth = 0)
{
    if (!hMenu || depth > 8)
    {
        return 0;
    }

    int count = GetMenuItemCount(hMenu);
    if (count <= 0)
    {
        return 0;
    }

    int removedHere = 0;
    int removedBelow = 0;

    // Walked backwards so that removing a row does not shift the ones still to be checked.
    for (int i = count - 1; i >= 0; --i)
    {
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask  = MIIM_STRING | MIIM_SUBMENU | MIIM_FTYPE;
        if (!GetMenuItemInfoW(hMenu, i, TRUE, &info))
        {
            continue;
        }

        HMENU hSub = info.hSubMenu;
        bool hide = false;

        // A separator or a bitmap has no text to compare. Owner-drawn rows are kept in: on Windows 11 the shell
        // turns every row of its classic menu owner-drawn (that is how the dark, icon-bearing menu is painted)
        // but the string it was inserted with stays behind MIIM_STRING, so it can still be read and matched.
        if (info.cch > 0 && !(info.fType & (MFT_SEPARATOR | MFT_BITMAP)))
        {
            wchar_t text[512] = {};
            MENUITEMINFOW textInfo = {};
            textInfo.cbSize     = sizeof(textInfo);
            textInfo.fMask      = MIIM_STRING;
            textInfo.dwTypeData = text;
            textInfo.cch        = ARRAYSIZE(text);
            if (GetMenuItemInfoW(hMenu, i, TRUE, &textInfo))
            {
                hide = ShouldHide(Normalize(text, true), rules);
                if (hide)
                {
                    SP_LogDebug(L"Hiding classic row \"%s\"", text);
                }
            }
        }

        if (hide)
        {
            // RemoveMenu detaches a sub-menu instead of destroying it: the handler that built it may still hold
            // the handle until the menu closes, so it is destroyed when the session ends, not now.
            if (RemoveMenu(hMenu, i, MF_BYPOSITION))
            {
                ++removedHere;
                if (hSub)
                {
                    StashDetached(hSub);
                }
            }
            continue;
        }

        if (hSub && recurse)
        {
            removedBelow += ProcessMenu(hSub, rules, true, depth + 1);
        }
    }

    if (removedHere == 0)
    {
        return removedBelow;
    }

    // Separators left touching each other, or at either end.
    count = GetMenuItemCount(hMenu);
    bool previousWasSeparator = false;
    for (int i = count - 1; i >= 0; --i)
    {
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask  = MIIM_FTYPE;
        if (!GetMenuItemInfoW(hMenu, i, TRUE, &info))
        {
            continue;
        }
        bool separator = (info.fType & MFT_SEPARATOR) != 0;
        if (separator && previousWasSeparator)
        {
            DeleteMenu(hMenu, i, MF_BYPOSITION);
            continue;
        }
        previousWasSeparator = separator;
    }

    for (int pass = 0; pass < 2; ++pass)
    {
        count = GetMenuItemCount(hMenu);
        if (count <= 0)
        {
            break;
        }
        int at = (pass == 0) ? 0 : count - 1;
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask  = MIIM_FTYPE;
        if (GetMenuItemInfoW(hMenu, at, TRUE, &info) && (info.fType & MFT_SEPARATOR))
        {
            DeleteMenu(hMenu, at, MF_BYPOSITION);
        }
    }

    return removedHere + removedBelow;
}

// WM_INITMENUPOPUP is sent, not posted, to the owner window from inside the menu's modal loop, which is why a
// thread-local WH_CALLWNDPROCRET hook is the way to see it. On return the shell has filled the sub-menu.
LRESULT CALLBACK MenuMessageHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && lParam)
    {
        const CWPRETSTRUCT* msg = (const CWPRETSTRUCT*)lParam;
        if (msg->message == WM_INITMENUPOPUP && msg->wParam)
        {
            try
            {
                const Rules rules = SnapshotRules();
                ProcessMenu((HMENU)msg->wParam, rules, false);
            }
            catch (...)
            {
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void BeginSession(UINT trackFlags)
{
    Session& s = t_session;
    if (s.depth == 0)
    {
        s.messageHook   = nullptr;
        s.detachedCount = 0;
        s.recurse       = true;

        HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROCRET, MenuMessageHook, nullptr, GetCurrentThreadId());
        if (hook)
        {
            bool keep = false;
            try
            {
                std::lock_guard<std::mutex> lock(g_messageHooksLock);
                if (!g_unloading)
                {
                    g_messageHooks.push_back(hook);
                    keep = true;
                }
            }
            catch (...)
            {
            }

            if (keep)
            {
                s.messageHook = hook;
                s.recurse     = false;
            }
            else
            {
                // Either the mod is on its way out or the list could not grow; the hook must not outlive the
                // sweep in BeforeUninit, so it is taken down here.
                UnhookWindowsHookEx(hook);
            }
        }
        else
        {
            SP_LogDebug(L"SetWindowsHookEx failed (%lu); sub-menus are walked up front instead", GetLastError());
        }

        // TPM_NONOTIFY suppresses WM_INITMENUPOPUP, so the hook would never see the sub-menus.
        if (trackFlags & TPM_NONOTIFY)
        {
            s.recurse = true;
        }
    }
    ++s.depth;
}

void EndSession()
{
    Session& s = t_session;
    if (--s.depth > 0)
    {
        return;
    }
    s.depth = 0;

    if (s.messageHook)
    {
        HHOOK hook = s.messageHook;
        s.messageHook = nullptr;
        try
        {
            // Under the lock, so the handle is either still listed and hooked or removed and unhooked, never
            // in between with BeforeUninit's sweep.
            std::lock_guard<std::mutex> lock(g_messageHooksLock);
            for (size_t i = 0; i < g_messageHooks.size(); ++i)
            {
                if (g_messageHooks[i] == hook)
                {
                    g_messageHooks.erase(g_messageHooks.begin() + i);
                    UnhookWindowsHookEx(hook);
                    break;
                }
            }
        }
        catch (...)
        {
        }
    }

    // The tracking call has returned; the shell's own tree no longer reaches these handles.
    for (int i = 0; i < s.detachedCount; ++i)
    {
        DestroyMenu(s.detached[i]);
    }
    s.detachedCount = 0;
}

// Shared by the two hooks. The top-level menu is always walked here: the message hook re-checks it too when
// WM_INITMENUPOPUP arrives, but TPM_NONOTIFY can keep that message away, and a second pass is cheap.
void FilterClassicMenu(HMENU hMenu, UINT trackFlags, HWND hOwner, OwnerKind kind, const wchar_t* via)
{
    BeginSession(trackFlags);
    try
    {
        const Rules rules = SnapshotRules();
        int removed = ProcessMenu(hMenu, rules, t_session.recurse);
        if (removed)
        {
            const wchar_t* where = L"navigation pane";
            if (kind == OwnerKind::Desktop)
            {
                where = L"desktop";
            }
            else if (kind == OwnerKind::FileView)
            {
                where = L"file view";
            }
            SP_Log(L"%s: %d row(s) hidden from a classic menu on %p (%s)", via, removed, hOwner, where);
        }
        else
        {
            SP_LogDebug(L"%s: nothing to hide in the classic menu on %p", via, hOwner);
        }
    }
    catch (...)
    {
        SP_LogError(L"The classic menu could not be filtered");
    }
}

using TrackPopupMenuEx_t = decltype(&TrackPopupMenuEx);
using TrackPopupMenu_t   = decltype(&TrackPopupMenu);
TrackPopupMenuEx_t g_origTrackPopupMenuEx = nullptr;
TrackPopupMenu_t   g_origTrackPopupMenu   = nullptr;

BOOL WINAPI TrackPopupMenuEx_Hook(HMENU hMenu, UINT uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm)
{
    OwnerKind kind = OwnerKind::Other;
    if (hMenu && g_anything.load(std::memory_order_relaxed))
    {
        kind = ClassifyOwner(hWnd);
    }
    const bool ours = kind != OwnerKind::Other;
    if (ours)
    {
        FilterClassicMenu(hMenu, uFlags, hWnd, kind, L"TrackPopupMenuEx");
    }

    BOOL result = g_origTrackPopupMenuEx(hMenu, uFlags, x, y, hWnd, lptpm);

    if (ours)
    {
        EndSession();
    }
    return result;
}

BOOL WINAPI TrackPopupMenu_Hook(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd, const RECT* prcRect)
{
    OwnerKind kind = OwnerKind::Other;
    if (hMenu && g_anything.load(std::memory_order_relaxed))
    {
        kind = ClassifyOwner(hWnd);
    }
    const bool ours = kind != OwnerKind::Other;
    if (ours)
    {
        FilterClassicMenu(hMenu, uFlags, hWnd, kind, L"TrackPopupMenu");
    }

    BOOL result = g_origTrackPopupMenu(hMenu, uFlags, x, y, nReserved, hWnd, prcRect);

    if (ours)
    {
        EndSession();
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    BuildLabelIndex();
    LoadSettings();

    // A previous unload left this set; a mod that is turned off and on again must install its message hooks.
    {
        std::lock_guard<std::mutex> lock(g_messageHooksLock);
        g_unloading = false;
    }

    // The classic hooks are the part that works on every build, so they are required.
    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetFunctionHook(TrackPopupMenuEx, TrackPopupMenuEx_Hook, &g_origTrackPopupMenuEx) ||
        !SP_SetFunctionHook(TrackPopupMenu, TrackPopupMenu_Hook, &g_origTrackPopupMenu))
    {
        SP_HookAbort();
        SP_LogError(L"TrackPopupMenu(Ex) could not be hooked");
        return FALSE;
    }
    if (!SP_HookCommit())
    {
        SP_LogError(L"The classic menu hooks could not be applied");
        return FALSE;
    }

    // Windows.UI.FileExplorer.dll arrives the first time a shell view needs it; on a cold sign-in that is after
    // the engine has started. The wait is cancelled by the engine if the mod unloads first. The callback resolves
    // symbols, which may read the cache or download a PDB, so it runs on a worker rather than on the thread that
    // is loading the library.
    if (!SP_WaitForModuleOnWorker(L"Windows.UI.FileExplorer.dll", 0, OnFileExplorerLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Windows.UI.FileExplorer.dll; only the classic menu is filtered");
    }

    SP_Log(L"Classic menu hooks in place (TrackPopupMenuEx, TrackPopupMenu); %u label(s) known; "
           L"waiting for the WinUI menu builder", (unsigned)g_labelIndex.size());
    return TRUE;
}

void BeforeUninit()
{
    // The function hooks go with the engine's help; the message hooks are ours to take down. After this no new
    // one is installed (see BeginSession) and a session still open finds its handle gone and skips the unhook.
    try
    {
        std::lock_guard<std::mutex> lock(g_messageHooksLock);
        g_unloading = true;
        for (HHOOK hook : g_messageHooks)
        {
            UnhookWindowsHookEx(hook);
        }
        g_messageHooks.clear();
    }
    catch (...)
    {
    }
}

}   // namespace

SP_MOD_DEFINE(g_modRemoveContextMenuItems) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Remove items from the context menu",
    /* basedOn        */ "remove-context-menu-items",
    /* originalAuthor */ "Armaninyow",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,     // the classic hooks work everywhere; the WinUI hooks are optional
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
