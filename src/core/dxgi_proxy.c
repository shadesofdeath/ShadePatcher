//
// dxgi_proxy.c - the way into explorer.exe.
//
// Windows offers no supported hook for "run my code inside the shell". ExplorerPatcher solved it by having
// explorer load the DLL itself, and the same approach is used here because it needs no remote thread, no
// injection and no driver:
//
//   The installer copies this DLL to C:\Windows\dxgi.dll. explorer.exe lives in C:\Windows, and the loader
//   searches an executable's own directory before System32, so explorer loads this file instead of the real
//   dxgi.dll. DllMain then starts the engine, and every export below hands the call on to the genuine
//   System32\dxgi.dll so that graphics keep working exactly as before.
//
// Every export the real dxgi.dll has is forwarded. A missing one would break any process that imports it, and
// that includes far more than explorer, so the list is kept complete rather than limited to what explorer uses.
//
// The signatures are the ones dxgi.dll publishes. Where a function is undocumented, the argument count is what
// matters: each stub only has to pass the register arguments through untouched.
//
#include <Windows.h>

// The real library, loaded once on first use. It is never freed: a process that has called through here may
// hold DXGI objects whose code lives in that module.
static HMODULE GetRealDxgi(void)
{
    static HMODULE s_hRealDxgi = NULL;
    if (!s_hRealDxgi)
    {
        wchar_t wszPath[MAX_PATH];
        UINT cch = GetSystemDirectoryW(wszPath, MAX_PATH);
        if (cch == 0 || cch >= MAX_PATH - 10)
        {
            return NULL;
        }
        wcscat_s(wszPath, MAX_PATH, L"\\dxgi.dll");

        // An absolute path into System32, so this cannot load itself or anything planted next to explorer.
        HMODULE hModule = LoadLibraryW(wszPath);

        // Benign race: two threads may load it at once, and the loader hands both the same handle.
        InterlockedCompareExchangePointer((PVOID*)&s_hRealDxgi, hModule, NULL);
    }
    return s_hRealDxgi;
}

static FARPROC GetRealExport(const char* name)
{
    HMODULE hReal = GetRealDxgi();
    return hReal ? GetProcAddress(hReal, name) : NULL;
}

// Each export resolves its counterpart once and then calls it. A missing counterpart returns E_NOTIMPL rather
// than crashing the caller, which matters on a build where an export has been removed.
#define SP_DXGI_FORWARD(returnType, name, signature, arguments, failureValue)            \
    typedef returnType(WINAPI* name##_t) signature;                                      \
    __declspec(dllexport) returnType WINAPI name signature                               \
    {                                                                                    \
        static name##_t s_pfn = NULL;                                                    \
        if (!s_pfn)                                                                      \
        {                                                                                \
            s_pfn = (name##_t)GetRealExport(#name);                                      \
            if (!s_pfn)                                                                  \
            {                                                                            \
                return (failureValue);                                                   \
            }                                                                            \
        }                                                                                \
        return s_pfn arguments;                                                          \
    }

SP_DXGI_FORWARD(HRESULT, ApplyCompatResolutionQuirking, (UINT a, UINT b), (a, b), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, CompatString, (const char* a, void* b, void* c, BOOL d), (a, b, c, d), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, CompatValue, (const char* a, UINT64* b), (a, b), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, CreateDXGIFactory, (REFIID a, void** b), (a, b), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, CreateDXGIFactory1, (REFIID a, void** b), (a, b), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, CreateDXGIFactory2, (UINT a, REFIID b, void** c), (a, b, c), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, DXGID3D10CreateDevice, (HMODULE a, void* b, void* c, void* d, void** e), (a, b, c, d, e), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, DXGID3D10CreateLayeredDevice, (void* a, void* b, void* c, void* d, void* e), (a, b, c, d, e), E_NOTIMPL)
SP_DXGI_FORWARD(SIZE_T,  DXGID3D10GetLayeredDeviceSize, (const void* a, UINT b), (a, b), 0)
SP_DXGI_FORWARD(HRESULT, DXGID3D10RegisterLayers, (const void* a, UINT b), (a, b), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, DXGIDeclareAdapterRemovalSupport, (void), (), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, DXGIDisableVBlankVirtualization, (void), (), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, DXGIDumpJournal, (void* a), (a), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, DXGIGetDebugInterface1, (UINT a, REFIID b, void** c), (a, b, c), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, DXGIReportAdapterConfiguration, (void* a), (a), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, PIXBeginCapture, (UINT a, void* b), (a, b), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, PIXEndCapture, (BOOL a), (a), E_NOTIMPL)
SP_DXGI_FORWARD(UINT,    PIXGetCaptureState, (void), (), 0)
SP_DXGI_FORWARD(HRESULT, SetAppCompatStringPointer, (SIZE_T a, const char* b), (a, b), E_NOTIMPL)
SP_DXGI_FORWARD(HRESULT, UpdateHMDEmulationStatus, (BOOL a), (a), E_NOTIMPL)
