/* Direct XUI startup. Deliberately independent of XLive/title initialization. */
#define XUI_FN(address, result, ...) ((result (WINAPI *)(__VA_ARGS__))((BYTE *)self_image + (address) - 0x400000))
#define XUI_C(address, result, ...) ((result (__cdecl *)(__VA_ARGS__))((BYTE *)self_image + (address) - 0x400000))
#define XUI_THIS(address, result, ...) ((result (__thiscall *)(__VA_ARGS__))((BYTE *)self_image + (address) - 0x400000))
#define XUI_GLOBAL(address, type) (*(type *)((BYTE *)self_image + (address) - 0x400000))
#define UI_CHECK(label, expression) do { hr=(expression); log_value(label,hr); if (FAILED(hr)) return hr; } while(0)
#include "xui_api.h"
#include "dashboard.c"
static HRESULT WINAPI ui_missing_class(void *self,const WCHAR *name,void **result) {
    log_text("Scene extension using its declared base class:"); log_wide(name); *result=0; return E_NOTIMPL;
}
static HRESULT WINAPI ui_resource_error(void *self,HRESULT hr,const WCHAR *uri) {
    /* XUI probes localized alternatives before falling back to the base asset. */
    if(diagnostics) {log_value("XUI resource lookup",hr);log_wide(uri);} return S_OK;
}
static void *ui_callbacks[6];
static void **ui_callback_object=ui_callbacks;
static DWORD ui_root;
static DWORD ui_scene,ui_background,ui_demo,ui_external;
static int ui_mode;
static float ui_offset_x=240,ui_offset_y=60;
static BOOL ui_scene_native;
static float ui_scale=1,ui_scale_y=1;
static BOOL ui_dash_zoom;
static DWORD ui_zoom_frames;
static IDirect3DSurface9 *ui_zoom_surface;
static int demo_selection,ui_pending;
static BOOL ui_quit;
static SDL_Gamepad *ui_pads[4];  /* open game controllers (settings: rumble, battery) */
static BOOL ui_shot_request;  /* probe key S: save shot-NN.png */
static int ui_shot_count;
static BOOL blades_message(DWORD element,DWORD *message);
static BOOL blades_switch(int mode);
static BOOL blades_in_submenu(void);
static BOOL blades_key(WORD key,BOOL down);
/* Local profiles (guide_profiles.c). */
static void profiles_dash_ready(void);
static const WCHAR *profiles_menu_file(void);
static const WCHAR *profiles_menu_scene(void);
static BOOL profiles_key(WORD key,BOOL down);
static BOOL profiles_pc_key(WORD key,BOOL down);
static void profiles_text(const char *utf8);
static void profiles_pc_char(WCHAR c);
static BOOL profiles_wants_text(void);
static void profiles_guide_opened(void);
static void profiles_meta(DWORD meta);
static BOOL ui_focus_first(DWORD parent) {
    DWORD button_class=XUI_FN(0x7dec34,DWORD,const WCHAR *)(L"XuiButton");
    if(XUI_FN(0x7d876c,DWORD,DWORD,DWORD)(parent,button_class) && SUCCEEDED(xui_focus(parent))) return TRUE;
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(parent,&child);
    while(child) {if(ui_focus_first(child)) return TRUE;XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
    return FALSE;
}
static BOOL guide_filter(DWORD element,DWORD *message);
static BOOL sound_filter(DWORD element,DWORD *message);
static void sound_init(void);
static HRESULT WINAPI ui_message_filter(DWORD element,DWORD *message) {
    if(sound_filter(element,message)) return S_OK;  /* XuiSound play/stop (guide_sound.c) */
    if(guide_filter(element,message)) return S_OK;
    if(blades_message(element,message)) return S_OK;
    if(message[1]==0xe && message[4]) {
        DWORD *data=(DWORD *)message[4];
        if(data[0]==1 && (element==ui_scene || element==ui_demo || element==ui_background)) {
            if(element==ui_demo) {
                for(int i=0;i<3;i++) if(data[1]==dashboard_buttons[i]) ui_pending=10+i;
            } else if(data[1]==xui_find(ui_background,L"CloseX")) ui_quit=TRUE;
            else if(data[1]==xui_find(ui_scene,L"btnSignIn")) ui_pending=1;
            else if(data[1]==xui_find(ui_scene,L"btnNewProfile")) ui_pending=2;
            message[2]=1;
            log_value("Standalone button action",ui_pending);
        }
    }
    return S_OK;
}
static void ui_switch(int mode) {
    if(blades_switch(mode)) return;
    ui_mode=mode;
    xui_bool(ui_background,L"Show",mode==0);
    xui_bool(ui_demo,L"Show",mode==1);
    if(ui_external) xui_bool(ui_external,L"Show",mode==2);
    ui_offset_x=mode?0:240;ui_offset_y=mode?0:60;ui_scale=ui_scale_y=1;
    if(mode<2) xui_focus(mode?dashboard_buttons[0]:xui_find(ui_scene,L"btnSignIn"));
    else if(ui_external) ui_focus_first(ui_external);
    demo_selection=0;
}
/* Blades navigation: dashmain's RootScene owns the "1To2".."4To3" frames that the
   dashboard code plays when the shoulder buttons move between blades
   (1 xbox live, 2 games, 3 media, 4 system). */
static DWORD ui_blade_root;
static BOOL guide_open,guide_at_main;
static void ui_guide_toggle(void);
static int ui_blade=2;
static DWORD ui_find_deep(DWORD element,const WCHAR *id,int depth) {
    DWORD *data=XUI_FN(0x7d268f,void *,DWORD,DWORD)(element,XUI_GLOBAL(0x8f1188,DWORD));
    if(data && data[1] && !lstrcmpW((WCHAR *)data[1],id)) return element;
    if(depth>12) return 0;
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(element,&child);
    while(child) {DWORD f=ui_find_deep(child,id,depth+1);if(f) return f;XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
    return 0;
}
static DWORD ui_find_frames(DWORD element,const WCHAR *frame,int depth) {
    int index=-1;XUI_FN(0x7d389d,HRESULT,DWORD,const WCHAR *,int *)(element,frame,&index);
    if(index>=0) return element;
    if(depth>8) return 0;
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(element,&child);
    while(child) {
        DWORD found=ui_find_frames(child,frame,depth+1);if(found) return found;
        XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);
    }
    return 0;
}
static void ui_dash_enter(int blade);
static BOOL ui_blade_move(int step) {
    int next=ui_blade+step;
    if(!ui_blade_root || next<1 || next>4) return FALSE;
    WCHAR start[16],end[16];wsprintfW(start,L"%dTo%d",ui_blade,next);wsprintfW(end,L"%sEnd",start);
    int first=-1,last=-1;
    XUI_FN(0x7d389d,HRESULT,DWORD,const WCHAR *,int *)(ui_blade_root,start,&first);
    XUI_FN(0x7d389d,HRESULT,DWORD,const WCHAR *,int *)(ui_blade_root,end,&last);
    if(first<0 || last<0) return FALSE;
    HRESULT hr=XUI_FN(0x7d3867,HRESULT,DWORD,int,int,int,BOOL,BOOL)(ui_blade_root,first,first,last,FALSE,TRUE);
    log_wide(start);log_value("Blade move",hr);
    if(SUCCEEDED(hr)) {ui_blade=next;ui_dash_enter(next);}
    return TRUE;
}
static void ui_key(WORD key,BOOL down) {
    if(profiles_key(key,down)) return;  /* virtual keyboard: X/Y/LB/RB edit the name */
    if(blades_key(key,down)) return;
    if(guide_open && (key==VK_BACK || key=='Y')) {
        /* Guide: Y returns to the Xbox Dashboard; B backs out of a submenu, or closes the Guide from its main menu. */
        if(down && (key=='Y' || guide_at_main)) {ui_guide_toggle();return;}
        if(key==VK_BACK) key=0x5801;
    }
    if(ui_mode==2 && ui_blade_root && !guide_open && !blades_in_submenu() && (key==VK_LEFT || key==VK_RIGHT || key==VK_PRIOR || key==VK_NEXT)) {
        if(down) ui_blade_move(key==VK_LEFT || key==VK_PRIOR?-1:1);
        return;
    }
    if(ui_mode==1 && (key==VK_UP || key==VK_DOWN)) {
        if(down) {demo_selection=(demo_selection+(key==VK_DOWN?1:2))%3;xui_focus(dashboard_buttons[demo_selection]);}
        return;
    }
    if(key==VK_RETURN) key=0x5800;
    struct {WORD key,unicode,flags;BYTE user,hid;} stroke={key,0,down?1:2,0,0};
    XUI_FN(0x7d4af9,HRESULT,void *)(&stroke);
}
static DWORD live_mouse_begin(void);       /* guide_live.c: mouse held while a message box is up */
static void live_mouse_end(DWORD focus);
static bool SDLCALL ui_window_message(void *unused,MSG *m) {
    if(!ui_root) return true;
    if(m->message==WM_KEYDOWN || m->message==WM_KEYUP) {
        if(m->wParam==VK_ESCAPE || m->wParam>=VK_F1 && m->wParam<=VK_F12) return true;
        if(profiles_pc_key((WORD)m->wParam,m->message==WM_KEYDOWN)) return true;  /* typing into the virtual keyboard */
        ui_key((WORD)m->wParam,m->message==WM_KEYDOWN);
    } else if(m->message==WM_CHAR) {
        profiles_pc_char((WCHAR)m->wParam);
    } else if(m->message>=WM_MOUSEFIRST && m->message<=WM_MOUSELAST) {
        DWORD message[5]={0},data[8]={0};
        RECT client;GetClientRect(m->hwnd,&client);
        if(!client.right || !client.bottom) return true;
        LPARAM xy=MAKELPARAM((short)(((short)LOWORD(m->lParam)*1280.0f/client.right-ui_offset_x)/ui_scale),(short)(((short)HIWORD(m->lParam)*720.0f/client.bottom-ui_offset_y)/ui_scale_y));
        DWORD modal=live_mouse_begin();
        if(SUCCEEDED(XUI_FN(0x7e2095,HRESULT,HWND,void *,void *,UINT,WPARAM,LPARAM)(m->hwnd,message,data,m->message,m->wParam,xy)))
            XUI_FN(0x7e22f9,HRESULT,DWORD,void *)(ui_root,message);
        live_mouse_end(modal);
    }
    return true;
}
static HRESULT ui_resource_uri(const WCHAR *path,WCHAR *uri) {
    WCHAR absolute[2048];
    if(wcsstr(path,L"://")) {lstrcpynW(uri,path,4096);return S_OK;}
    DWORD length=GetFullPathNameW(path,2048,absolute,0);
    if(!length || length>=2048) return E_INVALIDARG;
    lstrcpyW(uri,L"file://");lstrcatW(uri,absolute);return S_OK;
}
/* Named frames usually live on a child scene (dashmain keeps them on RootScene), so
   try each element depth-first until one owns the range. */
static HRESULT ui_play_tree(DWORD element,const WCHAR *start,const WCHAR *end,int depth) {
    /* 0x7d4c5b only plays a control's visual, so look the frames up on the element itself. */
    int first=-1,last=-1;
    XUI_FN(0x7d389d,HRESULT,DWORD,const WCHAR *,int *)(element,start,&first);
    XUI_FN(0x7d389d,HRESULT,DWORD,const WCHAR *,int *)(element,end,&last);
    HRESULT hr=first>=0 && last>=0?XUI_FN(0x7d3867,HRESULT,DWORD,int,int,int,BOOL,BOOL)(element,first,first,last,FALSE,TRUE):E_INVALIDARG;
    if(SUCCEEDED(hr)) {
        DWORD *data=XUI_FN(0x7d268f,void *,DWORD,DWORD)(element,XUI_GLOBAL(0x8f1188,DWORD));
        log_text("Timeline played on");if(data) log_wide((WCHAR *)data[1]);
        return hr;
    }
    if(depth>8) return hr;
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(element,&child);
    while(child) {
        HRESULT found=ui_play_tree(child,start,end,depth+1);
        if(SUCCEEDED(found)) return found;
        XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);
    }
    return hr;
}
/* Dashboard code (dash.xex's job, done here because the scenes only carry layout).
   Each blade's signed-out scene goes into its tab's scContainer (live.xzp, gamesblade.xzp,
   mediablade.xzp; Tab4 holds the System blade inline). Focusing a list item swaps the
   right-hand metaPanelScene: the games and media scenes list the meta scene per button
   (DashScene properties), the System blade lists description text per button. Live shows
   the default Billboard of the Day (botd.xzp defaultbanner0). Strings come from
   dashcomm.xzp dashStrings.xus, as the dash reads them. */
#define DASH_META_MAX 8
typedef struct {const WCHAR *tab,*content,*package,*panel;float x,y;const WCHAR *buttons[DASH_META_MAX],*metas[DASH_META_MAX];} DashBlade;
static DashBlade dash_blades[4]={  /* the profiles module swaps blades 1-3 to the signed-in scenes */
    {L"Tab1",L"liveSignedOutUI.xur",L"dash_live.xzp",L"MetaContainer",354,118,{L"fakeGamerCard"},{L"dash_botd.xzp#defaultbanner0.xur"}},
    {L"Tab2",L"gamesSignedOut.xur",L"dash_gamesbla.xzp",L"metaPanelScene",0,0,
        {L"fakeGamerCard",L"navCreateProfile",L"navArcade",L"navDemos",L"navTrailers"},
        {L"gamesMetaMOTD.xur",L"gamesMetaNewProfile.xur",L"gamesMetaArcade.xur",L"gamesMetaDemos.xur",L"gamesMetaTrailers.xur"}},
    {L"Tab3",L"mediaSignedOut.xur",L"dash_mediabla.xzp",L"metaPanelScene",0,0,
        {L"fakeGamerCard",L"navMusic",L"navPictures",L"navVideo",L"navMCX"},
        {L"mediaMetaMOTD.xur",L"mediaMetaMusic.xur",L"mediaMetaPictures.xur",L"mediaMetaVideo.xur",L"mediaMetaMCX.xur"}},
    {L"Tab4",0,L"dash_gamesbla.xzp",L"metaPanelScene",0,0,
        {L"navSettings",L"navPControls",L"navMemory",L"navNetwork",L"navWindowMediaConnect",L"navSystemSetUp"},
        {L"Edit your Xbox 360 system settings, including:\r\n\r\n\x2022 Display\r\n\x2022 Audio\r\n\x2022 Language\r\n\x2022 Remote control\r\n\x2022 and more",
         L"Protect younger family members by customizing content, communications, and online interactions. Adjust settings on the console or at the individual profile level.",
         L"Move or delete saved games, profiles, and other items on Xbox 360 storage devices.",
         L"Connect your console to your home network or Xbox Live. Set up wireless network connections and test network settings.",
         L"Manage your connection to a Windows XP or Media Center PC.",
         L"Run the system setup that you saw the first time you started the console."}}};
static DWORD dash_root[4],dash_panel[4],dash_meta[4],*dash_app;
static int dash_shown[4]={-1,-1,-1,-1};
static DWORD dash_focused;
static WCHAR dash_base[4096],ui_skin_uri[4096];
static DWORD dash_scene(const WCHAR *file) {
    WCHAR path[4096];lstrcpyW(path,dash_base);lstrcatW(path,file);
    DWORD scene=0;HRESULT hr=XUI_FN(0x7d5ea8,HRESULT,void *,const WCHAR *,void *,DWORD *)(0,path,dash_app,&scene);
    if(FAILED(hr)) {log_wide(path);log_value("Dash scene",hr);return 0;}
    return scene;
}
static void dash_attach(DWORD parent,DWORD child,float x,float y) {
    float position[3]={x,y,0};
    XUI_FN(0x7d34bc,HRESULT,DWORD,const float *)(child,position);
    if(SUCCEEDED(xui_add(parent,child))) xui_visuals(child); else XUI_FN(0x7dec45,HRESULT,DWORD)(child);
}
static void dash_set_text(DWORD root,const WCHAR *id,const WCHAR *text) {
    DWORD e=root?ui_find_deep(root,id,0):0;
    if(e) xui_string(e,L"Text",text);  /* the SetText helper refuses dash controls (0x80300016) */
}
static void dash_show_meta(int blade,int index) {
    const DashBlade *b=&dash_blades[blade];
    if(!dash_panel[blade] || index<0 || !b->metas[index] || dash_shown[blade]==index) return;
    WCHAR file[512];const WCHAR *text=0;
    if(b->content) {
        if(wcschr(b->metas[index],L'#')) lstrcpyW(file,b->metas[index]);
        else wsprintfW(file,L"%s#%s",b->package,b->metas[index]);
    } else {wsprintfW(file,L"%s#gamesMetaNewProfile.xur",b->package);text=b->metas[index];}
    DWORD meta=dash_scene(file);if(!meta) return;
    if(dash_meta[blade]) XUI_FN(0x7dec45,HRESULT,DWORD)(dash_meta[blade]);
    dash_attach(dash_panel[blade],meta,b->x,b->y);
    if(text) {  /* System descriptions reuse the games meta label, sized to the taller panel. */
        DWORD label=ui_find_deep(meta,L"labDefaultText",0);
        if(label) {xui_string(label,L"Text",text);xui_bounds(label,0,5,342,440);}
    }
    dash_meta[blade]=meta;dash_shown[blade]=index;
    profiles_meta(meta);    /* CBladeMOTD: billboard or "Create Gamer Profile" by profile count */
}
static void ui_blade_contents(DWORD scene,const WCHAR *uri,DWORD *app) {
    WCHAR *slash;lstrcpynW(dash_base,uri,4096);
    if(!wcsstr(uri,L"dash_dashmain.xzp") || !(slash=wcsrchr(dash_base,L'/'))) return;
    slash[1]=0;dash_app=app;
    for(int i=0;i<4;i++) {
        const DashBlade *b=&dash_blades[i];
        DWORD tab=ui_find_deep(scene,b->tab,0);
        dash_root[i]=tab;dash_panel[i]=dash_meta[i]=0;dash_shown[i]=-1;
        if(!tab) continue;
        /* The X legend reads "Sign Out" in the scene; nobody is signed in (dashStrings 17/21). */
        dash_set_text(tab,L"legend_x",L"");
        {   /* With no X action the dash disables the legend button, so its icon greys out like Y and B. */
            DWORD x=ui_find_deep(tab,L"legend_x",0);
            if(x) log_value("Legend X disable",xui_bool(x,L"Enabled",FALSE));
        }
        if(b->content) {
            DWORD holder=ui_find_deep(tab,L"scContainer",0),content=0;
            WCHAR file[512];wsprintfW(file,L"%s#%s",b->package,b->content);
            if(holder && (content=dash_scene(file))) {dash_attach(holder,content,0,0);dash_root[i]=content;}
            log_wide(file);log_value("Blade content",content?S_OK:E_FAIL);
            dash_set_text(content,L"labFoundProfiles",L"No Profiles Found");  /* dashStrings 9 */
            dash_set_text(content,L"btn_tray",L"Open Tray");                  /* dashStrings 31 */
        }
        /* MetaContainer is a label placeholder; its billboard goes on the scene at its spot. */
        dash_panel[i]=b->x?dash_root[i]:ui_find_deep(dash_root[i],b->panel,0);
        dash_show_meta(i,0);
    }
    dash_focused=0;
    profiles_dash_ready();  /* auto sign-in, signed-in blades, profile counts, X legend */
}
/* Follow focus inside the blades and swap the meta panel to match the focused item. */
static void ui_dash_focus(void) {
    if(!dash_app || !ui_root) return;
    DWORD focus=XUI_FN(0x7d4a95,DWORD,DWORD)(ui_root);
    if(focus==dash_focused) return;
    dash_focused=focus;
    DWORD *data=focus?XUI_FN(0x7d268f,void *,DWORD,DWORD)(focus,XUI_GLOBAL(0x8f1188,DWORD)):0;
    const WCHAR *id=data?(const WCHAR *)data[1]:0;
    if(!id) return;
    for(int i=0;i<4;i++) for(int j=0;j<DASH_META_MAX && dash_blades[i].buttons[j];j++)
        if(!lstrcmpW(id,dash_blades[i].buttons[j]) && dash_root[i] && ui_find_deep(dash_root[i],id,0)==focus) {
            dash_show_meta(i,j);return;
        }
}
/* Entering a blade focuses its first list item, as the dash does. */
static void ui_dash_enter(int blade) {
    if(blade<1 || blade>4 || !dash_root[blade-1]) return;
    for(int i=0;i<4;i++) if(dash_root[i]) xui_bool(dash_root[i],L"DisableFocusRecursion",i!=blade-1);
    DWORD first=ui_find_deep(dash_root[blade-1],dash_blades[blade-1].buttons[0],0);
    if(first) xui_focus(first);
}

/* Xbox Guide (hud.xex). On the console the Guide is its own XUI module with its own skin
   (huduiskin.xex). Here there is one engine, so the hud skin is loaded under the prefix
   "hud." and every Guide element is rebound to the prefixed copy of the visual it got from
   the dashboard skin. An element's visual is its first child, whose id is the visual name. */
static DWORD guide_scene;
static int guide_rebound;
static const WCHAR *ui_element_id(DWORD e) {
    DWORD *data=e?XUI_FN(0x7d268f,void *,DWORD,DWORD)(e,XUI_GLOBAL(0x8f1188,DWORD)):0;
    return data?(const WCHAR *)data[1]:0;
}
static void guide_rebind(DWORD e,int depth) {
    if(depth>12) return;
    DWORD visual=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(e,&visual);
    /* Only an XuiVisual first child is a visual. A group's first child is a control (radio group ->
       its first radio): setting "hud.<that id>" on the group is accepted by the engine, so the
       first radio was then skipped as if it were the group's visual and kept the dash skin's
       XuiRadioButton (248-wide ellipsis text: "Play Through Head..."). */
    if(visual && !XUI_FN(0x7d876c,DWORD,DWORD,DWORD)(visual,XUI_FN(0x7dec34,DWORD,const WCHAR *)(L"XuiVisual"))) visual=0;
    const WCHAR *name=ui_element_id(visual);
    /* XuiControl data +8 (class handle at 0x8f1194) keeps the authored Visual name, e.g.
       btn_oneline-icon. When that visual exists only in the hud. skin the engine instantiated the
       class default (XuiButton), so rebinding from the instance's id loses the authored layout. */
    DWORD *control=XUI_FN(0x7d268f,void *,DWORD,DWORD)(e,XUI_GLOBAL(0x8f1194,DWORD));
    const WCHAR *authored=control?(const WCHAR *)control[2]:0;
    if(authored && *authored && wcsncmp(authored,L"hud.",4)) {
        WCHAR prefixed[128];wsprintfW(prefixed,L"hud.%s",authored);
        if(SUCCEEDED(xui_string(e,L"Visual",prefixed))) {
            guide_rebound++;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(e,&visual);name=ui_element_id(visual);
        }
    }
    if(name && wcsncmp(name,L"hud.",4)) {
        WCHAR prefixed[128];wsprintfW(prefixed,L"hud.%s",name);
        if(SUCCEEDED(xui_string(e,L"Visual",prefixed))) {guide_rebound++;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(e,&visual);}
        else visual=0;
    } else if(!name || wcsncmp(name,L"hud.",4)) visual=0;
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(e,&child);
    while(child) {if(child!=visual) guide_rebind(child,depth+1);XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
}
/* The Guide's frame is xam.xex's hudbkgnd.xur: HUD_background panel, HUD_blade edge, clock,
   gamertag and the AppHostElementId slot the hud.xex menu scenes go into. Its timelines
   slide it between Closed, Half (the menu) and Full. */
static DWORD guide_menu;
static WCHAR guide_state[8]=L"Closed";
static void guide_play(const WCHAR *to) {
    WCHAR start[32],end[40];wsprintfW(start,L"%sTo%s",guide_state,to);wsprintfW(end,L"End%s",start);
    HRESULT hr=ui_play_tree(guide_scene,start,end,0);
    log_wide(start);log_value("Guide timeline",hr);
    lstrcpyW(guide_state,to);
}
static void guide_clock(void) {
    SYSTEMTIME t;GetLocalTime(&t);WCHAR text[16];
    wsprintfW(text,L"%d:%02d%s",t.wHour%12?t.wHour%12:12,t.wMinute,t.wHour<12?L"am":L"pm");
    dash_set_text(guide_scene,L"DateTimeTextId",text);
}
static DWORD guide_load(const WCHAR *file) {
    DWORD scene=dash_scene(file);
    if(scene && SUCCEEDED(xui_add(ui_root,scene))) {xui_visuals(scene);guide_rebind(scene,0);}
    return scene;
}
/* hud.xex shows the A/B/X/Y legend of the menu on top in the frame's Legend_* controls;
   the menu scenes carry them as hidden btnA..btnY buttons (texts copied from the scenes). */
typedef struct {const WCHAR *scene,*a,*b,*x,*y;} GuideLegend;
static const GuideLegend guide_legends[]={
    {L"sceneMainMenu",L"Select",L"Back",L"Sign in",L"Xbox Dashboard"},
    {L"scnInfoMessage",L"Select",L"Back",L"Sign Out",L"Xbox Dashboard"},
    {L"scnOptions",L"Select",L"Back"},{L"scnOptionsPersonalization",L"Select",L"Back"},
    {L"scnSkinSelect",L"Select",L"Back"},{L"scnTileSelect",L"Select",L"Back"},
    {L"scnColorSelect",L"Select",L"Back"},{L"scnDashStyle",L"Select",L"Back"},
    {L"scnInfoUpsellLive",L"Select",L"Back"},
    {L"scnOptionsOnline",0,L"Back"},{L"scnOptionsVoice",0,L"Back"},
    {L"scnNotificationsOnline",0,L"Back"},{L"scnOptionsController",0,L"Back"}};
static DWORD guide_host,guide_top,guide_seen[64];
static int guide_seen_count;
/* Authored visuals that exist only in the hud skin. When a scene loads, the engine resolves them
   against the dashboard skin and falls back to the class default, so set them explicitly. */
static const WCHAR *guide_hud_visuals[][2]={
    {L"btnNewProfile",L"hud.btn_oneline-icon"},{L"btnRecoverProfile",L"hud.btn_oneline-icon"},
    {L"btnJoinLive",L"hud.btn_oneline-icon"},{L"btnGameBanner",L"hud.btn_gamebanner"},
    {L"paneSongs",L"hud.btn_MetaPane_static"},{L"btnPlay",L"hud.xuiButtonImageCentered"},
    {L"btnPrev",L"hud.xuiButtonImageCenteredMusic"},{L"btnNext",L"hud.xuiButtonImageCenteredMusic"},
    {L"btnPlaybackMode",L"hud.xuiButtonImageCenteredMusic"},{L"btnVolume",L"hud.xuiButtonImageCenteredMusic"},
    {L"sliderVolume",L"hud.Slider_Volume"},{L"txtMessage",L"hud.txt_Starting"},
    {L"bg-Community",L"hud.btn_MetaPane_static"},{L"btnRecentPlayers",L"hud.xuiButtonImageLeftTextRight"},
    {L"btnFriends",L"hud.xuiButtonImageLeftTextRight"},{L"btnMessages",L"hud.xuiButtonImageLeftTextRight"}};
static void guide_prepare(DWORD scene) {
    guide_rebind(scene,0);
    for(int i=0;i<(int)(sizeof guide_hud_visuals/sizeof *guide_hud_visuals);i++) {
        DWORD e=ui_find_deep(scene,guide_hud_visuals[i][0],0);
        /* Only the mini player's sliderVolume uses Slider_Volume; OptionsVoice's has no authored
           Visual, so it takes the class default, which only the hud skin defines (hud.XuiSlider). */
        if(e && !lstrcmpW(guide_hud_visuals[i][0],L"sliderVolume") && !ui_find_deep(scene,L"sceneMiniMediaPlayer",0)) {xui_string(e,L"Visual",L"hud.XuiSlider");continue;}
        if(e) xui_string(e,L"Visual",guide_hud_visuals[i][1]);
    }
    /* InfoUpsellLive's four white body labels. */
    if(ui_find_deep(scene,L"scnInfoUpsellLive",0)) {
        const WCHAR *labels[]={L"XuiControl1",L"XuiControl2",L"XuiControl3",L"XuiControl4"};
        for(int i=0;i<4;i++) {DWORD e=ui_find_deep(scene,labels[i],0);if(e) xui_string(e,L"Visual",L"hud.Label_BodyWhite");}
    }
    /* artPanel stays hidden as authored: the Guide's panel is hudbkgnd's graphic-base (HUD_background). */
}
static const GuideLegend *guide_module_legend(DWORD scene);
static void guide_legend(DWORD scene) {
    const GuideLegend *l=guide_module_legend(scene);
    for(int i=0;i<(int)(sizeof guide_legends/sizeof *guide_legends) && !l;i++) if(ui_find_deep(scene,guide_legends[i].scene,0)) l=&guide_legends[i];
    const WCHAR *ids[4]={L"Legend_A",L"Legend_B",L"Legend_X",L"Legend_Y"},*texts[4]={l?l->a:0,l?l->b:0,l?l->x:0,l?l->y:0};
    for(int i=0;i<4;i++) {
        DWORD e=ui_find_deep(guide_scene,ids[i],0);if(!e) continue;
        xui_bool(e,L"Show",texts[i]!=0);if(texts[i]) xui_string(e,L"Text",texts[i]);
    }
    guide_at_main=l && !lstrcmpW(l->scene,L"sceneMainMenu");
}
/* Scenes the Guide navigates to (a menu's PressPath) appear under the host slot; prepare
   each new one and show the legend of the one on top. */
static void ui_guide_watch(void) {
    if(!guide_host) return;
    DWORD child=0,last=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(guide_host,&child);
    while(child) {
        BOOL seen=FALSE;for(int i=0;i<guide_seen_count;i++) seen|=guide_seen[i]==child;
        if(!seen) {
            guide_prepare(child);
            if(guide_seen_count==64) {memmove(guide_seen,guide_seen+1,63*sizeof *guide_seen);guide_seen_count--;}
            guide_seen[guide_seen_count++]=child;
        }
        last=child;XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);
    }
    if(last!=guide_top) {guide_top=last;if(last) guide_legend(last);}
}
/* Creates the Guide frame once (hud skin, hudbkgnd, main menu). It stays Closed until toggled;
   the message box uses it on its own (ClosedToError) when the Guide is not open. */
static BOOL guide_frame(void) {
    if(!dash_app) return FALSE;
    if(!guide_scene) {
        WCHAR skin[4096];lstrcpyW(skin,dash_base);lstrcatW(skin,L"huduiskin_skin.xzp#skin.xur");
        log_wide(skin);log_value("Guide skin",XUI_FN(0x7d6909,HRESULT,const WCHAR *,const WCHAR *)(skin,L"hud."));
        guide_rebound=0;
        guide_scene=guide_load(L"xam_xam.xzp#hudbkgnd.xur");
        /* Visuals only the hud skin has: the dashboard skin gave these no visual to rebind from. */
        static const WCHAR *named[][2]={{L"graphic-base",L"hud.HUD_background"},{L"graphic-blade",L"hud.HUD_blade"},
            {L"graphic-base-error",L"hud.HUD_background"},{L"graphic-blade-error",L"hud.HUD_blade"},{L"ROL",L"hud.ringOfLight_Group"}};
        for(int i=0;guide_scene && i<5;i++) {DWORD e=ui_find_deep(guide_scene,named[i][0],0);if(e) xui_string(e,L"Visual",named[i][1]);}
        DWORD host=guide_scene?ui_find_deep(guide_scene,L"AppHostElementId",0):0;
        guide_menu=host?dash_scene(profiles_menu_file()):0;
        if(guide_menu) dash_attach(host,guide_menu,0,0);
        if(!guide_menu) return FALSE;
        guide_host=host;ui_guide_watch();
        log_value("Guide visuals rebound",guide_rebound);
        dash_set_text(guide_scene,L"GamerTag",L"");
        guide_open=FALSE;
    }
    return guide_menu!=0;
}
static void ui_guide_toggle(void) {
    if(!guide_frame()) return;
    guide_open=!guide_open;
    if(guide_open) {
        guide_clock();guide_play(L"Half");
        if(guide_top) guide_legend(guide_top);
        DWORD b=ui_find_deep(guide_menu,L"btnSignIn",0);if(b) xui_focus(b);
    } else {guide_play(L"Closed");ui_dash_enter(ui_blade);}
    log_value("Guide open",guide_open);
}
/* Guide services (guide_core.c) used by blades.c. */
static BOOL guide_keep_press(DWORD src);
static BOOL guide_dispatch(DWORD button,const WCHAR *id);
static BOOL guide_back(void);
static void guide_page_shown(DWORD page,int catalog);
static void guide_page_restored(void);
static void guide_tick(void);
static void guide_list_unbind_tree(DWORD root);
static BOOL music_key_y(BOOL down);
#include "blades.c"
#include "guide_core.c"
#include "guide_msgbox.c"
#include "guide_profiles.c"
#include "guide_settings.c"
#include "guide_music.c"
#include "guide_live.c"
#include "guide_sound.c"
static HRESULT ui_load_scene(const WCHAR *path,DWORD *app) {
    WCHAR uri[4096];
    if(FAILED(ui_resource_uri(path,uri))) return E_INVALIDARG;
    DWORD scene=0;
    HRESULT hr=XUI_FN(0x7d5ea8,HRESULT,void *,const WCHAR *,void *,DWORD *)(0,uri,app,&scene);
    log_wide(uri);log_value("External scene load",hr);
    if(FAILED(hr)) return hr;
    hr=xui_add(app[2],scene);
    if(SUCCEEDED(hr)) hr=xui_visuals(scene);
    if(FAILED(hr)) {XUI_FN(0x7dec45,HRESULT,DWORD)(scene);return hr;}
    if(ui_external) {blades_reset();XUI_FN(0x7dec45,HRESULT,DWORD)(ui_external);}
    ui_external=scene;ui_switch(2);
    ui_scene_native=GetEnvironmentVariableA("XUI_NATIVE",0,0)>0;
    {   /* XUI_HIDE=id,id hides elements by id (layout debugging in probe runs). */
        char hide[512];WCHAR id[64];
        if(GetEnvironmentVariableA("XUI_HIDE",hide,sizeof hide)) for(char *t=strtok(hide,",");t;t=strtok(0,",")) {
            MultiByteToWideChar(CP_UTF8,0,t,-1,id,64);DWORD e=ui_find_deep(scene,id,0);
            log_text(t);log_value("Hide",e?xui_bool(e,L"Show",FALSE):E_FAIL);
        }
    }
    ui_blade_contents(scene,uri,app);
    ui_blade_root=ui_find_frames(scene,L"1To2",0);ui_blade=2;
    if(ui_blade_root) ui_dash_enter(2);
    if(ui_blade_root) blades_initialize();
    if(ui_blade_root) log_text("Blades navigation ready (Left/Right or LB/RB)");
    if(requested_play[0]) {
        /* --play=Start[,End]: run a named timeline range, as the dashboard code would. */
        WCHAR start[256],*end;lstrcpynW(start,requested_play,256);
        end=wcschr(start,L',');if(end) *end++=0; else end=start;
        log_wide(requested_play);
        log_value("Scene timeline",ui_play_tree(scene,start,end,0));
    }
    return S_OK;
}
static WORD ui_pad_key(Uint8 button) {
    switch(button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:return VK_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:return VK_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:return VK_LEFT;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:return VK_PRIOR;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:return VK_NEXT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:return VK_RIGHT;
        case SDL_GAMEPAD_BUTTON_SOUTH:return VK_RETURN;
        case SDL_GAMEPAD_BUTTON_EAST:return VK_BACK;
        case SDL_GAMEPAD_BUTTON_WEST:return 0x5802;
        case SDL_GAMEPAD_BUTTON_NORTH:return 0x5803;
        default:return 0;
    }
}
static void ui_dump(DWORD element,int depth) {
    if(depth>6) return;
    DWORD *data=XUI_FN(0x7d268f,void *,DWORD,DWORD)(element,XUI_GLOBAL(0x8f1188,DWORD));
    if(data) {
        char line[256];wsprintfA(line,"Element %08X depth %d flags %08X child %08X alpha %08X size %08X %08X",element,depth,data[60],data[6],data[5],data[3],data[4]);log_text(line);
        log_wide((WCHAR *)data[1]);
    }
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(element,&child);
    while(child){ui_dump(child,depth+1);XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
}
static HRESULT __fastcall ui_background_message(DWORD *self,void *unused,DWORD *message) {
    if(message[1]==0x13) {
        HRESULT hr=XUI_THIS(0x49ad31,HRESULT,void *)(self);
        if(FAILED(hr) && !xui_find(self[1],L"CloseX")) {
            /* The Xbox 360 Guide frame (xam hudbkgnd.xur) is the same HUDBkgndScene class but
               lacks the GFWL close button and evaluation text; give it hidden stand-ins. */
            static const WCHAR *extra[2][2]={{L"CloseX",L"XuiElement"},{L"EvaluationTextId",L"XuiText"}};
            for(int i=0;i<2;i++) {
                XuiElement e=0;
                if(SUCCEEDED(xui_create(extra[i][1],&e)) && SUCCEEDED(xui_string(e,L"Id",extra[i][0]))) {xui_bool(e,L"Show",FALSE);xui_add(self[1],e);}
            }
            hr=XUI_THIS(0x49ad31,HRESULT,void *)(self);
        }
        log_value("Background controls",hr);
        if(FAILED(hr)) return hr;
        self[2]=*(DWORD *)message[4];
        XUI_GLOBAL(0x8ab2a0,void *)=self;
        message[2]=1;
        return S_OK;
    }
    if(message[1]==0x27) return S_OK; /* No Xbox service notification polling. */
    return XUI_THIS(0x49bc1a,HRESULT,void *,void *)(self,message);
}
/* --dump-classes: log each built-in class's property definitions, base first, for the XUIB converter. */
static void ui_dump_defs(const BYTE *defs,DWORD count,int depth) {
    for(DWORD i=0;i<count;i++) {
        const BYTE *d=defs+i*0x34;char line[160],name[96]="";
        WideCharToMultiByte(CP_UTF8,0,*(const WCHAR **)(d+0x10),-1,name,sizeof(name),0,0);
        wsprintfA(line,"%sprop %lu %s type=%lu flags=0x%lx offset=0x%lx get=0x%lx set=0x%lx",depth?"    ":"  ",i,name,*(DWORD *)(d+0x14),*(DWORD *)d,*(DWORD *)(d+8),*(DWORD *)(d+0x20),*(DWORD *)(d+0x24));log_text(line);
        if(*(DWORD *)(d+0x14)==9 && *(void **)(d+0x28) && depth<2) {
            DWORD sub=0;const BYTE *subdefs=((const BYTE *(WINAPI *)(DWORD *))*(void **)(d+0x28))(&sub);
            if(subdefs) ui_dump_defs(subdefs,sub,depth+1);
        }
    }
}
static void ui_dump_classes(void) {
    static const WCHAR *names[]={L"XuiElement",L"XuiVisual",L"XuiControl",L"XuiCanvas",L"XuiScene",L"XuiTabScene",L"XuiGroup",
        L"XuiFigure",L"XuiImage",L"XuiImagePresenter",L"XuiText",L"XuiTextPresenter",L"XuiLabel",L"XuiButton",L"XuiNavButton",
        L"XuiBackButton",L"XuiCheckbox",L"XuiRadioButton",L"XuiRadioGroup",L"XuiList",L"XuiCommonList",L"XuiListItem",L"XuiEdit",
        L"XuiCaret",L"XuiSlider",L"XuiProgressBar",L"XuiScrollEnd",L"XuiNineGrid",L"XuiSoundXAudio",L"XuiSoundXAct",L"XuiSound",
        L"XuiGamerCard",L"XuiVideo",L"XuiHtmlPresenter",L"XuiHtmlControl",L"XuiMessageBox",L"XuiPassCodeControl",L"XuiSpinner",0};
    for(int n=0;names[n];n++) {
        BYTE *cls=XUI_FN(0x7dec34,BYTE *,const WCHAR *)(names[n]);
        char line[160],name[96];WideCharToMultiByte(CP_UTF8,0,names[n],-1,name,sizeof(name),0,0);
        if(!cls) {wsprintfA(line,"class %s missing",name);log_text(line);continue;}
        for(int level=0;cls && level<12;level++) {
            char own[96]="?";const WCHAR *cn=*(const WCHAR **)(cls+4);
            if(cn && !IsBadStringPtrW(cn,96)) WideCharToMultiByte(CP_UTF8,0,cn,-1,own,sizeof(own),0,0);
            wsprintfA(line,"class %s level %d %s count=%lu",name,level,own,*(DWORD *)(cls+0x20));log_text(line);
            if(*(BYTE **)(cls+0x1c)) ui_dump_defs(*(BYTE **)(cls+0x1c),*(DWORD *)(cls+0x20),0);
            cls=*(BYTE **)(cls+0x28);
        }
    }
}
static HRESULT independent_ui(SDL_Window *window, IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pp) {
    HRESULT hr;
    DWORD init[3]={12,2,0};
    UI_CHECK("XuiInit", XUI_FN(0x7e1b8b,HRESULT,void *)(init));
    UI_CHECK("Xui input classes", XUI_C(0x7e91b3,HRESULT,void)());
    DWORD ri[15]={60};
    ri[2]=(DWORD)device; ri[4]=(DWORD)self_image+0x40e45b; ri[5]=(DWORD)pp;
    UI_CHECK("XuiRenderInit", XUI_FN(0x80989d,HRESULT,void *)(ri));
    void *dc=0;
    UI_CHECK("XuiRenderCreateDC", XUI_FN(0x80863b,HRESULT,void **)(&dc));
    UI_CHECK("Xui sound classes", XUI_FN(0x7e40f0,HRESULT,void *,DWORD)(0,0));
    sound_init();
    UI_CHECK("Xui resource provider", XUI_C(0x49c4f9,HRESULT,void)());
    for(int i=0;i<6;i++) ui_callbacks[i]=(void *)((BYTE *)self_image+((DWORD *)((BYTE *)self_image+0x1ea8))[i]-0x400000);
    ui_callbacks[1]=ui_missing_class; ui_callbacks[3]=ui_resource_error;
    XUI_GLOBAL(0x8aa3b4,HMODULE)=self_image;
    XUI_FN(0x7d8bd6,HRESULT,void *)(&ui_callback_object);
    UI_CHECK("Guide base classes", XUI_C(0x49c16f,HRESULT,void)());
    if(dump_classes) {ui_dump_classes();return S_OK;}
    adapt_import(0x36a4,ui_background_message);
    XUI_FN(0x7d8879,void,const WCHAR *,BOOL)(0,TRUE);
    WCHAR font_uri[MAX_PATH+64]=L"file://",font_path[MAX_PATH];
    DWORD font_path_length=GetModuleFileNameW(0,font_path,MAX_PATH);
    if(!font_path_length || font_path_length>=MAX_PATH) return E_FAIL;
    WCHAR *font_filename=wcsrchr(font_path,L'\\');
    if(!font_filename || font_filename-font_path+sizeof(L"\\assets\\fonts\\Convection-Medium.ttf")/sizeof(WCHAR)>MAX_PATH) return E_FAIL;
    lstrcpyW(font_filename,L"\\assets\\fonts\\Convection-Medium.ttf");
    if(GetFileAttributesW(font_path)==INVALID_FILE_ATTRIBUTES) {log_text("Convection font asset is missing");return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);}
    lstrcatW(font_uri,font_path);
    struct {const WCHAR *name,*uri; DWORD flags; float scale; void *fallback;} font={L"SegoeUIorArial",font_uri,0,0,0};
    /* Original Guide scenes request this legacy alias; map it to Convection too. */
    UI_CHECK("Xui Guide font alias", XUI_FN(0x7fbef8,HRESULT,void *,BOOL)(&font,FALSE));
    font.name=L"Convection";
    UI_CHECK("Xui Convection font", XUI_FN(0x7fbef8,HRESULT,void *,BOOL)(&font,TRUE));
    /* Xbox 360 dashboard skins ask for ConvectionUI. */
    font.name=L"ConvectionUI";
    UI_CHECK("Xui ConvectionUI font", XUI_FN(0x7fbef8,HRESULT,void *,BOOL)(&font,FALSE));
    WCHAR uri[4096];
    if(requested_skin[0]) {
        if(FAILED(ui_resource_uri(requested_skin,uri))) return E_INVALIDARG;
        log_wide(uri);
    } else wsprintfW(uri,L"panres://%X,skin#skin.xur",(DWORD)self_image);
    UI_CHECK("Xui skin", XUI_FN(0x7d6909,HRESULT,const WCHAR *,void *)(uri,0));
    lstrcpynW(ui_skin_uri,uri,4096);
    DWORD app[22]={0};
    XUI_THIS(0x49aeec,void *,void *)(app);
    XUI_THIS(0x49a712,void,void *,void *)(app,dc);
    XUI_GLOBAL(0x8aa2dc,void *)=app;
    app[20]=(DWORD)self_image;
    log_value("Xui root canvas",app[2]);
    DWORD background=0;
    wsprintfW(uri,L"panres://%X,xam#hudbkgnd.xur",(DWORD)self_image);
    UI_CHECK("Guide background scene", XUI_FN(0x7d5ea8,HRESULT,void *,const WCHAR *,void *,DWORD *)(0,uri,app,&background));
    UI_CHECK("Guide background attach", XUI_FN(0x7d3261,HRESULT,DWORD,DWORD)(app[2],background));
    UI_CHECK("Guide background data", XUI_FN(0x7d8213,HRESULT,DWORD,DWORD *)(background,&app[16]));
    UI_CHECK("Guide host element search", XUI_FN(0x7d3605,HRESULT,DWORD,const WCHAR *,DWORD *)(background,L"AppHostElementId",&app[18]));
    log_value("Guide background",app[16]);
    wsprintfW(uri,L"panres://%X,HUD#MainMenuSignedOut.xur",(DWORD)self_image);
    void *scene=0;
    UI_CHECK("Xui scene", XUI_FN(0x7d5ea8,HRESULT,void *,const WCHAR *,void *,void **)(0,uri,app,&scene));
    xui_string(xui_find((DWORD)scene,L"labelProfiles"),L"Text",L"Offline UI runtime");
    log_value("Xui scene handle",(DWORD)scene);
    UI_CHECK("Guide scene attach", XUI_FN(0x7d3261,HRESULT,DWORD,DWORD)(app[18],(DWORD)scene));
    DWORD visual_message[5]={20,0x29,0,0,0};
    UI_CHECK("Xui update visuals",XUI_FN(0x7decdf,HRESULT,DWORD,void *)(app[2],visual_message));
    ((DWORD *)app[16])[3]=0;
    XUI_THIS(0x49a875,void,void *,DWORD)((void *)app[16],1);
    log_value("Background open state",((DWORD *)app[16])[3]);
    ui_scene=(DWORD)scene;ui_background=background;
    UI_CHECK("Dashboard scene",dashboard_create(app[2],&ui_demo));
    UI_CHECK("Dashboard visuals",xui_visuals(ui_demo));
    xui_bool(ui_demo,L"Show",FALSE);
    XUI_GLOBAL(0x8f141c,void *)=ui_message_filter;
    xui_text(xui_find(background,L"GamerTag"),L"Standalone");
    SYSTEMTIME now;GetLocalTime(&now);WCHAR time_text[64];GetTimeFormatW(LOCALE_USER_DEFAULT,TIME_NOSECONDS,&now,0,time_text,64);
    xui_text(xui_find(background,L"DateTimeTextId"),time_text);
    ui_root=app[2];
    XUI_FN(0x7e2089,void,HWND)(pp->hDeviceWindow);
    XUI_FN(0x7e222b,HRESULT,DWORD,HWND)(ui_root,pp->hDeviceWindow);
    SDL_SetWindowsMessageHook(ui_window_message,0);
    SDL_Gamepad **pads=ui_pads;
    int pad_count=0;SDL_JoystickID *pad_ids=SDL_GetGamepads(&pad_count);
    for(int i=0;i<pad_count && i<4;i++) pads[i]=SDL_OpenGamepad(pad_ids[i]);
    SDL_free(pad_ids);
    SDL_ShowWindow(window);
    BOOL running=TRUE, probe=probe_mode;
    /* XUI_END=frame lengthens probe runs for long key scripts; the last capture is 5 frames before. */
    DWORD probe_end=getenv("XUI_END")?(DWORD)atoi(getenv("XUI_END")):180;
    HRESULT outcome=S_OK;
    Uint64 last_tick=SDL_GetTicks();
    Uint64 clock_tick=last_tick;
    int stick_direction=0;
    for(DWORD frame=0;running;frame++) {
        SDL_Event event;
        if(profiles_wants_text()!=SDL_TextInputActive(window)) {if(profiles_wants_text()) SDL_StartTextInput(window); else SDL_StopTextInput(window);}
        while(SDL_PollEvent(&event)) {
            if(event.type==SDL_EVENT_TEXT_INPUT) profiles_text(event.text.text);
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.key==SDLK_F8) ui_dump_classes();
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.key==SDLK_F1) ui_switch(0);
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.key==SDLK_F2) ui_switch(1);
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.key==SDLK_F3 && ui_external) ui_switch(2);
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.key==SDLK_F11) SDL_SetWindowFullscreen(window,!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN));
            if(event.type==SDL_EVENT_GAMEPAD_ADDED) {
                BOOL present=FALSE;for(int i=0;i<4;i++) if(pads[i] && SDL_GetGamepadID(pads[i])==event.gdevice.which) present=TRUE;
                if(!present) for(int i=0;i<4;i++) if(!pads[i]) {pads[i]=SDL_OpenGamepad(event.gdevice.which);break;}
            }
            if(event.type==SDL_EVENT_GAMEPAD_REMOVED) for(int i=0;i<4;i++) if(pads[i] && SDL_GetGamepadID(pads[i])==event.gdevice.which) {SDL_CloseGamepad(pads[i]);pads[i]=0;}
            if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type==SDL_EVENT_GAMEPAD_BUTTON_UP) {
                WORD key=ui_pad_key(event.gbutton.button);if(key) ui_key(key,event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && blades_ready) {
                    if(event.gbutton.button==SDL_GAMEPAD_BUTTON_GUIDE || event.gbutton.button==SDL_GAMEPAD_BUTTON_BACK || event.gbutton.button==SDL_GAMEPAD_BUTTON_START) ui_switch(0);
                } else if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && dash_app) {
                    /* Blades dashboard: the Xbox button opens the Guide, B closes it. */
                    if(event.gbutton.button==SDL_GAMEPAD_BUTTON_GUIDE || event.gbutton.button==SDL_GAMEPAD_BUTTON_BACK) ui_guide_toggle();
                    else if(event.gbutton.button==SDL_GAMEPAD_BUTTON_EAST) {ui_key(VK_BACK,TRUE);ui_key(VK_BACK,FALSE);}
                    else if(event.gbutton.button==SDL_GAMEPAD_BUTTON_NORTH) {ui_key('Y',TRUE);ui_key('Y',FALSE);}
                } else {
                    if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button==SDL_GAMEPAD_BUTTON_EAST) ui_switch(0);
                    if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button==SDL_GAMEPAD_BUTTON_START) ui_switch(ui_mode?0:1);
                }
            }
            if(event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gaxis.axis==SDL_GAMEPAD_AXIS_LEFTY) {
                int direction=event.gaxis.value>16000?1:event.gaxis.value < -16000?-1:0;
                if(direction && direction!=stick_direction) {WORD key=direction>0?VK_DOWN:VK_UP;ui_key(key,TRUE);ui_key(key,FALSE);}
                stick_direction=direction;
            }
            if(event.type==SDL_EVENT_DROP_FILE) {
                WCHAR file[2048];if(MultiByteToWideChar(CP_UTF8,0,event.drop.data,-1,file,2048)) {
                    HRESULT loaded=ui_load_scene(file,app);
                    if(FAILED(loaded)) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Scene could not load","Use a compatible XUI version 5 .xur file. Details are in guide.log.",window);
                }
            }
            if(event.type==SDL_EVENT_QUIT) running=FALSE;
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.key==SDLK_ESCAPE) {
                if(blades_ready && (blades_guide_open || blades_in_submenu() || blades_notice_root)) blades_pending_back=1;
                else running=FALSE;
            }
        }
        if(probe && frame>=50 && frame%20==10) {
            /* XUI_KEYS=R,L,... presses Right/Left in probe runs, so navigation can be tested headless. */
            static char keys[1024];static int key_at=-1;
            if(key_at<0) {key_at=0;if(!GetEnvironmentVariableA("XUI_KEYS",keys,sizeof keys)) keys[0]=0;}
            while(keys[key_at]==',') key_at++;
            if(keys[key_at]=='S') {ui_shot_request=TRUE;key_at++;}
            else if(keys[key_at]=='W') key_at++;
            else if(keys[key_at]=='K') {
                /* K types the next '|'-separated part of XUI_TYPE as PC key presses ('<' Backspace, '>' Enter). */
                static char typed[256];static char *next=0;
                if(!next) {if(!GetEnvironmentVariableA("XUI_TYPE",typed,sizeof typed)) typed[0]=0;next=typed;}
                for(;*next && *next!='|';next++) {
                    char c=*next;WORD vk=c=='<'?VK_BACK:c=='>'?VK_RETURN:(WORD)(VkKeyScanA(c)&0xff);
                    PostMessageA(pp->hDeviceWindow,WM_KEYDOWN,vk,1);PostMessageA(pp->hDeviceWindow,WM_KEYUP,vk,0xC0000001);
                }
                if(*next) next++;
                key_at++;
            }
            else if(keys[key_at]=='G') {ui_switch(0);key_at++;}
            else if(keys[key_at]=='F') {ui_switch(1);key_at++;}
            else if(keys[key_at]=='T') {ui_switch(2);key_at++;}
            else if(keys[key_at]) {WORD key=keys[key_at]=='L'?VK_LEFT:keys[key_at]=='R'?VK_RIGHT:keys[key_at]=='D'?VK_DOWN:keys[key_at]=='U'?VK_UP:keys[key_at]=='B'?VK_BACK:keys[key_at]=='Y'?'Y':keys[key_at]=='X'?0x5802:keys[key_at]=='E'?VK_ESCAPE:VK_RETURN;ui_key(key,TRUE);ui_key(key,FALSE);key_at++;}
        }
        if(frame==45) {
            DWORD button=0;XUI_FN(0x7d3605,HRESULT,DWORD,const WCHAR *,DWORD *)((DWORD)scene,L"btnSignIn",&button);
            log_value("Initial focus",XUI_FN(0x7d43ef,HRESULT,DWORD,DWORD)(button,0));
            if(dashboard_mode) ui_switch(1);
            if(requested_scene[0]) {hr=ui_load_scene(requested_scene,app);if(FAILED(hr)) {outcome=hr;break;}}
            if(xbox_guide_mode && blades_ready) ui_switch(0);
        }
        if(probe && !audit_scenes && frame==65 && !getenv("XUI_KEYS")) {ui_key(VK_DOWN,TRUE);ui_key(VK_DOWN,FALSE);log_value("Focus after Down",XUI_FN(0x7d4a95,DWORD,DWORD)(ui_root));}
        if(probe && !audit_scenes && frame==100 && !getenv("XUI_KEYS")) {ui_key(VK_RETURN,TRUE);ui_key(VK_RETURN,FALSE);}
        if(ui_pending>=10) {
            const WCHAR *statuses[3]={L"Games selected.",L"Media selected.",L"Settings selected."};
            xui_text(dashboard_status,statuses[ui_pending-10]);ui_pending=0;
        } else if(ui_pending) {
            /* This is a UI runtime; no identity service is started or impersonated. */
            if(!probe) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,"Offline Guide",
                "The original Guide is running independently. Xbox sign-in and profile services are not connected.\n\nPress F2 to open the editable XUI dashboard.",window);
            ui_pending=0;
        }
        if(ui_quit) running=FALSE;
        blades_tick();
        blades_audit_tick(frame);
        HRESULT cooperative=IDirect3DDevice9_TestCooperativeLevel(device);
        if(cooperative==D3DERR_DEVICELOST) {SDL_Delay(50);last_tick=SDL_GetTicks();continue;}
        if(cooperative==D3DERR_DEVICENOTRESET || (probe && frame==110)) {
            XUI_FN(0x7d3910,HRESULT,DWORD,DWORD)(app[2],0xffffffff);
            XUI_FN(0x808693,HRESULT,void *)(dc);dc=0;
            XUI_C(0x808555,HRESULT,void)();
            if(ui_zoom_surface) {IDirect3DSurface9_Release(ui_zoom_surface);ui_zoom_surface=0;}
            hr=IDirect3DDevice9_Reset(device,pp);
            if(SUCCEEDED(hr)) hr=XUI_FN(0x80989d,HRESULT,void *)(ri);
            if(SUCCEEDED(hr)) hr=XUI_FN(0x80863b,HRESULT,void **)(&dc);
            app[3]=(DWORD)dc;
            log_value("Graphics reset",hr);
            if(FAILED(hr)) {outcome=hr;break;}
            xui_visuals(app[2]);
        }
        Uint64 tick=SDL_GetTicks();float elapsed=(float)(tick-last_tick);last_tick=tick;
        if(tick-clock_tick>=1000) {
            GetLocalTime(&now);GetTimeFormatW(LOCALE_USER_DEFAULT,TIME_NOSECONDS,&now,0,time_text,64);
            xui_text(xui_find(background,L"DateTimeTextId"),time_text);clock_tick=tick;
        }
        if(elapsed>100) elapsed=100;
        if(probe) elapsed=1000.0f/60.0f;
        sound_timers();  /* XuiSound play timers (guide_sound.c) */
        XUI_FN(0x7e33de,void,float)(elapsed);
        blades_visibility();
        HRESULT prepared=XUI_THIS(0x48cd65,HRESULT,void *)(app);
        if(frame==0) log_value("Xui prerender",prepared);
        IDirect3DDevice9_Clear(device,0,0,D3DCLEAR_TARGET,0xff181b20,1,0);
        ui_dash_focus();ui_guide_watch();
        IDirect3DDevice9_BeginScene(device);
        float transform[16];
        XUI_FN(0x486da9,HRESULT,void *,void *)(dc,transform);
        if(ui_mode) { transform[12]=0; transform[13]=0; }
        ui_dash_zoom=FALSE;
        if(ui_mode==2 && ui_external) {
            DWORD *data=XUI_FN(0x7d268f,void *,DWORD,DWORD)(ui_external,XUI_GLOBAL(0x8f1188,DWORD));
            float width=data?*(float *)&data[3]:1280,height=data?*(float *)&data[4]:720;
            /* dash.xex 2.0.1888 sets the view transform at 0x920a41e0: on a 960-wide (4:3)
               buffer it scales 960/1120 x 720/770, otherwise W/1120 x (H/720)*12/11 and moves
               up 64*(H/720), i.e. it shows the scene window (0,58.7)-(1120,718.7) full screen.
               Scaling through the GFWL engine clips the scene's left edge, so render 1:1 and
               stretch that window onto the back buffer after the frame (see ui_dash_zoom). */
            float scale=1,scale_y=0,x=0,y=0;
            ui_dash_zoom=width==1120 && height==770 && !ui_scene_native;
            if(ui_dash_zoom) scale_y=1;
            else if(height>720 && !ui_scene_native) {scale=720/height;x=(1280-width*scale)/2;} /* other tall scenes: fit and centre */
            transform[0]=scale;transform[5]=scale_y?scale_y:scale;transform[12]=x;transform[13]=y;
            ui_offset_x=ui_dash_zoom?0:x;ui_offset_y=ui_dash_zoom?-64:y;ui_scale=ui_dash_zoom?1280.0f/1120:scale;ui_scale_y=ui_dash_zoom?12.0f/11:scale;
        }
        XUI_FN(0x808731,HRESULT,void *,void *)(dc,transform);
        hr=XUI_FN(0x8085b0,HRESULT,void *,DWORD)(dc,0xff181b20);
        DWORD render_data[5]={(DWORD)dc,0xffffffff,1,0,0};
        DWORD render_message[5]={20,0,0,20,(DWORD)render_data};
        HRESULT rendered=XUI_FN(0x7dbd06,HRESULT,DWORD,void *)(app[2],render_message);
        XUI_FN(0x8085cd,HRESULT,void *)(dc);
        IDirect3DDevice9_EndScene(device);
        if(ui_dash_zoom) {
            IDirect3DSurface9 *rt=0;
            if(SUCCEEDED(IDirect3DDevice9_GetRenderTarget(device,0,&rt))) {
                D3DSURFACE_DESC desc;IDirect3DSurface9_GetDesc(rt,&desc);
                if(!ui_zoom_surface) IDirect3DDevice9_CreateRenderTarget(device,desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&ui_zoom_surface,0);
                RECT window={0,(LONG)(desc.Height*58.67f/720),(LONG)(desc.Width*1120.0f/1280),(LONG)(desc.Height*718.67f/720)};
                HRESULT first=ui_zoom_surface?IDirect3DDevice9_StretchRect(device,rt,&window,ui_zoom_surface,0,D3DTEXF_LINEAR):E_OUTOFMEMORY;
                HRESULT second=SUCCEEDED(first)?IDirect3DDevice9_StretchRect(device,ui_zoom_surface,0,rt,0,D3DTEXF_POINT):first;
                if(ui_zoom_frames++==0) {
                    char line[160];RECT client={0};GetClientRect(GetActiveWindow(),&client);
                    wsprintfA(line,"Dash zoom: target %ux%u format %u multisample %u, window %d,%d-%d,%d, client %dx%d",desc.Width,desc.Height,desc.Format,desc.MultiSampleType,window.left,window.top,window.right,window.bottom,client.right,client.bottom);
                    log_text(line);log_value("Dash zoom copy",first);log_value("Dash zoom present",second);
                }
                IDirect3DSurface9_Release(rt);
            }
        }
        if(frame==0) { log_value("First XUI begin",hr); log_value("First XUI render",rendered); }
        BOOL shot_now=ui_shot_request;ui_shot_request=FALSE;
        if(shot_now || probe && (frame==90 || frame==150 || frame==probe_end-5) || ui_dash_zoom && ui_zoom_frames==120) {
            if(strstr(GetCommandLineA(),"--dump-tree")) ui_dump(app[2],0);
            IDirect3DSurface9 *rt=0,*copy=0;
            if(SUCCEEDED(IDirect3DDevice9_GetRenderTarget(device,0,&rt))) {
                D3DSURFACE_DESC desc; IDirect3DSurface9_GetDesc(rt,&desc);
                if(SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(device,desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&copy,0))) {
                    HRESULT saved=IDirect3DDevice9_GetRenderTargetData(device,rt,copy);
                    if(SUCCEEDED(saved)) {
                        HMODULE dx=GetModuleHandleA("d3dx9_33.dll");
                        HRESULT (WINAPI *save)(LPCWSTR,int,IDirect3DSurface9 *,void *,void *)=(void *)GetProcAddress(dx,"D3DXSaveSurfaceToFileW");
                        WCHAR path[MAX_PATH];GetModuleFileNameW(0,path,MAX_PATH);WCHAR *slash=wcsrchr(path,L'\\');lstrcpyW(slash+1,!probe?L"dash-capture.png":ui_mode==1?L"dashboard-preview.png":ui_mode==2?L"scene-preview.png":L"guide-preview.png");
                        if(shot_now) wsprintfW(slash+1,L"shot-%02d.png",++ui_shot_count);
                        saved=save(path,3,copy,0,0);
                    }
                    log_value("Preview capture",saved); IDirect3DSurface9_Release(copy);
                }
                IDirect3DSurface9_Release(rt);
            }
        }
        IDirect3DDevice9_Present(device,0,0,0,0);
        if(probe && ((!audit_scenes && frame>=probe_end) || blades_audit_done)) running=FALSE;
        SDL_Delay(1);
    }
    SDL_SetWindowsMessageHook(0,0);ui_root=0;
    for(int i=0;i<4;i++) if(pads[i]) {SDL_CloseGamepad(pads[i]);pads[i]=0;}
    XUI_GLOBAL(0x8f141c,void *)=0;
    log_value("Xui scene cleanup",XUI_FN(0x7dec45,HRESULT,DWORD)(app[2]));
    if(dc) log_value("Xui context cleanup",XUI_FN(0x808693,HRESULT,void *)(dc));
    log_value("Xui engine cleanup",XUI_C(0x7e15d5,HRESULT,void)());
    log_value("Xui renderer cleanup",XUI_C(0x808555,HRESULT,void)());
    return outcome;
}
