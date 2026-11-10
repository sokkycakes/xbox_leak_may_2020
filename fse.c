/* Windows 11 Full Screen Experience (FSE) home app mode.
 *
 * When Blades-Dashboard.exe runs as the FSE home app (it is started from its gamingHome app
 * package, see fse/AppxManifest.xml), or with --fse:
 *   - one instance only: a second start (FSE's "home" activation) brings the running dashboard
 *     to the front and exits;
 *   - the window starts borderless full screen;
 *   - while another program (a game) has the foreground, the dashboard stops drawing and ignores
 *     the pad, except the Xbox button: that opens the Guide over the game in a topmost layered
 *     window (the console HUD: Guide + notifications, title dimmed to 30% as xam does). B closes
 *     it and gives the game the foreground back; Y ("Xbox Dashboard") brings the dashboard up.
 *   - notifications raised while a game is up show over it without taking the focus.
 *
 * The overlay is drawn by the same XUI pass as the in-window HUD (ui_hud_render), into an
 * A8R8G8B8 target with separate alpha blending so the target holds premultiplied colour and
 * coverage, then read back and handed to UpdateLayeredWindow (per-pixel alpha, no DWM tricks,
 * works over borderless and flip-model games; true exclusive full screen games are not covered).
 *
 * Game Bar: the installer turns off Game Bar on the Xbox button (HKCU\Software\Microsoft\GameBar
 * LegacyGamepadXboxGuideButtonEnabled=0 in Xbox mode, UseNexusForGameBarEnabled=0 on the desktop,
 * the values AnyFSE uses), so the Xbox button reaches
 * applications (XInput's guide bit, read by SDL) instead of opening Game Bar. */
static BOOL fse_mode;
static HANDLE fse_activate_event;
static HWND fse_main,fse_overlay,fse_game;
static BOOL fse_overlay_shown,fse_overlay_active,fse_was_guide,fse_background;
static int fse_overlay_w,fse_overlay_h;
static IDirect3DSurface9 *fse_rt,*fse_screen_rt,*fse_readback,*fse_saved_rt;
static HDC fse_dc;static HBITMAP fse_bitmap,fse_old_bitmap;static void *fse_bits;
static ULONGLONG fse_activate_tick;

/* Packaged (started through the app package by FSE / Start) -> FSE mode. */
static BOOL fse_packaged(void) {
    LONG (WINAPI *name)(UINT32 *,PWSTR)=(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetCurrentPackageFullName");
    UINT32 length=0;
    return name && name(&length,0)==ERROR_INSUFFICIENT_BUFFER;
}
/* Called first thing in main(), before the log is opened (a second instance must not truncate
   the running one's log). Returns FALSE when this process should exit at once. */
static BOOL fse_startup(BOOL requested) {
    fse_mode=requested || fse_packaged();
    if(!fse_mode) return TRUE;
    CreateMutexW(0,TRUE,L"Local\\CrimsonBladesFSE.Instance");
    BOOL second=GetLastError()==ERROR_ALREADY_EXISTS;
    fse_activate_event=CreateEventW(0,FALSE,FALSE,L"Local\\CrimsonBladesFSE.Activate");
    if(second) {
        /* This process was just started by the shell, so it may hand the foreground on. */
        AllowSetForegroundWindow(ASFW_ANY);
        if(fse_activate_event) SetEvent(fse_activate_event);
        return FALSE;
    }
    return TRUE;
}
static void fse_force_foreground(HWND hwnd) {
    if(!hwnd) return;
    if(IsIconic(hwnd)) ShowWindow(hwnd,SW_RESTORE);
    HWND fg=GetForegroundWindow();
    if(fg==hwnd) return;
    DWORD mine=GetCurrentThreadId(),theirs=fg?GetWindowThreadProcessId(fg,0):0;
    if(theirs && theirs!=mine) AttachThreadInput(theirs,mine,TRUE);
    BringWindowToTop(hwnd);SetForegroundWindow(hwnd);
    if(theirs && theirs!=mine) AttachThreadInput(theirs,mine,FALSE);
    if(GetForegroundWindow()!=hwnd) {
        /* Foreground lock: a tap of Alt counts as this process's input and lifts it. */
        INPUT in[2]={0};in[0].type=in[1].type=INPUT_KEYBOARD;
        in[0].ki.wVk=in[1].ki.wVk=VK_MENU;in[1].ki.dwFlags=KEYEVENTF_KEYUP;
        SendInput(2,in,sizeof(INPUT));SetForegroundWindow(hwnd);
    }
    SetFocus(hwnd);
    log_value("FSE foreground handed over",GetForegroundWindow()==hwnd);
}
static LRESULT CALLBACK fse_overlay_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
        case WM_MOUSEACTIVATE: return fse_overlay_active?MA_ACTIVATE:MA_NOACTIVATE;
        case WM_KEYDOWN:
            /* Arrows, Enter, Backspace, Y reach the engine through SDL's message hook (ui_window_message);
               Escape is an SDL key event for the dashboard window only, so it is handled here. */
            if(w==VK_ESCAPE && fse_overlay_active && blades_ready && blades_guide_open) blades_pending_back=2;
            return 0;
        case WM_CLOSE: return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
static void fse_window_init(SDL_Window *window,HWND hwnd) {
    fse_main=hwnd;
    if(!fse_mode) return;
    SDL_SetWindowFullscreen(window,true);
    WNDCLASSW wc={0};wc.lpfnWndProc=fse_overlay_proc;wc.hInstance=GetModuleHandleW(0);
    wc.lpszClassName=L"CrimsonGuideOverlay";wc.hCursor=LoadCursorW(0,(LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    fse_overlay=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,
        wc.lpszClassName,L"Xbox Guide",WS_POPUP,0,0,1,1,0,0,wc.hInstance,0);
    log_value("FSE mode: Guide overlay window",fse_overlay!=0);
    {   /* Report whether the Xbox button still opens Game Bar (the installer turns it off). */
        static const WCHAR *names[2]={L"LegacyGamepadXboxGuideButtonEnabled",L"UseNexusForGameBarEnabled"};
        for(int i=0;i<2;i++) {
            DWORD value=1,size=sizeof value;
            LONG read=RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\GameBar",names[i],RRF_RT_REG_DWORD,0,&value,&size);
            log_text(i?"FSE: Game Bar on the Xbox button, desktop (UseNexusForGameBarEnabled)":"FSE: Game Bar on the Xbox button, Xbox mode (LegacyGamepadXboxGuideButtonEnabled)");
            log_value("  value (1 = Game Bar takes the button; the installer sets 0)",read==ERROR_SUCCESS?value:1);
        }
    }
}
static BOOL fse_is_ours(HWND hwnd) {return hwnd && (hwnd==fse_main || hwnd==fse_overlay);}
/* Pad events while another program has the foreground and no Guide is over it: only the Xbox
   button (opens the Guide over the game) gets through. */
static BOOL fse_drop_pad_event(const SDL_Event *event) {
    if(!fse_mode || !fse_background || fse_overlay_active) return FALSE;
    if(event->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event->type==SDL_EVENT_GAMEPAD_BUTTON_UP)
        return event->gbutton.button!=SDL_GAMEPAD_BUTTON_GUIDE;
    return event->type==SDL_EVENT_GAMEPAD_AXIS_MOTION;
}
static void fse_surfaces_free(void) {
    if(fse_rt) {IDirect3DSurface9_Release(fse_rt);fse_rt=0;}
    if(fse_screen_rt) {IDirect3DSurface9_Release(fse_screen_rt);fse_screen_rt=0;}
    if(fse_readback) {IDirect3DSurface9_Release(fse_readback);fse_readback=0;}
    if(fse_dc) {SelectObject(fse_dc,fse_old_bitmap);DeleteObject(fse_bitmap);DeleteDC(fse_dc);fse_dc=0;fse_bitmap=0;fse_bits=0;}
}
static void fse_overlay_place(HWND over) {
    HMONITOR monitor=MonitorFromWindow(over?over:fse_main,MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info={sizeof info};GetMonitorInfoW(monitor,&info);
    int w=info.rcMonitor.right-info.rcMonitor.left,h=info.rcMonitor.bottom-info.rcMonitor.top;
    if(w!=fse_overlay_w || h!=fse_overlay_h) {fse_surfaces_free();fse_overlay_w=w;fse_overlay_h=h;}
    SetWindowPos(fse_overlay,HWND_TOPMOST,info.rcMonitor.left,info.rcMonitor.top,w,h,SWP_NOACTIVATE);
}
static void fse_overlay_show(BOOL interactive) {
    if(!fse_overlay) return;
    LONG ex=GetWindowLongW(fse_overlay,GWL_EXSTYLE);
    LONG want=interactive?(ex&~(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT)):(ex|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT);
    if(want!=ex) SetWindowLongW(fse_overlay,GWL_EXSTYLE,want);
    if(!fse_overlay_shown) {
        fse_overlay_place(fse_game?fse_game:GetForegroundWindow());
        ShowWindow(fse_overlay,SW_SHOWNOACTIVATE);fse_overlay_shown=TRUE;
        log_value("FSE overlay shown, interactive",interactive);
    }
    if(interactive && !fse_overlay_active) {fse_overlay_active=TRUE;fse_force_foreground(fse_overlay);}
    fse_overlay_active=interactive;
}
static void fse_overlay_hide(void) {
    if(fse_overlay_shown) {ShowWindow(fse_overlay,SW_HIDE);fse_overlay_shown=FALSE;log_text("FSE overlay hidden");}
    fse_overlay_active=FALSE;
}
/* Cloud tests: FSE_TEST_KEYS="ms:key,..." presses keys at fixed times after start (G Guide button,
   B, Y, A, U/D/L/R), whichever window has the foreground, as a pad would. */
static void fse_test_keys(void) {
    static char keys[512];static char *next;static ULONGLONG start;
    if(!next) {if(!GetEnvironmentVariableA("FSE_TEST_KEYS",keys,sizeof keys)) keys[0]=0;next=keys;start=GetTickCount64();}
    if(!*next) return;
    ULONGLONG due=(ULONGLONG)atoi(next);char *colon=strchr(next,':');if(!colon) {*next=0;return;}
    if(GetTickCount64()-start<due) return;
    char k=colon[1];next=strchr(colon,',');next=next?next+1:colon+strlen(colon);
    log_value("FSE test key",k);
    if(k=='G') ui_switch(0);
    else {WORD key=k=='B'?VK_BACK:k=='Y'?'Y':k=='U'?VK_UP:k=='D'?VK_DOWN:k=='L'?VK_LEFT:k=='R'?VK_RIGHT:VK_RETURN;ui_key(key,TRUE);ui_key(key,FALSE);}
}
/* Once per frame, before rendering. Returns how the frame is drawn:
   0 = skip drawing (another program is in front), 1 = dashboard window as usual, 2 = overlay. */
static int fse_frame(void) {
    if(!fse_mode) return 1;
    fse_test_keys();
    if(fse_activate_event && WaitForSingleObject(fse_activate_event,0)==WAIT_OBJECT_0) {
        /* FSE home activation (a second start): the dashboard comes to the front, Guide closed. */
        log_text("FSE: home activation");
        fse_game=0;fse_to_dash=FALSE;fse_overlay_hide();
        if(blades_ready && blades_guide_open) blades_guide(FALSE);
        fse_force_foreground(fse_main);fse_activate_tick=GetTickCount64();
    }
    HWND fg=GetForegroundWindow();
    BOOL guide_up=blades_ready && blades_guide_open;
    BOOL hud_up=guide_up || lstrcmpW(guide_state,L"Closed");
    if(guide_up && !fse_was_guide) {
        /* The Guide just opened: over a game if something else had the foreground. */
        fse_to_dash=FALSE;
        if(fg && !fse_is_ours(fg) && GetTickCount64()-fse_activate_tick>1000) {
            fse_game=fg;
            char title[128]="";GetWindowTextA(fg,title,sizeof title);log_text("FSE: Guide opened over");log_text(title);
            fse_overlay_show(TRUE);
        }
    }
    if(!guide_up && fse_was_guide && fse_game) {
        if(fse_to_dash) {
            /* Y: Xbox Dashboard. The game keeps running behind it. */
            log_text("FSE: Guide closed to the dashboard");
            fse_game=0;fse_overlay_hide();fse_force_foreground(fse_main);
        } else {
            fse_overlay_active=FALSE;
            fse_force_foreground(IsWindow(fse_game)?fse_game:fse_main);
        }
    }
    fse_was_guide=guide_up;
    if(fse_game && !hud_up && !guide_up) {fse_game=0;if(!live_toast.scene) fse_overlay_hide();}
    if(fse_game && !IsWindow(fse_game)) fse_game=0;
    fg=GetForegroundWindow();
    fse_background=fg && !fse_is_ours(fg);
    if(fse_overlay_active && fg!=fse_overlay) fse_background=FALSE;   /* the overlay is being brought up */
    if(fse_game && hud_up) {if(!fse_overlay_shown || fse_overlay_active!=guide_up) fse_overlay_show(guide_up);return 2;}
    if(fse_background && live_toast.scene) {fse_overlay_show(FALSE);return 2;}
    if(fse_overlay_shown) fse_overlay_hide();
    return fse_background?0:1;
}
/* Overlay frame: render target = A8R8G8B8 1280x720, cleared to transparent (or to 70% black
   while the Guide is up: the title at 30%, xam 0x818dc0f8). */
static BOOL fse_overlay_begin(IDirect3DDevice9 *device) {
    if(!fse_rt && FAILED(IDirect3DDevice9_CreateRenderTarget(device,1280,720,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&fse_rt,0))) {fse_rt=0;return FALSE;}
    if(!fse_screen_rt && FAILED(IDirect3DDevice9_CreateRenderTarget(device,fse_overlay_w,fse_overlay_h,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&fse_screen_rt,0))) {fse_screen_rt=0;return FALSE;}
    if(!fse_readback && FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(device,fse_overlay_w,fse_overlay_h,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&fse_readback,0))) {fse_readback=0;return FALSE;}
    if(!fse_dc) {
        BITMAPINFO bi={0};bi.bmiHeader.biSize=sizeof bi.bmiHeader;bi.bmiHeader.biWidth=fse_overlay_w;bi.bmiHeader.biHeight=-fse_overlay_h;
        bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
        fse_dc=CreateCompatibleDC(0);fse_bitmap=CreateDIBSection(fse_dc,&bi,DIB_RGB_COLORS,&fse_bits,0,0);
        if(!fse_bitmap) {DeleteDC(fse_dc);fse_dc=0;return FALSE;}
        fse_old_bitmap=SelectObject(fse_dc,fse_bitmap);
    }
    if(FAILED(IDirect3DDevice9_GetRenderTarget(device,0,&fse_saved_rt))) fse_saved_rt=0;
    IDirect3DDevice9_SetRenderTarget(device,0,fse_rt);
    BOOL dim=fse_game && lstrcmpW(guide_state,L"Closed");
    IDirect3DDevice9_Clear(device,0,0,D3DCLEAR_TARGET,dim?0xb3000000:0,1,0);
    /* Coverage: alpha = src + dst*(1-src) whatever colour blend the engine picks. */
    IDirect3DDevice9_SetRenderState(device,D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
    IDirect3DDevice9_SetRenderState(device,D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(device,D3DRS_DESTBLENDALPHA,D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(device,D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD);
    return TRUE;
}
static void fse_overlay_end(IDirect3DDevice9 *device) {
    IDirect3DDevice9_SetRenderState(device,D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
    if(fse_saved_rt) {IDirect3DDevice9_SetRenderTarget(device,0,fse_saved_rt);IDirect3DSurface9_Release(fse_saved_rt);fse_saved_rt=0;}
    HRESULT hr=IDirect3DDevice9_StretchRect(device,fse_rt,0,fse_screen_rt,0,D3DTEXF_LINEAR);
    if(SUCCEEDED(hr)) hr=IDirect3DDevice9_GetRenderTargetData(device,fse_screen_rt,fse_readback);
    D3DLOCKED_RECT locked;
    if(SUCCEEDED(hr)) hr=IDirect3DSurface9_LockRect(fse_readback,&locked,0,D3DLOCK_READONLY);
    if(FAILED(hr)) {static int failures;if(failures++<3) log_value("FSE overlay readback",hr);return;}
    for(int y=0;y<fse_overlay_h;y++) memcpy((BYTE *)fse_bits+y*fse_overlay_w*4,(BYTE *)locked.pBits+y*locked.Pitch,fse_overlay_w*4);
    IDirect3DSurface9_UnlockRect(fse_readback);
    GdiFlush();
    if(getenv("FSE_OVERLAY_DUMP")) {   /* cloud tests: the premultiplied BGRA the layered window gets */
        static int dumps;char path[MAX_PATH];
        if(dumps<40 && (GetTickCount64()/500)%2==0) {
            wsprintfA(path,"%s\\overlay-%02d.bgra",getenv("FSE_OVERLAY_DUMP"),dumps++);
            HANDLE f=CreateFileA(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0);DWORD n;
            if(f!=INVALID_HANDLE_VALUE) {WriteFile(f,fse_bits,fse_overlay_w*fse_overlay_h*4,&n,0);CloseHandle(f);}
        }
    }
    RECT r;GetWindowRect(fse_overlay,&r);
    POINT at={r.left,r.top},zero={0,0};SIZE size={fse_overlay_w,fse_overlay_h};
    BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    if(!UpdateLayeredWindow(fse_overlay,0,&at,&size,fse_dc,&zero,0,&blend,ULW_ALPHA)) {
        static int failures;if(failures++<3) log_value("FSE UpdateLayeredWindow error",GetLastError());
    }
}
static void fse_device_lost(void) {fse_surfaces_free();}
