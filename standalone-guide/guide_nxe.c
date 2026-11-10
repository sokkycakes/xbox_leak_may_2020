/* NXE Guide: the Guide of the 2008 dashboard (2.0.7371, hud.xex patched by $flash_hud.xexp).
 *
 * Packages (x360/blades-converted): hud_nxe.xzp (the 7371 hud.xex XUIZ, scenes folded to GFWL base
 * classes by xurconv; SettingsTabSignedIn by hand) and huduiskin_nxe.xzp (the 7371
 * $flash_huduiskin.xex skin, loaded under the prefix "nxe."). With both present the Guide is the
 * NXE one; --blades-guide keeps the Blades (1888) Guide.
 *
 * GuideMain.xur is the main Guide: HUDScene > Tabscene with five blades (Tab1 Marketplace,
 * Tab2 Games, Tab3 Home, Tab4 Media, Tab5 Settings) and the ring of light. hud.xex fills
 * Tab2/3/5 with GamesTab*, HomeTab*, SettingsTab* by sign-in state (Tab1 and Tab4 are inline,
 * Tab4's scnMusic holds the mini player). Tabscene timelines: NToM moves between neighbouring
 * blades (LB/RB here, and Left/Right), NClose brings the blades in (used for the open).
 *
 * GuideMain is authored in the HUD buffer's own units (852x480, the blades centred on it), so it is
 * drawn 1:1 into that buffer and the buffer is stretched to the window (ui_hud_render).
 * The second-level pages (Preferences, Notifications, sign in, profiles, music, message boxes) are
 * the 1888 Guide's pages in the 1888 Guide panel (xam hudbkgnd): the 7371 hud's own copies of those
 * pages are drawn by the 7371 xam HUD frame, whose delta patch these tools could not decrypt. Quick
 * Launch (7371 only) opens on GuideMain in the blade panel's visual.
 */
static BOOL guide_nxe_off;
static DWORD nxe_backdrop,nxe_scene,nxe_hud,nxe_tabs,nxe_tab[5],nxe_content[5];
static int nxe_blade=3,nxe_shown_signed=-2;
static BOOL nxe_folded;
static struct {int blade;const WCHAR *id;} nxe_after;   /* dashboard page to open once the Guide has closed */
static const WCHAR *nxe_tab_files[5][2]={   /* [signed out, signed in] */
    {0,0},{L"GamesTabSignedOut.xur",L"GamesTabSignedIn.xur"},{L"HomeTabSignedOut.xur",L"HomeTabSignedIn.xur"},
    {0,0},{L"SettingsTabSignedOut.xur",L"SettingsTabSignedIn.xur"}};

static BOOL nxe_package_present(const WCHAR *file) {
    WCHAR path[4096];const WCHAR *base=dash_base;
    if(!wcsncmp(base,L"file://",7)) base+=7;
    wsprintfW(path,L"%s%s",base,file);
    return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
}
static BOOL nxe_wanted(void) {
    if(guide_nxe_off || wcsstr(GetCommandLineW(),L"--blades-guide")) return FALSE;
    return nxe_package_present(L"hud_nxe.xzp") && nxe_package_present(L"huduiskin_nxe.xzp");
}
static BOOL nxe_up(void) {return guide_nxe && nxe_scene && blades_guide_open;}
/* An element of the NXE Guide (GuideMain or a page opened on it): its visuals come from "nxe.". */
static BOOL nxe_owns(DWORD element) {return nxe_scene && element && blades_contains(nxe_scene,element,0);}
static void nxe_rebind(DWORD scene) {
    const WCHAR *was=guide_pfx;guide_pfx=L"nxe.";guide_rebind(scene,0);guide_pfx=was;
}
static int nxe_catalog(void) {
    int i=nxe_blade-1,signed_in=profile_signed>=0;
    if(!nxe_tab_files[i][0]) return blades_catalog(L"hud_nxe.xzp",L"GuideMain.xur");
    return blades_catalog(L"hud_nxe.xzp",nxe_tab_files[i][signed_in]);
}
/* Tab2/3/5 contents by sign-in state (hud.xex's tab scenes load these on sign-in changes). */
static void nxe_fill_tabs(void) {
    if(!nxe_scene) return;
    int signed_in=profile_signed>=0;
    if(signed_in==nxe_shown_signed) return;
    nxe_shown_signed=signed_in;
    for(int i=0;i<5;i++) {
        if(!nxe_tab[i]) continue;
        if(!nxe_tab_files[i][0]) {nxe_content[i]=nxe_tab[i];continue;}
        if(nxe_content[i] && nxe_content[i]!=nxe_tab[i]) {guide_list_unbind_tree(nxe_content[i]);XUI_FN(0x7dec45,HRESULT,DWORD)(nxe_content[i]);}
        WCHAR file[128];wsprintfW(file,L"hud_nxe.xzp#%s",nxe_tab_files[i][signed_in]);
        DWORD content=dash_scene(file);
        nxe_content[i]=nxe_tab[i];
        if(!content) {log_wide(file);log_text("NXE Guide: tab scene did not load");continue;}
        dash_attach(nxe_tab[i],content,0,0);nxe_rebind(content);
        nxe_content[i]=content;
        log_wide(file);
    }
    if(blades_guide_open) {
        guide_menu=blades_guide_home=nxe_content[nxe_blade-1];
        if(blades_guide_stack.depth) blades_guide_stack.root_focus=0;
        else {blades_focus(blades_guide_home,nxe_catalog());guide_top=blades_guide_home;}
    }
}
/* Only the current blade takes focus; the others keep their buttons out of the focus chain. */
static void nxe_focus_blade(BOOL focus) {
    for(int i=0;i<5;i++) if(nxe_tab[i]) xui_bool(nxe_tab[i],L"DisableFocusRecursion",i!=nxe_blade-1);
    guide_menu=blades_guide_home=nxe_content[nxe_blade-1];
    if(focus) blades_focus(blades_guide_home,nxe_catalog());
    guide_top=blades_guide_home;
}
static void nxe_jump(int blade) {   /* to a blade without animation (the end frame of a move into it) */
    if(blade==nxe_blade || !nxe_tabs) return;
    WCHAR frame[16];wsprintfW(frame,L"%dTo%dEnd",blade<nxe_blade?blade+1:blade-1,blade);
    ui_play_tree(nxe_tabs,frame,frame,0);
    nxe_blade=blade;
}
/* LB/RB (and Left/Right on the blade lists): the neighbouring blade, as the console's Guide. */
static BOOL nxe_move(int step) {
    int next=nxe_blade+step;
    if(!nxe_up() || blades_guide_stack.depth || next<1 || next>5) return FALSE;
    WCHAR start[16],end[16];wsprintfW(start,L"%dTo%d",nxe_blade,next);wsprintfW(end,L"%sEnd",start);
    HRESULT hr=ui_play_tree(nxe_tabs,start,end,0);
    log_wide(start);log_value("NXE Guide blade move",hr);
    if(FAILED(hr)) return FALSE;
    nxe_blade=next;nxe_focus_blade(TRUE);
    return TRUE;
}
static void nxe_fold(BOOL fold) {
    if(fold==nxe_folded || !nxe_tabs) return;
    WCHAR start[16],end[16];wsprintfW(start,L"%d%s",nxe_blade,fold?L"Open":L"Close");wsprintfW(end,L"%sEnd",start);
    log_wide(start);log_value("NXE Guide blades",ui_play_tree(nxe_tabs,start,end,0));
    nxe_folded=fold;
}
static void nxe_build(void) {
    WCHAR skin[4096];wsprintfW(skin,L"%shuduiskin_nxe.xzp#skin.xur",dash_base);
    log_wide(skin);log_value("NXE Guide skin",XUI_FN(0x7d6909,HRESULT,const WCHAR *,const WCHAR *)(skin,L"nxe."));
    nxe_scene=dash_scene(L"hud_nxe.xzp#GuideMain.xur");
    if(!nxe_scene || FAILED(xui_add(ui_root,nxe_scene))) {
        if(nxe_scene) XUI_FN(0x7dec45,HRESULT,DWORD)(nxe_scene);
        nxe_scene=0;guide_nxe=FALSE;log_text("NXE Guide could not load; the Blades Guide is used");return;
    }
    xui_visuals(nxe_scene);nxe_rebind(nxe_scene);
    nxe_hud=nxe_scene;   /* the loader returns the scene under the canvas: GuideMain's HUDScene */
    nxe_tabs=ui_find_deep(nxe_scene,L"Tabscene",0);
    for(int i=0;i<5;i++) {WCHAR id[8];wsprintfW(id,L"Tab%d",i+1);nxe_tab[i]=nxe_tabs?ui_find_deep(nxe_tabs,id,0):0;nxe_content[i]=nxe_tab[i];}
    nxe_blade=3;nxe_shown_signed=-2;nxe_folded=FALSE;
    {   /* Quick Launch has no panel of its own (xam's HUD frame backs it on the console): the blade
           panel visual behind its tabs and lists, shown only while it is up (nxe_visibility). */
        XuiElement e=0;nxe_backdrop=0;
        if(SUCCEEDED(xui_create(L"XuiControl",&e)) && SUCCEEDED(xui_add(nxe_scene,e))) {
            xui_string(e,L"Id",L"nxeQuickLaunchPanel");xui_string(e,L"Visual",L"nxe.HUD_Bladedark");
            xui_bounds(e,124,24,600,396);xui_bool(e,L"Show",FALSE);nxe_backdrop=(DWORD)e;
        }
    }
    nxe_fill_tabs();
    blades_visible(nxe_scene,FALSE);
    log_text("NXE Guide ready (GuideMain, 5 blades)");
}
/* The Guide button: it opens on Home with the blades coming in (3Close), as hud.xex's open. */
static void nxe_show(BOOL open) {
    if(!nxe_scene) return;
    if(open) {
        nxe_fill_tabs();
        if(nxe_folded) {WCHAR f[16];wsprintfW(f,L"%dCloseEnd",nxe_blade);ui_play_tree(nxe_tabs,f,f,0);nxe_folded=FALSE;}
        nxe_jump(3);
        blades_visible(nxe_scene,TRUE);
        nxe_folded=TRUE;nxe_fold(FALSE);
        nxe_focus_blade(FALSE);
    } else {
        blades_visible(nxe_scene,FALSE);
    }
}
static void nxe_to_dash(int blade,const WCHAR *id) {
    nxe_after.blade=blade;nxe_after.id=id;
    blades_pending_back=2;fse_to_dash=TRUE;
}
static void nxe_tick(void) {
    if(!nxe_scene) return;
    if(nxe_after.blade && !blades_guide_open && blades_ready && ui_mode==2) {
        int blade=nxe_after.blade;const WCHAR *id=nxe_after.id;nxe_after.blade=0;
        while(ui_blade!=blade && ui_blade_move(blade<ui_blade?-1:1)) ;
        DWORD button=id && dash_root[blade-1]?ui_find_deep(dash_root[blade-1],id,0):0;
        log_value("NXE Guide: dashboard blade",blade);
        if(button) {xui_focus(button);log_wide(id);blades_dispatch(button);}
    }
}
/* A page from hud_nxe.xzp opens on the NXE Guide; the others (xam / 1888 pages) in hudbkgnd. */
static BOOL nxe_page(int catalog) {
    return guide_nxe && nxe_scene && catalog>=0 && !lstrcmpiW(blades_scenes[catalog].package,L"hud_nxe.xzp");
}
/* Keep GuideMain on screen only while it is the Guide in front (not under a 1888 page). */
static void nxe_visibility(void) {
    if(!nxe_scene) return;
    BOOL show=blades_guide_open && (!lstrcmpW(guide_state,L"Closed") || !lstrcmpW(guide_state,L"Error"));
    xui_bool(nxe_scene,L"Show",show);

    /* Quick Launch is a whole Guide of its own (its own HUDScene and tabs): the blades step aside. */
    BladesPage *top=blades_page();
    BOOL own_page=blades_guide_open && top && nxe_owns(top->scene);
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(nxe_scene,&child);   /* nxe_scene is GuideMain's HUDScene */
    while(child) {
        BOOL page=FALSE;for(int i=0;i<blades_guide_stack.depth;i++) page|=blades_guide_stack.pages[i].scene==child;
        if(!page) xui_bool(child,L"Show",child==nxe_backdrop?own_page:!own_page);
        XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);
    }
}
static BOOL nxe_key(WORD key,BOOL down) {
    if(!nxe_up() || blades_guide_stack.depth || blades_notice_root) return FALSE;
    if(key==VK_PRIOR || key==VK_NEXT) {if(down) nxe_move(key==VK_PRIOR?-1:1);return TRUE;}
    if(key==VK_LEFT || key==VK_RIGHT) {
        /* The mini player's buttons sit side by side: Left/Right stay theirs. */
        DWORD f=xui_focused();
        if(blades_music_scene && blades_contains(blades_music_scene,f,0)) return FALSE;
        if(down) nxe_move(key==VK_LEFT?-1:1);
        return TRUE;
    }
    return FALSE;
}
static void nxe_go(const WCHAR *scene) {
    if(!blades_navigate(blades_catalog(L"hud_nxe.xzp",scene))) return;
    BladesPage *top=blades_page();DWORD page=top?top->scene:0;
    if(page && !lstrcmpW(scene,L"QuickLaunch.xur")) {
        /* Quick Launch: hud.xex fills Recent / Latest Downloads / All from the console's title
           history and content; there is none here, so each tab shows its own empty-list text. */
        static const WCHAR *tabs[]={L"Tab1",L"Tab2",L"Tab3"};
        for(int i=0;i<3;i++) {
            DWORD tab=ui_find_deep(page,tabs[i],0);if(!tab) continue;
            guide_show(tab,L"waitCursorControl",FALSE);guide_show(tab,L"ProgressAnimation",FALSE);
            guide_show(tab,L"text_PercentFull",FALSE);
            BOOL sign_in=i==0 && profile_signed<0;   /* Recent needs a signed-in profile */
            guide_show(tab,L"labelSignIn",sign_in);guide_show(tab,L"labelNoGames",!sign_in);
        }
    }
}
/* NXE Guide buttons onto the Guide features the host has (the 1888 Guide's modules). */
static BOOL nxe_dispatch(DWORD button,const WCHAR *id) {
    if(!nxe_up() || !nxe_owns(button)) return FALSE;
    BladesPage *top=blades_page();
    if(top && nxe_owns(top->scene)) return FALSE;   /* NXE pages: their own buttons / authored paths */
    /* Home */
    if(!lstrcmpW(id,L"btnDashboard")) {nxe_to_dash(0,0);return TRUE;}
    if(!lstrcmpW(id,L"btnJumpIn")) {nxe_go(L"QuickLaunch.xur");return TRUE;}
    if(!lstrcmpW(id,L"btnCreateNewGamertag")) {profiles_open_create();return TRUE;}
    if(!lstrcmpW(id,L"btnRecoverGamertag")) {live_recover();return TRUE;}
    if(!lstrcmpW(id,L"btnConnectToLive")) {live_upsell();return TRUE;}
    if(!lstrcmpW(id,L"btnDiscInTray")) {log_text("NXE Guide: no disc drive (no reaction)");return TRUE;}
    /* Games */
    if(!lstrcmpW(id,L"btnAchievements")) {prof_go(L"gamerprofile_gp.xzp",L"820_GameShowcaseMe.xur");return TRUE;}
    if(!lstrcmpW(id,L"btnGamesLibrary")) {nxe_to_dash(2,0);return TRUE;}
    if(!lstrcmpW(id,L"btnPrimetime")) {log_text("NXE Guide: Reminders need Xbox LIVE (no reaction)");return TRUE;}
    /* Marketplace: no Xbox LIVE sign-in here, so the console's refusal. */
    if(!lstrcmpW(id,L"btnGameMarketplace") || !lstrcmpW(id,L"btnVideoMarketplace") || !lstrcmpW(id,L"btnRedeemCode") || !lstrcmpW(id,L"btnActiveDownloads")) {
        const WCHAR *b[]={S_MARKET_OK};
        guide_msgbox(S_MARKET_TITLE,S_MARKET_SIGNIN,b,1,0,0,0);
        return TRUE;
    }
    /* Media */
    if(!lstrcmpW(id,L"btnMusicLibrary")) {music_open(MP_SOURCES);return TRUE;}
    if(!lstrcmpW(id,L"btnVideoLibrary")) {nxe_to_dash(3,L"navVideo");return TRUE;}
    if(!lstrcmpW(id,L"btnPictureLibrary")) {nxe_to_dash(3,L"navPictures");return TRUE;}
    if(!lstrcmpW(id,L"btnMCX")) {log_text("NXE Guide: Windows Media Center not available (no reaction)");return TRUE;}
    /* Settings */
    if(!lstrcmpW(id,L"btnProfile")) {profiles_open_gamercard();return TRUE;}
    /* Preferences and Notifications: the 1888 Guide's pages (the 7371 hud's copies are drawn by the
       7371 xam HUD frame, which these files do not have), in the Guide panel as before. */
    if(!lstrcmpW(id,L"btnNotifications")) {blades_navigate(blades_catalog(L"hud_hud.xzp",L"OptionsNotifications.xur"));return TRUE;}
    if(!lstrcmpW(id,L"btnPersonalSettings")) {blades_navigate(blades_catalog(L"hud_hud.xzp",L"Options.xur"));return TRUE;}
    if(!lstrcmpW(id,L"btnFamilySettings")) {nxe_to_dash(4,L"navPControls");return TRUE;}
    if(!lstrcmpW(id,L"btnSystemSettings")) {nxe_to_dash(4,L"navSettings");return TRUE;}
    if(!lstrcmpW(id,L"btnAccountManagement")) {live_upsell();return TRUE;}
    if(!lstrcmpW(id,L"btnShutdown")) return settings_dispatch(button,L"btnTurnOff");
    return FALSE;
}
