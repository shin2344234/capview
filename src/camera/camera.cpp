// capview-camera.dll: the CapView Camera DirectShow source filter. CapView.exe sends its picture
// through shared memory (softcamcore/FrameBuffer) and this filter hands it to OBS, Discord, Zoom,
// browsers and any other app that opens a DirectShow camera.
//
// The filter is softcam by tshino (https://github.com/tshino/softcam, MIT, see LICENSE in this folder)
// with CapView's changes marked "CapView:". This file replaces softcam's softcam.cpp. It drops the
// sender exports, since CapView.exe builds the sender in, and adds a per-user install.
//
// regsvr32 capview-camera.dll            installs for all users (needs an elevated prompt)
// regsvr32 /n /i:user capview-camera.dll installs for the current user, no elevation
// CapView runs the per-user install through DllInstall in a child process of its own.

#include <baseclasses/streams.h>
#include <olectl.h>
#include <initguid.h>

#include <string>

#include <softcamcore/DShowSoftcam.h>

// {7BC6709A-957E-427E-A58A-24FAE7A4940C}
DEFINE_GUID(CLSID_CapViewCamera,
0x7bc6709a, 0x957e, 0x427e, 0xa5, 0x8a, 0x24, 0xfa, 0xe7, 0xa4, 0x94, 0x0c);

namespace {

const wchar_t FILTER_NAME[] = L"CapView Camera";
const GUID &FILTER_CLASSID = CLSID_CapViewCamera;

const AMOVIESETUP_MEDIATYPE s_pin_types[] =
{
    {
        &MEDIATYPE_Video,       // Major type
        &MEDIASUBTYPE_NULL      // Minor type
    }
};

const AMOVIESETUP_PIN s_pins[] =
{
    {
        const_cast<LPWSTR>(L"Output"),  // Pin string name
        FALSE,                  // Is it rendered
        TRUE,                   // Is it an output
        FALSE,                  // Can we have none
        FALSE,                  // Can we have many
        &CLSID_NULL,            // Connects to filter
        NULL,                   // Connects to pin
        1,                      // Number of types
        s_pin_types             // Pin details
    }
};

const REGFILTER2 s_reg_filter2 =
{
    1,
    MERIT_DO_NOT_USE,
    1,
    s_pins
};

CUnknown * WINAPI CreateCameraInstance(LPUNKNOWN lpunk, HRESULT *phr)
{
    return softcam::Softcam::CreateInstance(lpunk, FILTER_CLASSID, phr);
}

std::wstring GuidString(const GUID& g)
{
    wchar_t s[64] = {};
    StringFromGUID2(g, s, 64);
    return s;
}

HRESULT FromWin32(LONG r)
{
    return r == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(r);
}

// CLSID\{filter}: the COM server entry that points at this DLL. GetModuleFileNameW keeps a user
// folder with non-ASCII letters intact, where baseclasses' AMovieDllRegisterServer2 uses the ANSI call.
HRESULT WriteServer(HKEY classes)
{
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(g_hInst, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0]))
    {
        return E_FAIL;
    }
    std::wstring key = L"CLSID\\" + GuidString(FILTER_CLASSID);
    HKEY k = nullptr;
    LONG r = RegCreateKeyExW(classes, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS)
    {
        return FromWin32(r);
    }
    r = RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)FILTER_NAME, (DWORD)sizeof(FILTER_NAME));
    HKEY s = nullptr;
    if (r == ERROR_SUCCESS)
    {
        r = RegCreateKeyExW(k, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &s, nullptr);
    }
    if (r == ERROR_SUCCESS)
    {
        r = RegSetValueExW(s, nullptr, 0, REG_SZ, (const BYTE*)path, (n + 1) * sizeof(wchar_t));
    }
    if (r == ERROR_SUCCESS)
    {
        const wchar_t both[] = L"Both";
        r = RegSetValueExW(s, L"ThreadingModel", 0, REG_SZ, (const BYTE*)both, (DWORD)sizeof(both));
    }
    if (s)
    {
        RegCloseKey(s);
    }
    RegCloseKey(k);
    return FromWin32(r);
}

HRESULT DeleteServer(HKEY classes)
{
    std::wstring key = L"CLSID\\" + GuidString(FILTER_CLASSID);
    LONG r = RegDeleteTreeW(classes, key.c_str());
    return r == ERROR_FILE_NOT_FOUND ? S_OK : FromWin32(r);
}

// Installs or removes the filter in the video capture category, for all users or for the current
// user. IFilterMapper2 only writes through HKEY_CLASSES_ROOT, so for a per-user install that key is
// pointed at HKCU\Software\Classes while it writes. The mapper is created before the switch, since
// its own class is only registered for the machine. Apps that run elevated don't read per-user
// classes, so they only see a camera installed for all users.
HRESULT Setup(bool install, bool user)
{
    HRESULT hrInit = CoInitialize(nullptr);
    IFilterMapper2 *mapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, (void**)&mapper);
    HKEY classes = nullptr;
    if (SUCCEEDED(hr))
    {
        LONG r = user ? RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0,
                                        KEY_ALL_ACCESS, nullptr, &classes, nullptr)
                      : RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes", 0, KEY_ALL_ACCESS, &classes);
        hr = FromWin32(r);
    }
    if (SUCCEEDED(hr) && install)
    {
        hr = WriteServer(classes);
    }
    if (SUCCEEDED(hr))
    {
        bool overridden = user && RegOverridePredefKey(HKEY_CLASSES_ROOT, classes) == ERROR_SUCCESS;
        if (user && !overridden)
        {
            hr = E_FAIL;
        }
        else
        {
            HRESULT un = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, FILTER_CLASSID);
            if (install)
            {
                hr = mapper->RegisterFilter(FILTER_CLASSID, FILTER_NAME, nullptr,
                                            &CLSID_VideoInputDeviceCategory, nullptr, &s_reg_filter2);
            }
            else if (FAILED(un) && un != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            {
                hr = un;
            }
        }
        if (overridden)
        {
            RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
        }
    }
    if (classes && !install)
    {
        HRESULT del = DeleteServer(classes);
        if (SUCCEEDED(hr))
        {
            hr = del;
        }
    }
    if (classes)
    {
        RegCloseKey(classes);
    }
    if (mapper)
    {
        mapper->Release();
    }
    CoFreeUnusedLibraries();
    if (SUCCEEDED(hrInit))
    {
        CoUninitialize();
    }
    return hr;
}

} // namespace

// COM global table of objects in this dll

CFactoryTemplate g_Templates[] =
{
    {
        FILTER_NAME,
        &FILTER_CLASSID,
        &CreateCameraInstance,
        NULL,
        nullptr
    }
};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);


STDAPI DllRegisterServer()
{
    return Setup(true, false);
}

STDAPI DllUnregisterServer()
{
    return Setup(false, false);
}

STDAPI DllInstall(BOOL install, LPCWSTR cmdLine)
{
    bool user = cmdLine && _wcsicmp(cmdLine, L"user") == 0;
    return Setup(install != FALSE, user);
}

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

BOOL APIENTRY DllMain(HANDLE hModule, DWORD  dwReason, LPVOID lpReserved)
{
    return DllEntryPoint((HINSTANCE)(hModule), dwReason, lpReserved);
}
