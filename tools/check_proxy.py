#
# Confirms the dxgi proxy really forwards to the genuine library.
#
# This is the one thing that must not be wrong: explorer.exe loads the proxy in place of dxgi.dll, so a call that
# does not reach the real implementation stops the shell from drawing. The check loads the built DLL into this
# process and asks it for a DXGI factory, the same call explorer makes.
#
import ctypes
import ctypes.wintypes as w
import os

DLL = os.path.join("C:\\", "Users", "shades", "Desktop", "ShadePatcher",
                   "build", "bin", "Release", "ShadePatcher.dll")


class GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_uint32),
                ("Data2", ctypes.c_uint16),
                ("Data3", ctypes.c_uint16),
                ("Data4", ctypes.c_ubyte * 8)]


# IID_IDXGIFactory1
IID_IDXGIFactory1 = GUID(0x770aae78, 0xf26f, 0x4dba,
                         (ctypes.c_ubyte * 8)(0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87))

proxy = ctypes.WinDLL(DLL)
print("loaded the proxy:", DLL)

proxy.CreateDXGIFactory1.argtypes = [ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p)]
proxy.CreateDXGIFactory1.restype = ctypes.c_long

factory = ctypes.c_void_p()
hr = proxy.CreateDXGIFactory1(ctypes.byref(IID_IDXGIFactory1), ctypes.byref(factory))

print(f"CreateDXGIFactory1 returned 0x{hr & 0xFFFFFFFF:08X}, factory={factory.value}")

if hr == 0 and factory.value:
    # A real COM object: its first field is the vtable pointer, and Release is the third entry.
    vtable = ctypes.cast(factory, ctypes.POINTER(ctypes.c_void_p))[0]
    release = ctypes.cast(ctypes.cast(vtable, ctypes.POINTER(ctypes.c_void_p))[2],
                          ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p))
    remaining = release(factory)
    print(f"the factory is a live COM object; refcount after release: {remaining}")
    print("RESULT: the proxy forwards correctly")
else:
    print("RESULT: FORWARDING FAILED")
    raise SystemExit(1)

# The other exports must at least resolve, or a process importing one of them would fail to start.
missing = []
for name in ("ApplyCompatResolutionQuirking", "CompatString", "CompatValue", "CreateDXGIFactory",
             "CreateDXGIFactory2", "DXGID3D10CreateDevice", "DXGID3D10CreateLayeredDevice",
             "DXGID3D10GetLayeredDeviceSize", "DXGID3D10RegisterLayers", "DXGIDeclareAdapterRemovalSupport",
             "DXGIDisableVBlankVirtualization", "DXGIDumpJournal", "DXGIGetDebugInterface1",
             "DXGIReportAdapterConfiguration", "PIXBeginCapture", "PIXEndCapture", "PIXGetCaptureState",
             "SetAppCompatStringPointer", "UpdateHMDEmulationStatus"):
    if not getattr(proxy, name, None):
        missing.append(name)

print("every other export resolves" if not missing else f"MISSING: {missing}")
