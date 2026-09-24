#pragma once
//
// conditions.h - the conditions a ";s <Section> <Condition>" line of settings.reg may use.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns TRUE when the lines guarded by the condition should be shown. A leading '!' negates the condition.
// Unknown conditions are shown (and reported on the debug console) so a typo never hides a whole section.
BOOL GUI_EvaluateCondition(const char* name);

#ifdef __cplusplus
}
#endif
