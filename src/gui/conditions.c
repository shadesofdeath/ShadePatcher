#include "conditions.h"
#include "osversion.h"

#include <stdio.h>
#include <string.h>

typedef BOOL (*Condition_t)(void);

typedef struct Condition
{
    const char* name;
    Condition_t evaluate;
} Condition;

static BOOL IsWindows10(void)
{
    return !IsWindows11();
}

// Add new conditions here; the name is what settings.reg uses after ";s <Section> ".
static const Condition g_conditions[] =
{
    { "IsWindows10",                    IsWindows10 },
    { "IsWindows11",                    IsWindows11 },
    { "IsWindows11Version22H2OrHigher", IsWindows11Version22H2OrHigher },
    { "IsWindows11Version23H2OrHigher", IsWindows11Version23H2OrHigher },
    { NULL, NULL }
};

BOOL GUI_EvaluateCondition(const char* name)
{
    BOOL bNegate = FALSE;
    if (name[0] == '!')
    {
        bNegate = TRUE;
        name++;
    }
    for (const Condition* c = g_conditions; c->name; ++c)
    {
        if (!_stricmp(c->name, name))
        {
            BOOL bResult = c->evaluate();
            return bNegate ? !bResult : bResult;
        }
    }
    printf("settings.reg: unknown condition \"%s\", section shown.\n", name);
    return TRUE;
}
