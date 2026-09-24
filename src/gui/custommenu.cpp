#include "custommenu.h"

#include <strsafe.h>
#include <tchar.h>

#include <string>

#include "config.h"
#include "menuitems.h"
#include "resources/resource.h"

extern "C" HMODULE hModule;

namespace {

std::wstring LoadText(UINT id)
{
    wchar_t buffer[512];
    int length = LoadStringW(hModule, id, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, length > 0 ? length : 0);
}
struct Preset
{
    UINT           nameId;      // shown in the list, and used as the entry's name
    const wchar_t* command;     // what the shell is asked to open
    const wchar_t* glyph;       // a Segoe Fluent Icons character, drawn beside the entry in the menu
};

// "shell:" names and control panel commands are used rather than paths, because those keep working when Windows
// moves a program and they do not depend on the system language.
const Preset kPresets[] =
{
    { IDS_PRESET_TASKMANAGER,     L"taskmgr.exe", L"\xE9D9" },
    { IDS_PRESET_SETTINGS,        L"ms-settings:", L"\xE713" },
    { IDS_PRESET_CONTROLPANEL,    L"control.exe", L"\xE115" },
    { IDS_PRESET_THISPC,          L"shell:MyComputerFolder", L"\xE977" },
    { IDS_PRESET_RECYCLEBIN,      L"shell:RecycleBinFolder", L"\xE74D" },
    { IDS_PRESET_DOWNLOADS,       L"shell:Downloads", L"\xE896" },
    { IDS_PRESET_STARTUP,         L"shell:startup", L"\xE8B7" },
    { IDS_PRESET_TERMINAL,        L"wt.exe", L"\xE756" },
    { IDS_PRESET_CMD,             L"cmd.exe", L"\xE756" },
    { IDS_PRESET_POWERSHELL,      L"powershell.exe", L"\xE756" },
    { IDS_PRESET_REGEDIT,         L"regedit.exe", L"\xE943" },
    { IDS_PRESET_DEVICEMANAGER,   L"devmgmt.msc", L"\xE772" },
    { IDS_PRESET_DISKMANAGER,     L"diskmgmt.msc", L"\xEDA2" },
    { IDS_PRESET_SERVICES,        L"services.msc", L"\xE90F" },
    { IDS_PRESET_APPS,            L"ms-settings:appsfeatures", L"\xE71D" },
    { IDS_PRESET_DISPLAY,         L"ms-settings:display", L"\xE7F4" },
    { IDS_PRESET_SOUND,           L"ms-settings:sound", L"\xE767" },
    { IDS_PRESET_BLUETOOTH,       L"ms-settings:bluetooth", L"\xE702" },
};

// The id of the "Custom..." item in the drop-down, which adds a blank entry rather than a ready-made one.
constexpr int kCustomIndex = ARRAYSIZE(kPresets);

// Whether the list holds a given ready-made entry. Matched on the command, which is what makes an entry do what
// it does; the user may well have renamed it.
int FindPreset(const SP_MenuItem* items, int count, const Preset& preset)
{
    for (int i = 0; i < count; ++i)
    {
        if (_wcsicmp(items[i].command, preset.command) == 0)
        {
            return i;
        }
    }
    return -1;
}

}   // namespace
// ---------------------------------------------------------------------------------------------------------------
// Drawing the list inside the settings window
//
// The page engine reads its definition as text and draws one line at a time. The user's entries are not fixed
// text, so the ";k" line in the page is expanded here into real lines, one per entry, before the engine ever
// sees it. Nothing in the drawing loop has to know about lists, and the rows come out in the window's own font,
// colours and dark mode because the same code draws them as everything else.
// ---------------------------------------------------------------------------------------------------------------

namespace {

// Held between calls because the engine keeps reading from it while it draws.
std::string g_expandedPage;

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    int cb = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)cb, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), cb, nullptr, nullptr);
    return out;
}

}   // namespace

// One menu's worth of controls. Both menus are drawn the same way and differ only in which list they read and
// which action names their links carry, so the work is written once and called twice.
namespace {

// Which marker line belongs to which menu.
struct Section { const char* marker; const wchar_t* modId; const char* action; };
const Section kSections[] =
{
    { "\r\n;k\r\n", L"taskbar-menu-entry", "cm_" },
    { "\r\n;K\r\n", L"desktop-menu-entry", "cd_" },
    // The same markers with bare line feeds, for a settings.reg saved without carriage returns.
    { "\n;k\n",     L"taskbar-menu-entry", "cm_" },
    { "\n;K\n",     L"desktop-menu-entry", "cd_" },
};

std::string BuildSection(const wchar_t* modId, const char* actionPrefix)
{
    SP_MenuItem items[SP_MENU_MAX_ITEMS];
    int count = SP_MenuItemsLoadFor(modId, items, SP_MENU_MAX_ITEMS);

    const std::string mods = "[HKEY_CURRENT_USER\\" REGPATH "\\Mods\\" + ToUtf8(modId);
    const std::string modKey = mods + "]";
    const std::string itemKey = mods + "\\Items\\";

    std::string out;
    out.reserve(4096);

    // The ready-made entries, as a drop-down where each one is a check mark: picking one that is off adds it,
    // picking one that is on takes it out again. "Custom..." at the end adds a blank entry to fill in.
    out += "\r\n" + modKey;
    out += "\r\n;m " + ToUtf8(LoadText(IDS_PRESET_LABEL));
    for (int i = 0; i < (int)ARRAYSIZE(kPresets); ++i)
    {
        const bool present = FindPreset(items, count, kPresets[i]) >= 0;
        out += present ? "\r\n;X " : "\r\n;x ";
        out += std::to_string(i) + " " + ToUtf8(LoadText(kPresets[i].nameId));
    }
    out += "\r\n;x " + std::to_string(kCustomIndex) + " " + ToUtf8(LoadText(IDS_PRESET_CUSTOM));
    out += "\r\n;" + std::string(actionPrefix) + "toggle_";

    if (count >= SP_MENU_MAX_ITEMS)
    {
        out += "\r\n;e " + ToUtf8(LoadText(IDS_CM_FULL));
    }

    if (count == 0)
    {
        out += "\r\n;e ";
        out += "\r\n;e " + ToUtf8(LoadText(IDS_CM_EMPTY));
    }

    // Each field is an ordinary text box bound to the value the mod reads, so editing one is the same code path
    // as every other setting in the window and takes effect without a save step.
    for (int i = 0; i < count; ++i)
    {
        const std::string key = itemKey + std::to_string(i) + "]";
        const std::wstring name = items[i].name[0] ? items[i].name : LoadText(IDS_PRESET_CUSTOM);

        out += "\r\n;e ";
        out += "\r\n;a " + ToUtf8(name);

        // The prompt is what the input box says above the text field. For the icon it carries the two spellings
        // the field accepts and an example, since a bare "Icon" would not tell anyone that a glyph code works.
        struct Field { UINT labelId; UINT promptId; const char* value; };
        const Field fields[] =
        {
            { IDS_CM_NAME,    IDS_CM_NAME,        "\"Name\"=\"\"" },
            { IDS_CM_COMMAND, IDS_CM_COMMAND,     "\"Command\"=\"\"" },
            { IDS_CM_ARGS,    IDS_CM_ARGS,        "\"Arguments\"=\"\"" },
            { IDS_CM_ICON,    IDS_CM_ICON_PROMPT, "\"Icon\"=\"\"" },
        };

        for (const Field& field : fields)
        {
            const std::string label = ToUtf8(LoadText(field.labelId));
            out += "\r\n" + key;
            out += "\r\n;w " + label;
            out += "\r\n;" + ToUtf8(LoadText(field.promptId));
            out += "\r\n;";
            out += "\r\n";
            out += field.value;
        }

        out += "\r\n;u " + ToUtf8(LoadText(IDS_CM_REMOVE));
        out += "\r\n;" + std::string(actionPrefix) + "del_" + std::to_string(i);
    }

    // How to fill in the icon field, once per page, after the entries so it does not push them down.
    out += "\r\n;e ";
    out += "\r\n;a " + ToUtf8(LoadText(IDS_CM_ICON_HEADING));
    out += "\r\n;e " + ToUtf8(LoadText(IDS_CM_ICON_HELP_1));
    out += "\r\n;e " + ToUtf8(LoadText(IDS_CM_ICON_HELP_2));
    out += "\r\n;e " + ToUtf8(LoadText(IDS_CM_ICON_HELP_3));
    out += "\r\n;y " + ToUtf8(LoadText(IDS_CM_ICON_LINK)) + " \xF0\x9F\xA1\x95";
    out += "\r\n;https://learn.microsoft.com/windows/apps/design/style/segoe-fluent-icons-font";
    out += "\r\n;y " + ToUtf8(LoadText(IDS_CM_ICON_CHARMAP)) + " \xF0\x9F\xA1\x95";
    out += "\r\n;charmap.exe";

    out += "\r\n";
    return out;
}

}   // namespace

extern "C" PVOID SP_ExpandCustomMenuLines(const void* page, DWORD pageSize, DWORD* expandedSize)
{
    if (!page || pageSize == 0 || !expandedSize)
    {
        return nullptr;
    }

    std::string source((const char*)page, pageSize);
    bool touched = false;

    for (const Section& section : kSections)
    {
        size_t at = source.find(section.marker);
        if (at == std::string::npos)
        {
            continue;
        }
        source = source.substr(0, at) + BuildSection(section.modId, section.action) +
                 source.substr(at + strlen(section.marker));
        touched = true;
    }

    if (!touched)
    {
        return nullptr;     // pages without a list cost nothing
    }

    g_expandedPage = std::move(source);
    *expandedSize = (DWORD)g_expandedPage.size();
    return (PVOID)g_expandedPage.data();
}

extern "C" BOOL SP_HandleCustomMenuAction(HWND hParent, const char* action)
{
    UNREFERENCED_PARAMETER(hParent);

    if (!action)
    {
        return FALSE;
    }

    // The action name says which menu it belongs to, so one handler serves both pages.
    const Section* section = nullptr;
    for (const Section& candidate : kSections)
    {
        if (strncmp(action, candidate.action, strlen(candidate.action)) == 0)
        {
            section = &candidate;
            break;
        }
    }
    if (!section)
    {
        return FALSE;
    }

    const char* verb = action + strlen(section->action);

    SP_MenuItem items[SP_MENU_MAX_ITEMS];
    int count = SP_MenuItemsLoadFor(section->modId, items, SP_MENU_MAX_ITEMS);

    if (strncmp(verb, "toggle_", 7) == 0)
    {
        int id = atoi(verb + 7);

        if (id == kCustomIndex)
        {
            // A blank entry, filled in with the boxes that appear for it. It stays out of the shell's menu
            // until it has a command.
            if (count >= SP_MENU_MAX_ITEMS)
            {
                return FALSE;
            }
            SP_MenuItem item;
            ZeroMemory(&item, sizeof(item));
            StringCchCopyW(item.name, ARRAYSIZE(item.name), LoadText(IDS_PRESET_NEW_NAME).c_str());
            items[count++] = item;
            return SP_MenuItemsSaveFor(section->modId, items, count);
        }

        if (id < 0 || id >= (int)ARRAYSIZE(kPresets))
        {
            return FALSE;
        }
        const Preset& preset = kPresets[id];

        // On already: every copy of it comes out, so the check mark and the list agree.
        int at = FindPreset(items, count, preset);
        if (at >= 0)
        {
            int out = 0;
            for (int i = 0; i < count; ++i)
            {
                if (_wcsicmp(items[i].command, preset.command) != 0)
                {
                    items[out++] = items[i];
                }
            }
            return SP_MenuItemsSaveFor(section->modId, items, out);
        }

        // Off: goes in at the end.
        if (count >= SP_MENU_MAX_ITEMS)
        {
            return FALSE;
        }
        SP_MenuItem item;
        ZeroMemory(&item, sizeof(item));
        StringCchCopyW(item.name, ARRAYSIZE(item.name), LoadText(preset.nameId).c_str());
        StringCchCopyW(item.command, ARRAYSIZE(item.command), preset.command);
        StringCchCopyW(item.icon, ARRAYSIZE(item.icon), preset.glyph);
        items[count++] = item;
        return SP_MenuItemsSaveFor(section->modId, items, count);
    }

    if (strncmp(verb, "del_", 4) == 0)
    {
        int index = atoi(verb + 4);
        if (index < 0 || index >= count)
        {
            return FALSE;
        }
        for (int i = index; i + 1 < count; ++i)
        {
            items[i] = items[i + 1];
        }
        SP_MenuItemsSaveFor(section->modId, items, count - 1);
        return TRUE;
    }

    return FALSE;
}
