#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#define SDL_MAIN_HANDLED
#define SDL_STATIC_LIB
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <string.h>
#include <wchar.h>
#include <shellapi.h>
#include <stdlib.h>
#include "runtime_layout.h"

static HANDLE log_file;
static HMODULE self_image;
static WCHAR requested_scene[2048];
static WCHAR requested_skin[2048];
static WCHAR requested_play[256];
static BOOL dump_classes;
static BOOL probe_mode,dashboard_mode,diagnostics,blades_mode;
static BOOL audit_scenes;
static BOOL xbox_guide_mode;
static void log_text(const char *text) {
    DWORD n;WriteFile(log_file,text,lstrlenA(text),&n,0);WriteFile(log_file,"\r\n",2,&n,0);FlushFileBuffers(log_file);
}
static void log_value(const char *label,DWORD value) {
    char line[256];wsprintfA(line,"%s: 0x%08lX",label,value);log_text(line);
}
static void log_wide(const WCHAR *text) {
    char utf8[4096];if(text && WideCharToMultiByte(CP_UTF8,0,text,-1,utf8,sizeof(utf8),0,0))log_text(utf8);
}
/* Internal resource references to the former DLL resolve to this native EXE.
 * No game image, title catalog, DLL extraction, or XLiveInitialize call exists.
 */
static BOOL runtime_a(const char *name) {return name && !lstrcmpiA(name,"xlive.dll");}
static BOOL runtime_w(const WCHAR *name) {return name && !lstrcmpiW(name,L"xlive.dll");}
static HMODULE WINAPI module_a(LPCSTR name) {return runtime_a(name)?self_image:GetModuleHandleA(name);}
static HMODULE WINAPI module_w(LPCWSTR name) {return runtime_w(name)?self_image:GetModuleHandleW(name);}
static HMODULE WINAPI load_a(LPCSTR name) {return runtime_a(name)?self_image:LoadLibraryA(name);}
static HMODULE WINAPI load_w(LPCWSTR name) {return runtime_w(name)?self_image:LoadLibraryW(name);}
static HMODULE WINAPI load_ex_a(LPCSTR name,HANDLE file,DWORD flags) {return runtime_a(name)?self_image:LoadLibraryExA(name,file,flags);}
static HMODULE WINAPI load_ex_w(LPCWSTR name,HANDLE file,DWORD flags) {return runtime_w(name)?self_image:LoadLibraryExW(name,file,flags);}
static BOOL WINAPI free_module(HMODULE module) {return module==self_image?TRUE:FreeLibrary(module);}
static void adapt_import(DWORD rva,void *function) {
    DWORD old,ignored;void **slot=(void **)((BYTE *)self_image+rva);
    if(VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&old)) {
        *slot=function;VirtualProtect(slot,sizeof(*slot),old,&ignored);
    }
}
static LONG WINAPI crash_log(EXCEPTION_POINTERS *e) {
    log_value("Unhandled exception",e->ExceptionRecord->ExceptionCode);
    log_value("Exception address",(DWORD)e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
static BOOL verify_modules(void) {
    BOOL ok=TRUE;HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    if(snapshot==INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W item={.dwSize=sizeof(item)};
    for(BOOL found=Module32FirstW(snapshot,&item);found;found=Module32NextW(snapshot,&item)) {
        if(diagnostics) log_wide(item.szExePath);
        if(!lstrcmpiW(item.szModule,L"xlive.dll") || !lstrcmpiW(item.szModule,L"SDL3.dll")) ok=FALSE;
    }
    CloseHandle(snapshot);log_value("No xlive.dll or SDL3.dll loaded",ok);return ok;
}
#include "ui.c"

int main(void) {
    WCHAR path[MAX_PATH];GetModuleFileNameW(0,path,MAX_PATH);
    WCHAR *slash=wcsrchr(path,L'\\');
    blades_mode=!lstrcmpiW(slash+1,L"Blades-Dashboard.exe");
    lstrcpyW(slash+1,blades_mode?L"blades.log":L"guide.log");
    log_file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);SetUnhandledExceptionFilter(crash_log);
    int argc=0;LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;argv && i<argc;i++) {
        if(!wcscmp(argv[i],L"--probe")) probe_mode=TRUE;
        else if(!wcscmp(argv[i],L"--audit-scenes")) {audit_scenes=TRUE;probe_mode=TRUE;blades_mode=TRUE;}
        else if(!wcscmp(argv[i],L"--blades")) blades_mode=TRUE;
        else if(!wcscmp(argv[i],L"--guide")) {blades_mode=TRUE;xbox_guide_mode=TRUE;}
        else if(!wcscmp(argv[i],L"--pc-guide")) blades_mode=FALSE;
        else if(!wcscmp(argv[i],L"--dashboard")) dashboard_mode=TRUE;
        else if(!wcscmp(argv[i],L"--diagnostics")) diagnostics=TRUE;
        else if(!wcscmp(argv[i],L"--dump-classes")) dump_classes=TRUE;
        else if(!wcsncmp(argv[i],L"--scene=",8)) lstrcpynW(requested_scene,argv[i]+8,2048);
        else if(!wcsncmp(argv[i],L"--skin=",7)) lstrcpynW(requested_skin,argv[i]+7,2048);
        else if(!wcsncmp(argv[i],L"--play=",7)) lstrcpynW(requested_play,argv[i]+7,256);
        else if(wcsstr(argv[i],L".xur")) lstrcpynW(requested_scene,argv[i],2048);
    }
    LocalFree(argv);
    if(dashboard_mode) blades_mode=FALSE;
    if(blades_mode && !requested_scene[0]) {
        WCHAR base[2048];GetModuleFileNameW(0,base,2048);
        WCHAR *name=wcsrchr(base,L'\\');if(!name) return 10;
        *name=0;name=wcsrchr(base,L'\\');if(!name) return 10;
        lstrcpyW(name,L"/x360/blades-converted/");
        for(WCHAR *c=base;*c;c++) if(*c==L'\\') *c=L'/';
        wsprintfW(requested_skin,L"file://%sdash_dashuisk.xzp#skin.xur",base);
        wsprintfW(requested_scene,L"file://%sdash_dashmain.xzp#dashmain.xur",base);
    }
    self_image=GetModuleHandleW(0);
    adapt_import(IAT_GetModuleHandleA,module_a);adapt_import(IAT_GetModuleHandleW,module_w);
    adapt_import(IAT_LoadLibraryA,load_a);adapt_import(IAT_LoadLibraryW,load_w);
    adapt_import(IAT_LoadLibraryExA,load_ex_a);adapt_import(IAT_LoadLibraryExW,load_ex_w);
    adapt_import(IAT_FreeLibrary,free_module);
    BOOL (WINAPI *attach)(HINSTANCE,DWORD,LPVOID)=(void *)((BYTE *)self_image+ORIGINAL_ENTRY_RVA);
    if(!attach(self_image,DLL_PROCESS_ATTACH,0)) {log_text("Runtime process initialization failed");return 10;}
    log_text("Standalone XUI: direct engine startup; no game or XLive services");
    SDL_SetMainReady();
    if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD)) {log_text(SDL_GetError());return 11;}
    SDL_Window *window=SDL_CreateWindow("Standalone XUI | F1 Guide | F2 Dashboard | Esc Exit",1280,720,SDL_WINDOW_HIDDEN);
    if(!window) {log_text(SDL_GetError());SDL_Quit();return 11;}
    HWND hwnd=SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,0);
    IDirect3D9 *d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if(!d3d) {SDL_DestroyWindow(window);SDL_Quit();return 12;}
    D3DPRESENT_PARAMETERS pp={0};
    pp.BackBufferWidth=1280;pp.BackBufferHeight=720;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferCount=1;
    pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=hwnd;pp.Windowed=TRUE;pp.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *device=0;
    HRESULT hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,hwnd,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED|D3DCREATE_FPU_PRESERVE,&pp,&device);
    if(FAILED(hr)) hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,hwnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED|D3DCREATE_FPU_PRESERVE,&pp,&device);
    log_value("Graphics device",hr);
    if(SUCCEEDED(hr)) {
        hr=independent_ui(window,device,&pp);
        log_value("UI exit",hr);
        if(!verify_modules()) hr=E_FAIL;
        IDirect3DDevice9_Release(device);
    }
    IDirect3D9_Release(d3d);SDL_DestroyWindow(window);SDL_Quit();
    /* The OS releases image-wide CRT and compatibility state at process exit.
     * The XUI scene, render context, and input are explicitly released in ui.c. */
    if(FAILED(hr) && !probe_mode) MessageBoxW(0,L"The interface could not continue. See guide.log beside the executable.",L"Standalone XUI",MB_OK|MB_ICONERROR);
    CloseHandle(log_file);
    return FAILED(hr)?13:0;
}
