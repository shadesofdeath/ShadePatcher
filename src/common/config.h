#pragma once
//
// config.h - project-wide identifiers.
//
// Every string that names the product, its registry location or its files lives here, so renaming the
// project is a matter of editing this one file. Included by every module (C and C++).
//

#define PRODUCT_NAME            "ShadePatcher"
#define PRODUCT_PUBLISHER       "shadesofdeath"
#define PRODUCT_URL             "https://github.com/shadesofdeath/ShadePatcher"

// Where the settings live. Every option in settings.reg writes below HKEY_CURRENT_USER\<REGPATH> unless it
// deliberately targets a Windows key.
#define REGPATH                 "Software\\ShadePatcher"

// Unique id of this program. Used to build window class names, event names and the "Virtualized_<CLSID>_" prefix
// of settings that are not stored in the registry (see gui/registry.c).
#define APP_CLSID_LITE          "7A2C3F1E-4B8D-4E62-9C5A-1F0D6B8E2A47"
#define APP_CLSID               "{" APP_CLSID_LITE "}"

// Install location: %ProgramFiles%\<PRODUCT_NAME>
#define SPECIAL_FOLDER          CSIDL_PROGRAM_FILES
#define APP_RELATIVE_PATH       "\\" PRODUCT_NAME

// File names of the binaries this solution produces.
#define CORE_DLL_NAME           PRODUCT_NAME ".dll"
#define GUI_DLL_NAME            "sp_gui.dll"
#define SETUP_UTILITY_NAME      "sp_setup.exe"

// Window class of the settings window; also used to find an already open instance.
#define GUI_WINDOW_CLASS        PRODUCT_NAME "_Settings_" APP_CLSID_LITE
