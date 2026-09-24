#pragma once
//
// resource.h - resource ids of sp_gui.dll.
//
// Numeric layout:
//   101         RCDATA: the embedded settings.reg
//   201         product name
//   301-399     shared UI strings (dialogs, menus)
//   1001+       page strings, one block of 100 ids per page (see strings.h)
//

// Icon id 1 is deliberate: Windows shows the lowest-numbered icon resource as the file's icon in Explorer.
#define IDI_APPICON                     1
#define IDR_SETTINGS                    101

// The custom menu editor. The page engine draws every other option itself, but a list the user adds to and
// reorders needs a real control, so this one is an ordinary dialog.
#define IDD_CUSTOMMENU                  200
#define IDC_CM_LIST                     2001
#define IDC_CM_ADD                      2002
#define IDC_CM_EDIT                     2003
#define IDC_CM_REMOVE                   2004
#define IDC_CM_UP                       2005
#define IDC_CM_DOWN                     2006
#define IDC_CM_HINT                     2007

#define IDD_CUSTOMMENU_EDIT             201
#define IDC_CME_NAME                    2101
#define IDC_CME_COMMAND                 2102
#define IDC_CME_ARGS                    2103
#define IDC_CME_ICON                    2104
#define IDC_CME_BROWSE                  2105
#define IDC_CME_NAMELABEL               2106
#define IDC_CME_COMMANDLABEL            2107
#define IDC_CME_ARGSLABEL               2108
#define IDC_CME_ICONLABEL               2109
#define IDC_CME_DELETE                  2110
#define IDC_CME_PRESET                  2111
#define IDC_CME_PRESETLABEL             2112

#include "strings.h"
