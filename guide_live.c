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
static struct {DWORD scene,control;int frames;BOOL leaving;WCHAR text[256],image[512];} live_toast;
#define TOAST_SHOW_FRAMES 300   /* TransTo loops until the popup is dismissed (~5 s) */
#define TOAST_LEAVE_FRAMES 60   /* TransFrom 186..240 */
static void live_toast_close(void) {
    if(live_toast.scene) XUI_FN(0x7dec45,HRESULT,DWORD)(live_toast.scene);
    live_toast.scene=live_toast.control=0;live_toast.leaving=FALSE;
}
/* image: a file in xam_xam.xzp (signed_in.png, signed_out.png, ...) or a full URI. */
static void guide_toast(const WCHAR *text,const WCHAR *image) {
    /* Sign-in/out popups are system notifications the console always shows (the hud's
       "some system notifications will always appear"); others honour the setting. */
    BOOL system=image && (!lstrcmpiW(image,L"signed_in.png") || !lstrcmpiW(image,L"signed_out.png") || !lstrcmpiW(image,L"signed_in_live.png"));
    if(!system && !setting_get_int(L"ShowNotifications",1)) {log_text("Notification hidden by settings");log_wide(text);return;}
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
    if(control) {
        /* The visual's xenonLogo.png is a hud skin file; relative to notify.xur it would be looked
           up in xam_xam.xzp. */
        static WCHAR logo[512];DWORD e=ui_find_deep(control,L"xenonLogo1",0);
        if(e) xui_string(e,L"ImagePath",guide_uri(logo,L"huduiskin_skin.xzp",L"xenonLogo.png"));
        xui_string(control,L"Text",live_toast.text);
        if(live_toast.image[0]) xui_string(control,L"ImagePath",live_toast.image);
    }
    /* Bottom centre of the title-safe area of the 1120x770 dash canvas (shown window y 58.7..718.7). */
    xui_bounds(scene,330,592,460,87);
    live_toast.scene=scene;live_toast.control=control;live_toast.frames=TOAST_SHOW_FRAMES;live_toast.leaving=FALSE;
    if(control) ui_play_tree(control,L"TransTo",L"EndTransTo",0);
    /* PopUpSound (NotifyPopup.xma): the engine's sound classes run without an audio device here
       and XMA cannot be decoded on the PC, so NotificationSound has nothing to play. */
    log_text("Notification");log_wide(live_toast.text);
}
static void live_toast_tick(void) {
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
    if(!lstrcmpW(id,L"btnSignInLive")) {
        /* Connect to Xbox Live with a local (non-Live) profile: the Live upsell (Full Guide). */
        blades_navigate(blades_catalog(L"hud_hud.xzp",L"InfoUpsellLive.xur"));
        return TRUE;
    }
    if(!lstrcmpW(id,L"btnJoinLive") || !lstrcmpW(id,L"btnJoinXbox")) {live_join();return TRUE;}
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
static BOOL live_filter(DWORD element,DWORD *message) {return msgbox_filter(element,message);}
/* blades_notice: a console message box with the console's OK button (signin [39]). */
static void live_notice(const WCHAR *text) {const WCHAR *b[]={S_BTN_OK};guide_msgbox(L"",text,b,1,0,0,0);}
static void live_test_tick(void);
/* hudbkgnd's frame timelines key Legend_A..Y Show on (HalfToFull, ErrorToHalf, ...), after the
   page's legend was applied: apply the page's legend again once the frame has settled. */
static void live_legend_tick(void) {
    static WCHAR state[8];static int settle;
    if(lstrcmpW(state,guide_state)) {lstrcpyW(state,guide_state);settle=64;}
    if(settle>0 && --settle==0 && blades_guide_open && !guide_msgbox_open() && guide_top) guide_legend(guide_top);
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

/* Probe-only hooks (cloud tests): XUI_LIVE=action@frame;action@frame...
   actions: box1 box2 box3 (message boxes), recover, join, upsell, toast, status, statushide. */
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
            else if(!strcmp(name,"upsell")) blades_navigate(blades_catalog(L"hud_hud.xzp",L"InfoUpsellLive.xur"));
            else if(!strcmp(name,"toast")) guide_toast(L"Player1\r\nsigned in",L"signed_in.png");
            else if(!strcmp(name,"status")) guide_status_show(L"Signing out");
            else if(!strcmp(name,"statushide")) guide_status_hide();
        }
        at+=n;while(*at==';') at++;
    }
}
