/* Guide module: live.
 * The Xbox-Live-only entries end in the console's own offline behaviour (research/signedin.md §2-3):
 * no network service exists for this host, so every attempt shows the console's connecting screen and
 * then its own can't-connect message. Nothing pretends to succeed.
 * Also: console message boxes (guide_msgbox.c), the xam notification popup (guide_toast) and the hud
 * Status page (guide_status_show/hide).
 */

/* Console strings (string table index in /root/research/dumps/all_root_xus.txt). */
#define S_RECOVER_TITLE  L"Recover Gamertag"   /* signin_signin strings.xus[40] */
#define S_RECOVER_BODY   L"To recover your profile from Xbox Live, you must exit your current session. If you are playing a game, you will lose any unsaved progress. Are you sure you want to exit?"   /* [41] */
#define S_YES            L"Yes"                /* signin [1] */
#define S_NO_CANCEL      L"No, cancel"         /* signin [38] */
#define S_BTN_OK             L"OK"                 /* signin [39] */
#define S_CHECKING       L"Checking network status"   /* createprofile_cp CreateProfile_Custom.xus[0] */
#define S_CANT_CONNECT_T L"Can't Connect"      /* createprofile_cp [17] */
#define S_CANT_CONNECT   L"Can't connect to Xbox Live. For more info, go to www.xbox.com/support."   /* createprofile_cp [16] */
#define S_JOIN_TITLE     L"Join Xbox Live"     /* dash_signupbo SignupBootstrap_Custom.xus[4] */
#define S_JOIN_CONNECT   L"Connecting to Xbox Live"   /* dash_signupbo [2] */
#define S_JOIN_FAIL      L"Can't connect to Xbox Live."   /* dash_signupbo [1] */
#define S_JOIN_OK        L"OK"                 /* dash_signupbo [6] */
#define S_MARKET_TITLE   L"Xbox Live Marketplace"   /* marketplace_media Strings.xus[200] */
#define S_MARKET_SIGNIN  L"Please sign in to Xbox Live to use Xbox Live Marketplace."   /* [195] */
#define S_MARKET_OK      L"OK"                 /* marketplace_media [207] */
#define LIVE_CONNECT_FRAMES 120                /* ~2 s of connecting/checking before the attempt fails */

static BOOL msgbox_filter(DWORD element,DWORD *message);
static BOOL msgbox_back(void);
static void msgbox_tick(void);

/* ---------- connecting screens (AddXboxLive / DownloadLiveSignup) ---------- */
enum {LIVE_NONE,LIVE_RECOVER,LIVE_JOIN};
static struct {int kind;DWORD page;int frames;} live_flow;
static BOOL live_page_on_top(DWORD page) {BladesPage *top=blades_page();return page && top && top->scene==page;}
static void live_flow_pop(int choice,void *ctx) {
    DWORD page=(DWORD)ctx;
    if(live_page_on_top(page)) blades_back();
}
static void live_flow_start(int kind,const WCHAR *package,const WCHAR *scene,const WCHAR *status) {
    int index=blades_catalog(package,scene);
    if(index<0 || !blades_navigate(index)) {log_text("Live flow scene missing");log_wide(scene);return;}
    BladesPage *top=blades_page();
    live_flow.kind=kind;live_flow.page=top?top->scene:0;live_flow.frames=LIVE_CONNECT_FRAMES;
    guide_text(live_flow.page,L"statusText",status);
    /* Loading_Large spins on its own timelines once the visual plays. */
    DWORD spin=guide_find(live_flow.page,L"waitCursorControl");
    if(spin) ui_play_tree(spin,L"Normal",L"EndNormal",0);
    log_text("Live flow started (no Xbox Live service: the attempt will fail)");log_wide(status);
}
static void live_flow_tick(void) {
    if(!live_flow.kind) return;
    /* B on the connecting screen (its hidden back button) cancels the attempt. */
    if(!live_page_on_top(live_flow.page)) {live_flow.kind=LIVE_NONE;return;}
    if(guide_msgbox_open() || --live_flow.frames>0) return;
    int kind=live_flow.kind;DWORD page=live_flow.page;
    live_flow.kind=LIVE_NONE;
    log_text("Xbox Live not reachable");
    if(kind==LIVE_RECOVER) {const WCHAR *b[]={S_BTN_OK};guide_msgbox(S_CANT_CONNECT_T,S_CANT_CONNECT,b,1,0,live_flow_pop,(void *)page);}
    else {const WCHAR *b[]={S_JOIN_OK};guide_msgbox(S_JOIN_TITLE,S_JOIN_FAIL,b,1,0,live_flow_pop,(void *)page);}
}

/* ---------- Recover Gamertag / Join Xbox Live ---------- */
static void live_recover_check(void) {
    live_flow_start(LIVE_RECOVER,L"createprofile_cp.xzp",L"AddXboxLive.xur",S_CHECKING);
}
static void live_recover_confirmed(int choice,void *ctx) {
    if(choice!=0) return;   /* No, cancel / B: back to the Guide menu */
    /* "You must exit your current session": leave the Guide for the dashboard's recovery flow. */
    if(blades_guide_open) {
        while(blades_guide_stack.depth) blades_guide_pop();
        blades_guide(FALSE);
    }
    live_recover_check();
}
static void live_recover(void) {
    /* The exit-session confirmation belongs to the Guide (signin.xex over a running session);
       the dashboard's own Live blade entry goes straight to the recovery attempt. */
    if(blades_guide_open) {
        const WCHAR *b[]={S_YES,S_NO_CANCEL};
        guide_msgbox(S_RECOVER_TITLE,S_RECOVER_BODY,b,2,0,live_recover_confirmed,0);
    } else live_recover_check();
}
static void live_join(void) {
    live_flow_start(LIVE_JOIN,L"dash_signupbo.xzp",L"DownloadLiveSignup.xur",S_JOIN_CONNECT);
}
/* Connect to Xbox Live with a local (non-Live) profile: xam's Live upsell, InfoUpsellLive (Full Guide).
   From the dashboard (Live blade btnConnect) it opens over the dash like the console's
   XamShowLiveUpsellUI, and B there returns to the dashboard. */
static DWORD live_upsell_page;
static BOOL live_upsell_from_dash;
static void live_upsell(void) {
    BOOL from_dash=!blades_guide_open;
    if(from_dash) {blades_guide(TRUE);if(!blades_guide_open) return;}
    if(!blades_navigate(blades_catalog(L"hud_hud.xzp",L"InfoUpsellLive.xur"))) {if(from_dash) blades_guide(FALSE);return;}
    BladesPage *top=blades_page();
    live_upsell_page=top?top->scene:0;live_upsell_from_dash=from_dash;
}

/* ---------- hud Status.xur ("Signing out") ---------- */
static DWORD live_status_page;
static BOOL live_status_closing;
static void guide_status_show(const WCHAR *text) {
    if(!blades_guide_open) return;
    if(!live_page_on_top(live_status_page)) {
        int index=blades_catalog(L"hud_hud.xzp",L"Status.xur");
        if(index<0 || !blades_navigate(index)) return;
        BladesPage *top=blades_page();live_status_page=top?top->scene:0;
    }
    DWORD msg=guide_find(live_status_page,L"txtMessage");
    if(msg) {
        /* txt_Starting's text presenter has no TextColor: on the hud skin it would draw in the
           presenter default; the console status text is the Guide's white body text. The
           visual's own Normal timeline runs the Loading_Inline spinner under the text. */
        xui_string(msg,L"Text",text?text:L"");
        ui_play_tree(msg,L"Normal",L"EndNormal",0);
    }
}
static void guide_status_hide(void) {
    if(!live_page_on_top(live_status_page)) {live_status_page=0;return;}
    live_status_closing=TRUE;blades_back();live_status_closing=FALSE;
    live_status_page=0;
}

/* ---------- xam notify.xur popup ---------- */
static struct {DWORD scene,control,visual;int frames,serial,player;BOOL leaving;WCHAR text[256],image[512];} live_toast;   /* serial: one per popup */
#define TOAST_SHOW_FRAMES 300   /* TransTo loops until the popup is dismissed (~5 s) */
#define TOAST_LEAVE_FRAMES 60   /* TransFrom 186..240 */
static DWORD live_toast_scene(void) {return live_toast.scene;}
static void live_toast_close(void) {
    if(live_toast.scene) XUI_FN(0x7dec45,HRESULT,DWORD)(live_toast.scene);
    live_toast.scene=live_toast.control=0;live_toast.leaving=FALSE;
}
/* image: a file in xam_xam.xzp (signed_in.png, signed_out.png, ...) or a full URI. */
/* Widest line of a popup text in XUI units, measured with the same Convection face the
   engine draws with (assets\\fonts\\Convection-Medium.ttf, registered privately for GDI).
   LIVE_TEXT_EM converts XUI PointSize to the GDI em height; calibrated against the engine's
   rendered text (probe actions toast, toastfriend, toastlong: the engine needs about 1.5x the TTF em width before it stops clipping the last letter). */
#define LIVE_TEXT_EM 1.5f
#define LIVE_TOAST_MIN_W 140.0f
#define LIVE_TOAST_BOTTOM (58.7f+648.0f*660.0f/720.0f)   /* window y 648 = 90% of 720 */
static float live_text_width(const WCHAR *text,float points) {
    static HDC dc;static HFONT font;static float cached_points;
    if(!dc) {
        WCHAR path[MAX_PATH];DWORD n=GetModuleFileNameW(0,path,MAX_PATH);WCHAR *slash=n?wcsrchr(path,L'\\'):0;
        if(slash) {lstrcpyW(slash,L"\\assets\\fonts\\Convection-Medium.ttf");AddFontResourceExW(path,FR_PRIVATE,0);}
        dc=CreateCompatibleDC(0);
    }
    if(!dc) return 283.0f;
    if(!font || cached_points!=points) {
        if(font) DeleteObject(font);
        font=CreateFontW(-(int)(points*LIVE_TEXT_EM*16.0f+0.5f),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Convection Medium");
        cached_points=points;SelectObject(dc,font);
    }
    float widest=0;
    for(const WCHAR *line=text;line && *line;) {
        int len=0;while(line[len] && line[len]!=L'\r' && line[len]!=L'\n') len++;
        SIZE size={0};if(len) GetTextExtentPoint32W(dc,line,len,&size);
        if(size.cx/16.0f>widest) widest=size.cx/16.0f;
        line+=len;while(*line==L'\r' || *line==L'\n') line++;
    }
    return widest;
}
/* Text, icon, Xbox logo and ring quadrant on the popup's visual. Applied again whenever the
   engine rebuilds the visual (a graphics device reset at start-up does), which would
   otherwise bring back the authored defaults: all four quadrants lit and no icon. */
static void live_toast_dress(DWORD control) {
        /* The visual's xenonLogo.png is a hud skin file; relative to notify.xur it would be looked
           up in xam_xam.xzp. */
        static WCHAR logo[512];DWORD e=ui_find_deep(control,L"xenonLogo1",0);
        if(e) xui_string(e,L"ImagePath",guide_uri(logo,L"huduiskin_skin.xzp",L"xenonLogo.png"));
        xui_string(control,L"Text",live_toast.text);
        if(live_toast.image[0]) xui_string(control,L"ImagePath",live_toast.image);
        /* Ring of light: scr_Notification has one glow per controller quadrant (Indicator1 top
           left = player 1 .. Indicator4 bottom right, each with its own TransTo/TransFrom) and
           its TransTo/loop keys Show=True on all four from frame 44 to 192, so the visual alone
           lights the whole ring. NotifyPopupScene (xam code) picks the quadrant of the player the
           notification is about; Opacity is not keyed by the timeline, so the others stay off.
           Sign-in/out popups concern the player on controller 1 (this PC host has one
           controller); other popups are not about a player and light no quadrant. */
        for(int i=1;i<=4;i++) {
            WCHAR id[16];wsprintfW(id,L"Indicator%d",i);
            DWORD e=ui_find_deep(control,id,0);
            if(e) xui_number(e,L"Opacity",i==live_toast.player+1?1.0f:0.0f);
        }
    XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(control,&live_toast.visual);
}
static void guide_toast(const WCHAR *text,const WCHAR *image) {
    int player;   /* controller index the popup is about (0..3), -1 = none */
    /* Sign-in/out popups are system notifications the console always shows (the hud's
       "some system notifications will always appear"); others honour the setting. */
    /* image may be a bare xam_xam.xzp file name or a full URI (profiles passes ...xam_xam.xzp#signed_in.png) */
    const WCHAR *file=image?wcsrchr(image,L'#'):0;file=file?file+1:image;
    BOOL system=file && (!lstrcmpiW(file,L"signed_in.png") || !lstrcmpiW(file,L"signed_out.png") || !lstrcmpiW(file,L"signed_in_live.png"));
    if(!system && !setting_get_int(L"ShowNotifications",1)) {log_text("Notification hidden by settings");log_wide(text);return;}
    player=system?0:-1;
    live_toast_close();
    guide_frame();   /* loads the hud skin (scr_Notification lives there) and the frame below the popup */
    DWORD scene=dash_scene(L"xam_xam.xzp#notify.xur");
    if(!scene) {log_text("notify.xur missing");return;}
    if(FAILED(xui_add(ui_root,scene))) {XUI_FN(0x7dec45,HRESULT,DWORD)(scene);return;}
    xui_visuals(scene);guide_rebind(scene,0);
    DWORD control=ui_find_deep(scene,L"PopupControl",0);
    if(control) xui_string(control,L"Visual",L"hud.scr_Notification");
    lstrcpynW(live_toast.text,text?text:L"",256);
    if(image && !wcsstr(image,L"://")) guide_uri(live_toast.image,L"xam_xam.xzp",image);
    else lstrcpynW(live_toast.image,image?image:L"",512);
    live_toast.player=player;
    if(control) live_toast_dress(control);
    /* Bottom centre of the title-safe area of the 1120x770 dash canvas (shown window y 58.7..718.7). */
    /* NotifyPopupScene (xam code) sizes the popup to its text: scr_Notification is authored
       361 wide with its TextPresenter at x 59, 283 wide (19 to the right edge), and the bar,
       right cap and text are anchored to stretch with the control. So the popup is
       59 + widest line + 19 wide, centred at the bottom of the title-safe area. */
    float width=59.0f+live_text_width(live_toast.text,11.0f)+19.0f;
    if(width<LIVE_TOAST_MIN_W) width=LIVE_TOAST_MIN_W;
    if(width>460.0f) width=460.0f;
    xui_bounds(scene,560.0f-width/2,LIVE_TOAST_BOTTOM-87.0f,width,87);
    if(control) xui_bounds(control,0,0,width,87);
    /* notify.xur is authored on a 640x480 canvas: XAM scaled it with the screen height, so on a
       720-line screen it is 720/480 = 1.5x its authored size (uniformly, so the disc stays round).
       The dash view shows canvas 1120x660 (y 58.7..718.7) in the 1280x720 window, i.e. 1.143x
       wide and 1.091x tall, so the scene itself is scaled by 1.5 divided by those. Scaling
       pivots on the popup's bottom centre, which sits at the title-safe bottom (90% of the
       screen height). */
    {XuiValue v={.type=7};float *f=(float *)&v.integer;f[0]=1.5f*1120.0f/1280.0f;f[1]=1.5f*660.0f/720.0f;f[2]=1.0f;
     XuiValue p={.type=7};float *g=(float *)&p.integer;g[0]=width/2;g[1]=87.0f;g[2]=0.0f;
     xui_property(scene,L"Pivot",&p);xui_property(scene,L"Scale",&v);}
    log_value("Notification width",(DWORD)width);
    live_toast.scene=scene;live_toast.control=control;live_toast.frames=TOAST_SHOW_FRAMES;live_toast.leaving=FALSE;live_toast.serial++;
    if(control) ui_play_tree(control,L"TransTo",L"EndTransTo",0);
    /* PopUpSound (NotifyPopup.xma) is played by guide_sound.c, which honours NotificationSound. */
    log_text("Notification");log_wide(live_toast.text);log_value("Notification player quadrant",player+1);
}
/* The popup belongs above everything: a scene added to the UI root after it (a dashboard page,
   the example dashboard) would otherwise draw over it. Keep it the root's last child. */
static void live_toast_raise(void) {
    DWORD child=0,last=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(ui_root,&child);
    while(child) {last=child;XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
    if(!last || last==live_toast.scene) return;
    XUI_FN(0x7d3289,HRESULT,DWORD)(live_toast.scene);   /* XuiElementUnlink */
    if(FAILED(xui_add(ui_root,live_toast.scene))) {log_text("Notification could not be raised");live_toast.scene=live_toast.control=0;return;}
    log_text("Notification kept above a newer scene");
}
static void live_toast_tick(void) {
    if(live_toast.scene) live_toast_raise();
    if(live_toast.scene && live_toast.control) {
        DWORD visual=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(live_toast.control,&visual);
        if(visual!=live_toast.visual) {log_text("Notification visual rebuilt: dressed again");live_toast_dress(live_toast.control);}
    }
    if(!live_toast.scene || --live_toast.frames>0) return;
    if(!live_toast.leaving && live_toast.control) {
        live_toast.leaving=TRUE;live_toast.frames=TOAST_LEAVE_FRAMES;
        ui_play_tree(live_toast.control,L"TransFrom",L"EndTransFrom",0);
        return;
    }
    live_toast_close();
}

/* ---------- hooks ---------- */
static BOOL live_dispatch(DWORD button,const WCHAR *id) {
    if(!lstrcmpW(id,L"btnRecoverProfile") || !lstrcmpW(id,L"btnUseExistingTag")) {live_recover();return TRUE;}
    if(!lstrcmpW(id,L"btnSignInLive") || !lstrcmpW(id,L"btnConnect")) {live_upsell();return TRUE;}
    /* Xbox Live Marketplace (dash Live blade banner / Marketplace buttons, authored to
       1400_StoreFrontScene): no Live sign-in exists here, so the console's own refusal. */
    if(wcsstr(blades_nav_path(button),L"1400_StoreFrontScene")) {
        const WCHAR *b[]={S_MARKET_OK};
        log_text("Xbox Live Marketplace needs an Xbox Live sign-in");
        guide_msgbox(S_MARKET_TITLE,S_MARKET_SIGNIN,b,1,0,0,0);
        return TRUE;
    }
    if(!lstrcmpW(id,L"btnJoinLive") || !lstrcmpW(id,L"btnJoinXbox")) {live_join();return TRUE;}
    /* Family Settings > Xbox Live Controls (dash_accountm, an Xbox Live account page): needs an Xbox Live
       sign-in, so the Guide's own Xbox Live sign-in offer, as for Sign In to Xbox Live. */
    if(!lstrcmpW(id,L"btnLive")) {
        BladesPage *top=blades_page();
        if(top && top->catalog>=0 && !lstrcmpiW(blades_scenes[top->catalog].scene,L"dashSysCslSetPControlSelect.xur")) {
            log_text("Xbox Live Controls need an Xbox Live sign-in");live_upsell();return TRUE;
        }
    }
    /* Game banner: no game is running on the dashboard, so A does nothing. */
    if(!lstrcmpW(id,L"btnGameBanner")) {log_text("Game banner: no game running (no reaction)");return TRUE;}
    return FALSE;
}
static void live_notify(DWORD src,DWORD code,DWORD *data) {}
static void live_page(DWORD page,int catalog) {
    /* InfoUpsellLive's right column: the four Label_BodyWhite labels (XuiControl1..4) are
       authored on the hud skin visual; force it in case the rebind found no dash visual. */
    if(guide_page_is(page,L"scnInfoUpsellLive")) {
        const WCHAR *labels[]={L"XuiControl1",L"XuiControl2",L"XuiControl3",L"XuiControl4"};
        for(int i=0;i<4;i++) {DWORD e=guide_find(page,labels[i]);if(e) xui_string(e,L"Visual",L"hud.Label_BodyWhite");}
    }
}
static BOOL live_back(void) {
    if(msgbox_back()) return TRUE;
    if(live_page_on_top(live_status_page) && !live_status_closing) return TRUE;   /* signing out can't be cancelled */
    if(live_upsell_from_dash && live_page_on_top(live_upsell_page)) {
        /* The upsell was opened from the dashboard: B goes back to the dashboard. */
        live_upsell_from_dash=FALSE;live_upsell_page=0;
        if(blades_guide_open) {while(blades_guide_stack.depth) blades_guide_pop();blades_guide(FALSE);}
        return TRUE;
    }
    return FALSE;
}
/* Keys while a message box or the status page is up: only Up/Down/A reach the box; B goes to
   live_back; the Guide button, Y and the blade moves wait. */
static BOOL live_key(WORD key,BOOL down) {
    BOOL modal=guide_msgbox_open() || live_page_on_top(live_status_page);
    if(!modal) return FALSE;
    if(key==VK_BACK || key==0x5801) return FALSE;
    if(key==VK_UP || key==VK_DOWN || key==VK_RETURN || key==0x5800) return guide_msgbox_open()?FALSE:TRUE;
    return TRUE;
}
static BOOL live_guide_toggle_blocked(void) {return guide_msgbox_open() || live_page_on_top(live_status_page);}
/* While a box or the status page is up nothing else may act: the pad and keyboard are held by
   live_key; mouse input still reaches the engine (ui.c), so notifies from elements outside the
   modal scene (a click or hover on the page or dashboard below) are dropped here, and
   live_mouse_end gives the focus back to the box if the mouse moved it out. */
static DWORD live_modal_root(void) {return msgbox.box?msgbox.box:live_page_on_top(live_status_page)?live_status_page:0;}
static DWORD live_mouse_begin(void) {return live_modal_root()?xui_focused():0;}
static void live_mouse_end(DWORD focus) {
    DWORD root=live_modal_root(),now=xui_focused();
    /* Only the box's buttons may take the focus (a click on its message text would leave A nothing to press). */
    BOOL button=now && msgbox.box && blades_contains(msgbox.box,now,0) && !wcsncmp(blades_id(now),L"Button",6);
    if(root && focus && now!=focus && !button) xui_focus(focus);
}
static BOOL live_filter(DWORD element,DWORD *message) {
    if(msgbox_filter(element,message)) return TRUE;
    DWORD root=live_modal_root();
    if(root && message[1]==0xe && message[4]) {
        DWORD *n=(DWORD *)message[4];
        if(n[1] && !blades_contains(root,n[1],0) && !(msgbox.dying && blades_contains(msgbox.dying,n[1],0))) {message[2]=1;return TRUE;}
    }
    return FALSE;
}
/* blades_notice: a console message box with the console's OK button (signin [39]). */
static void live_notice(const WCHAR *text) {const WCHAR *b[]={S_BTN_OK};guide_msgbox(L"",text,b,1,0,0,0);}
static void live_test_tick(void);
/* hudbkgnd's frame timelines key Legend_A..Y Show on (HalfToFull, ErrorToHalf, ...), after the
   page's legend was applied: apply the page's legend again once the frame has settled. */
static void live_legend_tick(void) {
    static WCHAR state[8];static int settle;
    if(lstrcmpW(state,guide_state)) {lstrcpyW(state,guide_state);settle=64;}
    /* Applied every 4 frames until the frame timeline has run, so a legend the timeline turns on
       is hidden again before it shows for long. */
    if(settle>0 && (--settle&3)==0 && blades_guide_open && !guide_msgbox_open() && guide_top) guide_legend(guide_top);
}
static void live_tick(void) {
    msgbox_tick();live_flow_tick();live_toast_tick();live_legend_tick();
    live_test_tick();
}
static const GuideLegend live_legends[]={
    {L"downloadLiveSignupScene",0,L"Back"},{L"addXboxLiveScene",0,L"Back"},{L"sceneStatus",0,0},
    {L"scnInfoUpsellLive",L"Select",L"Back"}};
static const GuideLegend *live_legend(DWORD scene) {
    for(int i=0;i<(int)(sizeof live_legends/sizeof *live_legends);i++) if(ui_find_deep(scene,live_legends[i].scene,0)) return &live_legends[i];
    return 0;
}

static BOOL CALLBACK live_test_window(HWND w,LPARAM out) {if(IsWindowVisible(w)) {*(HWND *)out=w;return FALSE;}return TRUE;}
/* Probe-only hooks (cloud tests): XUI_LIVE=action@frame;action@frame...
   actions: box1 box2 box3 (message boxes), recover, join, upsell, toast, toastfriend, status, statushide, shot. */
static void live_test_tick(void) {
    static DWORD frame;static char spec[256];static int ready=-1;
    if(!probe_mode) return;
    if(ready<0) ready=GetEnvironmentVariableA("XUI_LIVE",spec,sizeof spec)?1:0;
    if(!ready) return;
    frame++;
    for(char *at=spec;*at;) {
        char name[32];int when=0,n=0,k=0;
        while(at[n] && at[n]!='@' && k<31) name[k++]=at[n++];
        name[k]=0;
        if(at[n]!='@') break;
        n++;while(at[n]>='0' && at[n]<='9') when=when*10+at[n++]-'0';
        if((DWORD)when==frame) {
            log_text("Live probe action");log_text(name);
            if(!strcmp(name,"box1")) {const WCHAR *b[]={S_BTN_OK};guide_msgbox(S_CANT_CONNECT_T,S_CANT_CONNECT,b,1,0,0,0);}
            else if(!strcmp(name,"box2")) {const WCHAR *b[]={S_YES,S_NO_CANCEL};guide_msgbox(S_RECOVER_TITLE,S_RECOVER_BODY,b,2,0,0,0);}
            else if(!strcmp(name,"box3")) {const WCHAR *b[]={L"Test Connection",S_JOIN_OK,L"Cancel"};guide_msgbox(S_JOIN_TITLE,S_JOIN_FAIL,b,3,1,0,0);}
            else if(!strcmp(name,"recover")) live_recover();
            else if(!strcmp(name,"join")) live_join();
            else if(!strcmp(name,"upsell")) live_upsell();
            else if(!strcmp(name,"page")) blades_navigate(blades_catalog(L"dash_dashmark.xzp",L"1400_StoreFrontScene.xur"));
            else if(!strcmp(name,"toast")) guide_toast(L"Player1\r\nsigned in",L"signed_in.png");
            else if(!strcmp(name,"toastlong")) guide_toast(L"Somebody With A Long Name\r\nhas sent you a friend request",L"friend_online.png");
            else if(!strcmp(name,"toastfriend")) guide_toast(L"Gamertag\r\nis online",L"friend_online.png");
            else if(!strcmp(name,"shot")) ui_shot_request=TRUE;   /* screenshot at an exact frame (animation strips) */
            else if(!strcmp(name,"status")) guide_status_show(L"Signing out");
            else if(!strcmp(name,"statushide")) guide_status_hide();
            else if(!strncmp(name,"click",5) || !strncmp(name,"hover",5)) {
                /* clickXxY / hoverXxY: a mouse click (or move) at client pixel X,Y through the window's message queue */
                int x=atoi(name+5),y=0;char *by=strchr(name,'x');if(by) y=atoi(by+1);
                HWND w=0;EnumThreadWindows(GetCurrentThreadId(),live_test_window,(LPARAM)&w);
                if(w) {
                    LPARAM xy=MAKELPARAM(x,y);PostMessageW(w,WM_MOUSEMOVE,0,xy);
                    if(name[0]=='c') {PostMessageW(w,WM_LBUTTONDOWN,MK_LBUTTON,xy);PostMessageW(w,WM_LBUTTONUP,0,xy);}
                }
                log_value("Probe mouse x",x);log_value("Probe mouse y",y);
            }
        }
        at+=n;while(*at==';') at++;
    }
}
