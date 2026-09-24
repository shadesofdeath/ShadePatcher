#pragma once
//
// log.h - engine logging.
//
// The engine lives inside explorer.exe, where there is no console and a crash costs the user their shell. Logging
// therefore goes to the debugger (DbgView, WinDbg) and, when enabled, to a file under %LOCALAPPDATA%.
//
// Logging is off unless "Logging" is non-zero under HKCU\Software\ShadePatcher, so a normal run writes nothing.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SP_LogLevel
{
    SP_LOG_ERROR = 0,   // something the user would notice
    SP_LOG_INFO  = 1,   // lifecycle events: engine start, mod loaded
    SP_LOG_DEBUG = 2,   // per-hook, per-symbol detail
} SP_LogLevel;

// Reads the log level from the registry and opens the log file if needed. Safe to call more than once.
void SP_LogInitialize(void);
void SP_LogShutdown(void);

// TRUE when a message of that level would be written. Use it to skip expensive formatting.
BOOL SP_LogEnabled(SP_LogLevel level);

// Writes one line. A newline is added; the source tag identifies the component ("engine", "symbols", a mod id).
void SP_LogWrite(SP_LogLevel level, const char* tag, const wchar_t* format, ...);

// A hook transaction suspends every other thread of the process (SlimDetours) for its whole Begin..Commit window.
// A suspended thread may be holding the log's lock, so the transaction thread must not wait for it: between these
// two calls its lines are kept aside and written by SP_LogDeferEnd once the threads run again.
void SP_LogDeferBegin(void);
void SP_LogDeferEnd(void);

#define SP_LOG_ERR(tag, ...)  SP_LogWrite(SP_LOG_ERROR, (tag), __VA_ARGS__)
#define SP_LOG_INF(tag, ...)  SP_LogWrite(SP_LOG_INFO,  (tag), __VA_ARGS__)
#define SP_LOG_DBG(tag, ...)  SP_LogWrite(SP_LOG_DEBUG, (tag), __VA_ARGS__)

#ifdef __cplusplus
}
#endif
