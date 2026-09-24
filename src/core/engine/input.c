#include "input.h"
#include "log.h"
#include "settings.h"

#include <string.h>

#define TAG "input"

#define SP_MAX_SUBSCRIPTIONS 64
#define SP_DEFAULT_PRIORITY  100

typedef struct Subscription
{
    const char*     modId;
    SP_InputSurface surface;
    SP_InputGesture gesture;
    SP_InputHandler handler;
    void*           context;
    int             priority;
    BOOL            active;
} Subscription;

static Subscription     g_subscriptions[SP_MAX_SUBSCRIPTIONS];
static int              g_count = 0;
static CRITICAL_SECTION g_lock;
static BOOL             g_initialized = FALSE;

// Implemented by the surface watchers (surface_desktop.c and friends). Starting a surface installs whatever
// subclass or hook that surface needs; stopping it takes them away again.
extern BOOL SP_SurfaceStart(SP_InputSurface surface);
extern void SP_SurfaceStop(SP_InputSurface surface);

static int CountOnSurface(SP_InputSurface surface)
{
    int n = 0;
    for (int i = 0; i < g_count; ++i)
    {
        if (g_subscriptions[i].active && g_subscriptions[i].surface == surface)
        {
            n++;
        }
    }
    return n;
}

// Insertion sort by priority; the table is small and only changes when a mod loads or settings change.
static void SortLocked(void)
{
    for (int i = 1; i < g_count; ++i)
    {
        Subscription key = g_subscriptions[i];
        int j = i - 1;
        while (j >= 0 && g_subscriptions[j].priority > key.priority)
        {
            g_subscriptions[j + 1] = g_subscriptions[j];
            j--;
        }
        g_subscriptions[j + 1] = key;
    }
}

void SP_InputInitialize(void)
{
    if (g_initialized)
    {
        return;
    }
    InitializeCriticalSection(&g_lock);
    g_count = 0;
    g_initialized = TRUE;
}

void SP_InputShutdown(void)
{
    if (!g_initialized)
    {
        return;
    }

    for (int s = 0; s < SP_SURFACE_COUNT; ++s)
    {
        if (CountOnSurface((SP_InputSurface)s) > 0)
        {
            SP_SurfaceStop((SP_InputSurface)s);
        }
    }

    EnterCriticalSection(&g_lock);
    g_count = 0;
    LeaveCriticalSection(&g_lock);

    DeleteCriticalSection(&g_lock);
    g_initialized = FALSE;
}

BOOL SP_InputSubscribe(const char* modId, SP_InputSurface surface, SP_InputGesture gesture,
                       SP_InputHandler handler, void* context)
{
    if (!g_initialized || !modId || !handler || surface >= SP_SURFACE_COUNT || gesture >= SP_GESTURE_COUNT)
    {
        return FALSE;
    }

    EnterCriticalSection(&g_lock);

    if (g_count >= SP_MAX_SUBSCRIPTIONS)
    {
        LeaveCriticalSection(&g_lock);
        SP_LOG_ERR(TAG, L"%S: no room for another input subscription", modId);
        return FALSE;
    }

    BOOL firstOnSurface = (CountOnSurface(surface) == 0);

    g_subscriptions[g_count].modId = modId;
    g_subscriptions[g_count].surface = surface;
    g_subscriptions[g_count].gesture = gesture;
    g_subscriptions[g_count].handler = handler;
    g_subscriptions[g_count].context = context;
    g_subscriptions[g_count].priority = SP_SettingsGetInt(modId, L"InputPriority", SP_DEFAULT_PRIORITY);
    g_subscriptions[g_count].active = TRUE;
    g_count++;

    SortLocked();
    LeaveCriticalSection(&g_lock);

    SP_LOG_DBG(TAG, L"%S subscribed to surface %d gesture %d", modId, (int)surface, (int)gesture);

    if (firstOnSurface && !SP_SurfaceStart(surface))
    {
        SP_LOG_ERR(TAG, L"Surface %d could not be started; %S will not receive events", (int)surface, modId);
        return FALSE;
    }
    return TRUE;
}

void SP_InputUnsubscribe(const char* modId)
{
    if (!g_initialized || !modId)
    {
        return;
    }

    BOOL surfaceEmptied[SP_SURFACE_COUNT] = { 0 };

    EnterCriticalSection(&g_lock);

    int out = 0;
    for (int i = 0; i < g_count; ++i)
    {
        if (g_subscriptions[i].active && strcmp(g_subscriptions[i].modId, modId) == 0)
        {
            continue;   // dropped
        }
        g_subscriptions[out++] = g_subscriptions[i];
    }
    g_count = out;

    for (int s = 0; s < SP_SURFACE_COUNT; ++s)
    {
        surfaceEmptied[s] = (CountOnSurface((SP_InputSurface)s) == 0);
    }

    LeaveCriticalSection(&g_lock);

    // Stopping a surface can take a lock of its own, so it happens outside ours.
    for (int s = 0; s < SP_SURFACE_COUNT; ++s)
    {
        if (surfaceEmptied[s])
        {
            SP_SurfaceStop((SP_InputSurface)s);
        }
    }
}

void SP_InputReloadPriorities(void)
{
    if (!g_initialized)
    {
        return;
    }

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_count; ++i)
    {
        g_subscriptions[i].priority = SP_SettingsGetInt(g_subscriptions[i].modId, L"InputPriority", SP_DEFAULT_PRIORITY);
    }
    SortLocked();
    LeaveCriticalSection(&g_lock);
}

BOOL SP_InputDispatch(const SP_InputEvent* event)
{
    if (!g_initialized || !event)
    {
        return FALSE;
    }

    // The handlers are copied out under the lock and called without it: a handler may show UI, take its own
    // locks, or unsubscribe, and none of that may run while the table is held.
    SP_InputHandler handlers[SP_MAX_SUBSCRIPTIONS];
    void*           contexts[SP_MAX_SUBSCRIPTIONS];
    const char*     owners[SP_MAX_SUBSCRIPTIONS];
    int             n = 0;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_count; ++i)
    {
        if (g_subscriptions[i].active &&
            g_subscriptions[i].surface == event->surface &&
            g_subscriptions[i].gesture == event->gesture)
        {
            handlers[n] = g_subscriptions[i].handler;
            contexts[n] = g_subscriptions[i].context;
            owners[n] = g_subscriptions[i].modId;
            n++;
        }
    }
    LeaveCriticalSection(&g_lock);

    for (int i = 0; i < n; ++i)
    {
        if (handlers[i](event, contexts[i]))
        {
            SP_LOG_DBG(TAG, L"%S consumed surface %d gesture %d", owners[i], (int)event->surface, (int)event->gesture);
            return TRUE;
        }
    }

    return FALSE;
}
