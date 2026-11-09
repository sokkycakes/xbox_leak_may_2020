/* Guide module: profiles. Local gamer profiles (create / sign in / sign out / gamer picture /
 * gamercard / auto sign-in), built only from the console's own scenes and strings.
 *
 * Storage: profiles\<n>.ini [Profile] Name, Picture (xam tile id), Gamerscore, Achievements,
 * TitlesPlayed, Live=0, Created; profiles\console.ini [Console] SignedIn=<n>, AutoSignIn=<n> (0 = none).
 * Slot n is profile index+1 (setting_get_int in guide_core.c uses the same file).
 *
 * The profile UI is XAM system UI on the console (sign-in, create profile, virtual keyboard, gamer
 * profile): it is shown in the Guide frame's Full state whether it was started from the Guide or from
 * the dashboard blades. A flow started from the dashboard opens the Guide and closes it again at the end.
 *
 * Console strings used (package#table[index]):
 *   xam_shrdres LiveProfile.xus [6] keyboard description, [7]..[12] profile-name validation; LiveAll.xus [3] "OK"
 *   createprofile_cp CreateProfile_Custom.xus [12] "Gamer Profile" (keyboard title)
 *   signin strings.xus / createprofile [29] "Signing in"
 *   gamerprofile_gp GamerProfile_Custom.xus [8]/[27] 807 meta panel, [25]/[31] auto sign-in, [30] "Edit Profile Name", [56]..[61] Live-only
 *   hud Strings.xus [38]/[39] profile count, [40]/[41]/[42] sign-out box, [44] "Signing out"
 *   dash dashStrings.xus [8]/[9]/[10] profile count, [17]/[21] "Sign Out", [31] "Open Tray"
 *   xam Strings.xus [43] "%s\nsigned in", [46] "%s\nsigned out"
 */
#define PROF_MAX 32
#define PROF_PICTURES 13
typedef struct {BOOL used;WCHAR name[16];int picture,gamerscore,achievements,titles;} Profile;
static Profile prof[PROF_MAX];
static int prof_count;
static BOOL prof_loaded;

/* ---------- strings ---------- */
static const WCHAR *PS_KB_DESC=L"Enter a profile name. Your profile name can contain up to 15 characters.";
static const WCHAR *PS_KB_TITLE=L"Gamer Profile";
static const WCHAR *PS_EDIT_NAME=L"Edit Profile Name";
static const WCHAR *PS_SIGNING_IN=L"Signing in";
static const WCHAR *PS_SIGNING_OUT=L"Signing out";
static const WCHAR *PS_SIGNOUT_TITLE=L"Sign Out";
static const WCHAR *PS_SIGNOUT_BODY=L"Are you sure you want to sign out? If you are playing a game, you will lose any unsaved progress.";
static const WCHAR *PS_SIGNOUT_YES=L"Yes, sign out";
static const WCHAR *PS_SIGNOUT_NO=L"No, don't sign out";
static const WCHAR *PS_OK=L"OK";
static const WCHAR *PS_AUTO_ON=L"Enable Auto Sign-In";
static const WCHAR *PS_AUTO_OFF=L"Disable Auto Sign-In";

/* ---------- storage ---------- */
static void prof_file(WCHAR *name,int index) {wsprintfW(name,L"%d.ini",index+1);}
static const WCHAR *prof_tile_id(int picture,WCHAR *out) {
    if(picture<0 || picture>=PROF_PICTURES) picture=0;
    wsprintfW(out,L"fffe07d1000200%02x0001%04x",picture,picture);return out;
}
static void prof_load(void) {
    prof_count=0;
    for(int i=0;i<PROF_MAX;i++) {
        Profile *p=&prof[i];memset(p,0,sizeof *p);
        WCHAR path[MAX_PATH],name[32];prof_file(name,i);
        lstrcpyW(path,store_folder());lstrcatW(path,name);
        if(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES) continue;   /* do not create empty slot files */
        GetPrivateProfileStringW(L"Profile",L"Name",L"",p->name,16,path);
        if(!p->name[0]) continue;
        WCHAR tile[64];GetPrivateProfileStringW(L"Profile",L"Picture",L"",tile,64,path);
        p->picture=0;
        for(int k=0;k<PROF_PICTURES;k++) {WCHAR id[32];if(!lstrcmpiW(tile,prof_tile_id(k,id))) p->picture=k;}
        p->gamerscore=(int)GetPrivateProfileIntW(L"Profile",L"Gamerscore",0,path);
        p->achievements=(int)GetPrivateProfileIntW(L"Profile",L"Achievements",0,path);
        p->titles=(int)GetPrivateProfileIntW(L"Profile",L"TitlesPlayed",0,path);
        p->used=TRUE;prof_count++;
    }
    prof_loaded=TRUE;
    if(profile_signed>=0 && (profile_signed>=PROF_MAX || !prof[profile_signed].used)) profile_signed=-1;
}
static void prof_save(int index) {
    Profile *p=&prof[index];WCHAR name[32],tile[32],num[16];prof_file(name,index);
    ini_set_str(name,L"Profile",L"Name",p->name);
    ini_set_str(name,L"Profile",L"Picture",prof_tile_id(p->picture,tile));
    wsprintfW(num,L"%d",p->gamerscore);ini_set_str(name,L"Profile",L"Gamerscore",num);
    wsprintfW(num,L"%d",p->achievements);ini_set_str(name,L"Profile",L"Achievements",num);
    wsprintfW(num,L"%d",p->titles);ini_set_str(name,L"Profile",L"TitlesPlayed",num);
    ini_set_str(name,L"Profile",L"Live",L"0");   /* local profile: never an Xbox Live account */
}
static int prof_create(const WCHAR *gamertag,int picture) {
    for(int i=0;i<PROF_MAX;i++) if(!prof[i].used) {
        WCHAR path[MAX_PATH],name[32];prof_file(name,i);lstrcpyW(path,store_folder());lstrcatW(path,name);
        DeleteFileW(path);                         /* a stale nameless slot file */
        Profile *p=&prof[i];memset(p,0,sizeof *p);
        lstrcpynW(p->name,gamertag,16);p->picture=picture;p->used=TRUE;prof_count++;
        prof_save(i);
        SYSTEMTIME t;GetLocalTime(&t);WCHAR when[32];
        wsprintfW(when,L"%04d-%02d-%02dT%02d:%02d:%02d",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
        ini_set_str(name,L"Profile",L"Created",when);
        log_text("Profile created");log_wide(p->name);
        return i;
    }
    return -1;
}
/* Gamer picture URIs (xam_shrdres.xzp 64_/32_<tile id>.png). Static: lists keep the pointers. */
static WCHAR prof_pic_buf[PROF_PICTURES][2][600];
static const WCHAR *prof_picture_uri(int picture,int size) {
    if(picture<0 || picture>=PROF_PICTURES) picture=0;
    WCHAR *out=prof_pic_buf[picture][size==32];
    if(!out[0]) {WCHAR id[32],file[64];wsprintfW(file,L"%d_%s.png",size,prof_tile_id(picture,id));guide_uri(out,L"xam_shrdres.xzp",file);}
    return out;
}

/* Console name rules (xam_shrdres LiveProfile.xus). Returns 0 when valid. */
static BOOL prof_alpha(WCHAR c) {return (c>=L'a' && c<=L'z') || (c>=L'A' && c<=L'Z');}
static const WCHAR *prof_validate(const WCHAR *n,int self) {
    static WCHAR used[160];
    int len=lstrlenW(n);
    if(!len) return L"Your profile must have a name.";
    for(int i=0;i<len;i++) if(!prof_alpha(n[i]) && !(n[i]>=L'0' && n[i]<=L'9') && n[i]!=L' ')
        return L"Your profile name can consist only of letters, numbers, and spaces. ";
    if(!prof_alpha(n[0])) return L"Your profile name must begin with a letter.";
    for(int i=1;i<len;i++) if(n[i]==L' ' && n[i-1]==L' ') return L"Your profile name cannot contain two spaces in a row.";
    if(n[len-1]==L' ') return L"Your profile name cannot end with a space.";
    for(int i=0;i<PROF_MAX;i++) if(i!=self && prof[i].used && !lstrcmpiW(prof[i].name,n)) {
        wsprintfW(used,L"The profile name '%s' is already in use. Please choose another name.",n);return used;
    }
    return 0;
}

/* ---------- engine bits ---------- */
static void prof_frame(DWORD owner,const WCHAR *start,const WCHAR *end) {
    int a=-1,b=-1;if(!owner) return;
    XUI_FN(0x7d389d,HRESULT,DWORD,const WCHAR *,int *)(owner,start,&a);
    XUI_FN(0x7d389d,HRESULT,DWORD,const WCHAR *,int *)(owner,end?end:start,&b);
    if(a>=0 && b>=0) XUI_FN(0x7d3867,HRESULT,DWORD,int,int,int,BOOL,BOOL)(owner,a,a,b,FALSE,FALSE);
}
static void prof_unlink(DWORD e) {if(e) XUI_FN(0x7d3289,HRESULT,DWORD)(e);}          /* XuiElementUnlink */
static void prof_destroy(DWORD e) {if(e) XUI_FN(0x7dec45,HRESULT,DWORD)(e);}
static DWORD prof_parent(DWORD e) {DWORD p=0;if(e) XUI_FN(0x7d32af,HRESULT,DWORD,DWORD *)(e,&p);return p;}
static void prof_hide_scene(DWORD e,BOOL show) {if(e) {xui_bool(e,L"Show",show);xui_bool(e,L"DisableFocusRecursion",!show);}}
static int prof_catalog(DWORD page) {
    BladesStack *stacks[2]={&blades_guide_stack,&blades_dash_stack};
    for(int s=0;s<2;s++) for(int i=0;i<stacks[s]->depth;i++) if(stacks[s]->pages[i].scene==page) return stacks[s]->pages[i].catalog;
    return -1;
}
static BOOL prof_scene_is(int catalog,const WCHAR *scene) {return catalog>=0 && !lstrcmpiW(blades_scenes[catalog].scene,scene);}
static int prof_top_catalog(void) {BladesPage *p=blades_page();return p?p->catalog:-1;}
static BOOL prof_top_is(const WCHAR *scene) {return prof_scene_is(prof_top_catalog(),scene);}

/* ---------- gamercards (xam_gamercrd.xzp#GamerCard.xur) ---------- */
static void prof_card_fill(DWORD card,int index) {
    if(!card || index<0 || index>=PROF_MAX || !prof[index].used) return;
    Profile *p=&prof[index];
    DWORD off=ui_find_deep(card,L"offlineBaseScene",0),on=ui_find_deep(card,L"onlineBaseScene",0);
    if(on) xui_bool(on,L"Show",FALSE);
    if(!off) return;
    xui_bool(off,L"Show",TRUE);
    WCHAR num[16];
    dash_set_text(off,L"gamerTagText",p->name);
    wsprintfW(num,L"%d",p->gamerscore);dash_set_text(off,L"credText",num);
    wsprintfW(num,L"%d",p->titles);dash_set_text(off,L"titlesPlayedText",num);
    wsprintfW(num,L"%d",p->achievements);dash_set_text(off,L"achievementsText",num);
    DWORD img=ui_find_deep(off,L"gamerTileImage",0);if(img) xui_string(img,L"ImagePath",prof_picture_uri(p->picture,64));
}
/* frame: Normal (blades, gamer profile pages) or Mini (Guide main menu). */
static DWORD prof_card_attach(DWORD slot,const WCHAR *frame,BOOL extended) {
    if(!slot || profile_signed<0) return 0;
    DWORD old=ui_find_deep(slot,L"gamerCardScene",0);
    if(old) {DWORD parent=prof_parent(old);prof_destroy(parent && parent!=slot?parent:old);}
    DWORD card=blades_load(blades_catalog(L"xam_gamercrd.xzp",L"GamerCard.xur"));
    if(!card) {log_text("Gamercard scene could not load");return 0;}
    dash_attach(slot,card,0,0);
    if(guide_scene && blades_contains(guide_scene,slot,0)) guide_prepare(card);
    DWORD root=ui_find_deep(card,L"gamerCardScene",0);if(!root) root=card;
    WCHAR end[32];wsprintfW(end,L"End%s",frame);
    prof_frame(root,frame,end);
    prof_frame(ui_find_deep(card,L"offlineBaseScene",0),frame,0);
    DWORD ext=ui_find_deep(card,L"extendedScene",0);
    if(ext) {
        prof_hide_scene(ext,extended);
        if(extended) {
            prof_frame(ext,L"Offline",0);
            dash_set_text(ext,L"topText",L"");dash_set_text(ext,L"bottomText",L"");
        }
    }
    prof_card_fill(card,profile_signed);
    return card;
}
/* Refresh every card on screen after a picture/name change. */
static void prof_card_refresh_tree(DWORD root,int depth) {
    if(!root || depth>14) return;
    if(!lstrcmpW(blades_id(root),L"gamerCardScene")) {prof_card_fill(root,profile_signed);return;}
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(root,&child);
    while(child) {prof_card_refresh_tree(child,depth+1);XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
}
static void prof_cards_refresh(void) {
    if(ui_external) prof_card_refresh_tree(ui_external,0);
    if(guide_scene) prof_card_refresh_tree(guide_scene,0);
    for(int i=0;i<blades_dash_stack.depth;i++) prof_card_refresh_tree(blades_dash_stack.pages[i].scene,0);
    if(guide_scene && profile_signed>=0) dash_set_text(guide_scene,L"GamerTag",prof[profile_signed].name);
}

/* ---------- profile count labels ---------- */
static const WCHAR *prof_count_text(void) {
    static WCHAR text[64];
    if(!prof_count) return L"No Profiles Found";                         /* dashStrings 9 / signin strings 47 */
    wsprintfW(text,prof_count==1?L"%d Profile Found":L"%d Profiles Found",prof_count);   /* hud 39 / hud 38, dashStrings 10 / 8 */
    return text;
}

/* ---------- Guide main menu (MainMenuSignedOut / MainMenuSignedIn) ---------- */
static const WCHAR *profiles_menu_file(void) {
    return profile_signed>=0?L"hud_hud.xzp#MainMenuSignedIn.xur":L"hud_hud.xzp#MainMenuSignedOut.xur";
}
static const WCHAR *profiles_menu_scene(void) {
    return profile_signed>=0?L"MainMenuSignedIn.xur":L"MainMenuSignedOut.xur";
}
static void prof_nav(DWORD scene,const WCHAR *id,const WCHAR *prop,const WCHAR *target) {
    DWORD e=ui_find_deep(scene,id,0);if(e) xui_string(e,prop,target);
}
static void prof_menu_fill(DWORD menu) {
    if(!menu) return;
    if(ui_find_deep(menu,L"ctlGamerCard",0)) {
        /* Offline variant: the Live rows (community bar, private chat) are replaced by the
           "Not connected to Xbox Live" label and "Connect to Xbox Live" (signedin.md section 2). */
        guide_show(menu,L"btnQuickChat",FALSE);
        prof_hide_scene(ui_find_deep(menu,L"scnCommunity",0),FALSE);
        guide_show(menu,L"txtSignInLive",TRUE);guide_show(menu,L"btnSignInLive",TRUE);
        prof_nav(menu,L"ctlGamerCard",L"NavDown",L"btnSignInLive");
        prof_nav(menu,L"btnSignInLive",L"NavUp",L"ctlGamerCard");
        prof_nav(menu,L"btnSignInLive",L"NavDown",L"btnOptions");
        prof_nav(menu,L"btnOptions",L"NavUp",L"btnSignInLive");
        prof_card_attach(ui_find_deep(menu,L"ctlGamerCard",0),L"Mini",FALSE);
    } else dash_set_text(menu,L"labelProfiles",prof_count_text());
}
static void profiles_guide_menu(void) {
    if(!guide_scene || !guide_host || !guide_menu) return;
    BOOL want=profile_signed>=0,have=ui_find_deep(guide_menu,L"ctlGamerCard",0)!=0;
    if(want!=have) {
        DWORD menu=dash_scene(profiles_menu_file());
        if(!menu) return;
        if(blades_music_scene) prof_unlink(blades_music_scene);
        prof_destroy(guide_menu);
        dash_attach(guide_host,menu,0,0);
        guide_prepare(menu);
        guide_menu=blades_guide_home=menu;
        DWORD holder=ui_find_deep(menu,L"scnMusic",0);
        if(holder && blades_music_scene) {dash_attach(holder,blades_music_scene,0,0);guide_prepare(blades_music_scene);}
        log_text("Guide main menu swapped");log_wide(profiles_menu_file());
        blades_visible(menu,blades_guide_open && !blades_guide_stack.depth);
        prof_menu_fill(menu);
        if(blades_guide_open && !blades_guide_stack.depth) {
            blades_focus(menu,blades_catalog(L"hud_hud.xzp",profiles_menu_scene()));
            guide_top=menu;guide_legend(menu);
        }
    } else prof_menu_fill(guide_menu);
    dash_set_text(guide_scene,L"GamerTag",profile_signed>=0?prof[profile_signed].name:L"");
}

/* ---------- dashboard blades (signed-out <-> signed-in scenes) ---------- */
static const DashBlade prof_blades_in[3]={
    {L"Tab1",L"liveSignedInNLUI.xur",L"dash_live.xzp",L"MetaContainer",354,122,{L"baseMetaPane"},{L"dash_botd.xzp#defaultbanner0.xur"}},
    {L"Tab2",L"gamesSignedIn.xur",L"dash_gamesbla.xzp",L"metaPanelScene",0,0,
        {L"baseMetaPane",L"navAchievements",L"navGames",L"navArcade",L"navDemos",L"navTrailers"},
        {L"gamesMetaMOTD.xur",L"gamesMetaAchievements.xur",L"gamesMetaGames.xur",L"gamesMetaArcade.xur",L"gamesMetaDemos.xur",L"gamesMetaTrailers.xur"}},
    {L"Tab3",L"mediaSignedIn.xur",L"dash_mediabla.xzp",L"metaPanelScene",0,0,
        {L"baseMetaPane",L"navMusic",L"navPictures",L"navVideo",L"navMCX"},
        {L"mediaMetaMOTD.xur",L"mediaMetaMusic.xur",L"mediaMetaPictures.xur",L"mediaMetaVideo.xur",L"mediaMetaMCX.xur"}}};
static DashBlade prof_blades_out[3];
static BOOL prof_blades_saved;
static void profiles_dash_blades(void) {
    if(!dash_app || !ui_external) return;
    if(!prof_loaded) prof_load();
    if(!prof_blades_saved) {memcpy(prof_blades_out,dash_blades,sizeof prof_blades_out);prof_blades_saved=TRUE;}
    BOOL in=profile_signed>=0;
    for(int i=0;i<3;i++) {
        const DashBlade *want=in?&prof_blades_in[i]:&prof_blades_out[i];
        DWORD tab=ui_find_deep(ui_external,dash_blades[i].tab,0);if(!tab) continue;
        if(lstrcmpiW(dash_blades[i].content,want->content) || !dash_root[i] || dash_root[i]==tab) {
            DWORD holder=ui_find_deep(tab,L"scContainer",0);
            WCHAR file[512];wsprintfW(file,L"%s#%s",want->package,want->content);
            DWORD content=holder?dash_scene(file):0;
            if(content) {
                if(dash_root[i] && dash_root[i]!=tab) prof_destroy(dash_root[i]);
                dash_blades[i]=*want;
                dash_attach(holder,content,0,0);dash_root[i]=content;
                dash_meta[i]=0;dash_shown[i]=-1;
                dash_panel[i]=want->x?content:ui_find_deep(content,want->panel,0);
                dash_show_meta(i,0);
                log_wide(file);log_text("Blade content swapped");
            }
        }
        dash_set_text(dash_root[i],L"labFoundProfiles",prof_count_text());
        dash_set_text(dash_root[i],L"btn_tray",L"Open Tray");                   /* dashStrings 31 */
        if(in) prof_card_attach(ui_find_deep(dash_root[i],L"ctlGamerCard",0),L"Normal",FALSE);
    }
    for(int i=0;i<4;i++) {
        DWORD tab=ui_find_deep(ui_external,dash_blades[i].tab,0);
        DWORD x=tab?ui_find_deep(tab,L"legend_x",0):0;if(!x) continue;
        xui_string(x,L"Text",in?L"Sign Out":L"");                           /* dashStrings 17 */
        xui_bool(x,L"Enabled",in);
    }
    dash_focused=0;
    if(!blades_guide_open && !blades_dash_stack.depth && ui_mode==2) ui_dash_enter(ui_blade);
}

/* ---------- flows ---------- */
static BOOL prof_popping;
static BOOL prof_from_dash;      /* the flow opened the Guide from the dashboard: close it at the end */
static int prof_flow_depth;      /* guide stack depth when the flow started */
static void prof_back_one(void) {prof_popping=TRUE;blades_back();prof_popping=FALSE;}
static void prof_pop_to(int depth) {
    blades_notice_close();
    BladesStack *s=blades_stack();
    for(int guard=0;s->depth>depth && guard<BLADES_STACK_MAX;guard++) prof_back_one();
}
/* Start a profile flow: XAM UI goes in the Guide frame. */
static BOOL prof_flow_begin(void) {
    if(!blades_guide_open) {
        blades_guide(TRUE);
        if(!blades_guide_open) return FALSE;
        prof_from_dash=TRUE;prof_flow_depth=0;
    } else if(!prof_from_dash) prof_flow_depth=blades_guide_stack.depth;
    return TRUE;
}
static void prof_close_if_dash(void) {
    if(prof_from_dash && blades_guide_open && !blades_guide_stack.depth) {prof_from_dash=FALSE;blades_guide(FALSE);}
}
static void prof_flow_end(void) {
    prof_pop_to(prof_flow_depth);
    prof_close_if_dash();
    prof_flow_depth=0;
}
static void prof_go(const WCHAR *package,const WCHAR *scene) {blades_navigate(blades_catalog(package,scene));}

/* sign in / out */
static int prof_signin_wait,prof_signin_index=-1,prof_signout_wait;
static WCHAR prof_toast_text[64];
static void prof_toast(const WCHAR *format,const WCHAR *name,const WCHAR *image) {
    static WCHAR uri[600];
    wsprintfW(prof_toast_text,format,name);guide_uri(uri,L"xam_xam.xzp",image);
    log_text("Toast");log_wide(prof_toast_text);
    guide_toast(prof_toast_text,uri);
}
static void prof_apply_state(void) {
    console_set_int(L"SignedIn",profile_signed+1);
    profiles_guide_menu();
    profiles_dash_blades();
}
static void profiles_sign_in(int index,BOOL end_flow) {
    if(index<0 || index>=PROF_MAX || !prof[index].used) return;
    profile_signed=index;
    log_text("Profile signed in");log_wide(prof[index].name);
    if(end_flow) prof_flow_end();
    prof_apply_state();
    prof_toast(L"%s\nsigned in",prof[index].name,L"signed_in.png");           /* xam Strings.xus 43 */
}
static void profiles_sign_out(void) {
    if(profile_signed<0) return;
    WCHAR name[16];lstrcpyW(name,prof[profile_signed].name);
    log_text("Profile signed out");log_wide(name);
    profile_signed=-1;
    if(blades_guide_open) prof_pop_to(0);
    prof_apply_state();
    prof_toast(L"%s\nsigned out",name,L"signed_out.png");                     /* xam Strings.xus 46 */
}
static void prof_signout_choice(int choice,void *ctx) {
    /* Until the live module's message box lands, the placeholder answers -1 at once;
       probes set XUI_MSGBOX_CHOICE to stand in for the button the user would pick. */
    if(choice<0) {char c[8];if(GetEnvironmentVariableA("XUI_MSGBOX_CHOICE",c,8)) choice=atoi(c);}
    log_value("Sign out choice",(DWORD)choice);
    if(choice!=0) return;
    guide_status_show(PS_SIGNING_OUT);                                         /* hud Strings.xus 44 */
    prof_signout_wait=blades_guide_open?120:60;   /* in the Guide the Error panel slides back for ~60 frames first: keep "Signing out" up ~1 s after it */
}
static void profiles_sign_out_confirm(void) {
    if(profile_signed<0) return;
    static const WCHAR *buttons[2];buttons[0]=PS_SIGNOUT_YES;buttons[1]=PS_SIGNOUT_NO;
    guide_msgbox(PS_SIGNOUT_TITLE,PS_SIGNOUT_BODY,buttons,2,0,prof_signout_choice,0);
}
static void prof_message(const WCHAR *title,const WCHAR *body) {
    static const WCHAR *ok[1];ok[0]=PS_OK;
    guide_msgbox(title,body,ok,1,0,0,0);
}

/* sign-in pane: signin1.xur Scene_Pane0 <- UserList.xur, then status.xur */
static DWORD prof_userlist,prof_userlist_list;
static int prof_list_count(void *ctx) {return prof_count;}
static int prof_list_slot(int row) {for(int i=0;i<PROF_MAX;i++) if(prof[i].used && !row--) return i;return -1;}
static const WCHAR *prof_list_text(void *ctx,int row) {int i=prof_list_slot(row);return i<0?0:prof[i].name;}
static const WCHAR *prof_list_image(void *ctx,int row) {int i=prof_list_slot(row);return i<0?0:prof_picture_uri(prof[i].picture,64);}
static void prof_signin_page(DWORD page) {
    DWORD pane=ui_find_deep(page,L"Scene_Pane0",0);if(!pane) return;
    DWORD ul=blades_load(blades_catalog(L"signin_signin.xzp",L"UserList.xur"));if(!ul) return;
    prof_userlist=ul;
    prof_userlist_list=ui_find_deep(ul,L"listUser",0);
    dash_attach(pane,ul,0,0);
    if(blades_guide_open) guide_prepare(ul);
    /* UserList ships listUser with Visual 'List_SignIn' (rows = btn_SignIn: picture + gamertag). This xlive
       build's XuiList visual loader creates no rows from that visual (its template is the first child; the same
       happens with List_standard), so the Guide's common list visual is used: rows show the gamertag only. */
    if(prof_userlist_list) xui_string(prof_userlist_list,L"Visual",L"hud.list_Common");
    guide_list_bind(prof_userlist_list,prof_list_count,prof_list_text,prof_list_image,0);
    if(prof_userlist_list) {xui_focus(prof_userlist_list);xui_set_cursel(prof_userlist_list,0);}
}
static void prof_signin_status(int index) {
    BladesPage *top=blades_page();if(!top) return;
    DWORD pane=ui_find_deep(top->scene,L"Scene_Pane0",0);if(!pane) return;
    xui_focus(top->scene);
    if(prof_userlist) {guide_list_unbind_tree(prof_userlist);prof_destroy(prof_userlist);prof_userlist=prof_userlist_list=0;}
    DWORD st=blades_load(blades_catalog(L"signin_signin.xzp",L"status.xur"));
    if(st) {
        dash_attach(pane,st,0,0);
        if(blades_guide_open) guide_prepare(st);
        guide_show(st,L"txtWaiting",FALSE);guide_show(st,L"btnStart",FALSE);
        guide_text(st,L"txtStarting",PS_SIGNING_IN);
        guide_text(st,L"SelectedProfile",prof[index].name);
        guide_text(st,L"label_Gamertag",L"");
        DWORD sel=ui_find_deep(st,L"SelectedProfile",0);
        if(sel) xui_string(sel,L"ImagePath",prof_picture_uri(prof[index].picture,64));
    }
    prof_signin_index=index;prof_signin_wait=50;
}
static void profiles_open_signin(void) {
    if(!prof_loaded) prof_load();
    if(!prof_flow_begin()) return;
    if(!prof_count) {   /* signin strings.xus [8]: no profiles on this console -> create one */
        log_text("No profiles: create flow");
        prof_go(L"createprofile_cp.xzp",L"CreateGamerProfile.xur");
        return;
    }
    prof_go(L"signin_signin.xzp",L"signin1.xur");
}

/* virtual keyboard: vk_vk.xzp#Keyboard.xur */
enum {KB_NONE,KB_CREATE,KB_RENAME};
static int kb_mode;
static WCHAR kb_buf[16];
static int kb_len,kb_cur;
static BOOL kb_caps;
static DWORD kb_page;
static int prof_new_picture;
static WCHAR prof_new_name[16];
static BOOL prof_kb_up(void) {BladesPage *p=blades_ready?blades_page():0;return kb_page && p && p->scene==kb_page;}
static void kb_show(void) {
    if(!prof_kb_up()) return;
    DWORD edit=ui_find_deep(kb_page,L"XuiEdit1",0);if(!edit) return;
    xui_string(edit,L"Text",kb_buf);
    DWORD d[1]={(DWORD)kb_cur};xui_send(edit,0x7eb,d,sizeof d);
}
static void kb_insert(WCHAR c) {
    if(kb_len>=15) return;                                  /* XuiEdit TextLimit 15 */
    memmove(kb_buf+kb_cur+1,kb_buf+kb_cur,(kb_len-kb_cur+1)*sizeof(WCHAR));
    kb_buf[kb_cur++]=c;kb_len++;kb_show();
}
static void kb_backspace(void) {
    if(!kb_cur) return;
    memmove(kb_buf+kb_cur-1,kb_buf+kb_cur,(kb_len-kb_cur+1)*sizeof(WCHAR));
    kb_cur--;kb_len--;kb_show();
}
static void kb_move(int step) {kb_cur+=step;if(kb_cur<0) kb_cur=0;if(kb_cur>kb_len) kb_cur=kb_len;kb_show();}
static void kb_set_caps(BOOL caps) {
    kb_caps=caps;
    for(WCHAR c=L'a';c<=L'z';c++) {
        WCHAR id[8],text[2]={caps?(WCHAR)towupper(c):c,0};wsprintfW(id,L"Key.%c",towupper(c));
        DWORD key=ui_find_deep(kb_page,id,0);if(key) xui_string(key,L"Text",text);
    }
}
static void prof_open_keyboard(int mode,const WCHAR *text) {
    kb_mode=mode;lstrcpynW(kb_buf,text?text:L"",16);kb_len=kb_cur=lstrlenW(kb_buf);
    if(!blades_navigate(blades_catalog(L"vk_vk.xzp",L"Keyboard.xur"))) {kb_mode=KB_NONE;return;}
    kb_page=blades_page()->scene;
    dash_set_text(kb_page,L"Text_Header",mode==KB_RENAME?PS_EDIT_NAME:PS_KB_TITLE);
    dash_set_text(kb_page,L"XuiText1",PS_KB_DESC);
    dash_set_text(kb_page,L"XuiText2",L"");
    DWORD edit=ui_find_deep(kb_page,L"XuiEdit1",0);if(edit) xui_int(edit,L"TextLimit",15);
    kb_set_caps(FALSE);kb_show();
}
static void prof_keyboard_close(void) {
    if(prof_kb_up()) prof_back_one();
    kb_page=0;
}
static void kb_done(void) {
    int self=kb_mode==KB_RENAME?profile_signed:-1;
    const WCHAR *error=prof_validate(kb_buf,self);
    if(error) {log_text("Profile name rejected");log_wide(error);prof_message(PS_KB_TITLE,error);return;}
    if(kb_mode==KB_RENAME) {
        if(profile_signed>=0) {lstrcpyW(prof[profile_signed].name,kb_buf);prof_save(profile_signed);log_text("Profile renamed");log_wide(kb_buf);}
        prof_keyboard_close();kb_mode=KB_NONE;prof_cards_refresh();
        return;
    }
    lstrcpyW(prof_new_name,kb_buf);
    log_text("Profile name accepted");log_wide(prof_new_name);
    prof_keyboard_close();kb_mode=KB_NONE;
    BladesPage *host=blades_page();
    if(host) dash_set_text(host->scene,L"gamerTagText",prof_new_name);
    prof_go(L"createprofile_cp.xzp",L"Congratulations.xur");
}
static void kb_cancel(void) {
    int mode=kb_mode;kb_mode=KB_NONE;
    prof_keyboard_close();
    if(mode==KB_CREATE && prof_top_is(L"CreateGamerProfile.xur")) prof_back_one();   /* leave the invisible host too */
    prof_close_if_dash();
}
static BOOL kb_press(const WCHAR *id) {
    if(!wcsncmp(id,L"Key.",4)) {
        const WCHAR *k=id+4;
        if(!lstrcmpW(k,L"OK")) kb_done();
        else if(!lstrcmpW(k,L"BS")) kb_backspace();
        else if(!lstrcmpW(k,L"Spc")) kb_insert(L' ');
        else if(!lstrcmpW(k,L"Left")) kb_move(-1);
        else if(!lstrcmpW(k,L"Right")) kb_move(1);
        else if(!lstrcmpW(k,L"Caps")) kb_set_caps(!kb_caps);
        else if(!lstrcmpW(k,L"Prev") || !lstrcmpW(k,L"Next")) log_text("Keyboard symbol/accent sets are in vk.xex code (missing)");
        else if(k[0] && !k[1]) {WCHAR c=k[0];if(c>=L'A' && c<=L'Z' && !kb_caps) c=(WCHAR)towlower(c);kb_insert(c);}
        return TRUE;
    }
    if(!lstrcmpW(id,L"legend_x")) {kb_backspace();return TRUE;}
    if(!lstrcmpW(id,L"legend_y")) {kb_insert(L' ');return TRUE;}
    return FALSE;
}
/* Characters the vk grid offers (letters, digits, space, - @ _ .). */
static BOOL kb_char_ok(WCHAR c) {
    return prof_alpha(c) || (c>=L'0' && c<=L'9') || c==L' ' || c==L'-' || c==L'@' || c==L'_' || c==L'.';
}
/* PC keyboard text (SDL_EVENT_TEXT_INPUT) while the virtual keyboard is up. */
static void profiles_text(const char *utf8) {
    if(!prof_kb_up() || !utf8) return;
    WCHAR text[64];int n=MultiByteToWideChar(CP_UTF8,0,utf8,-1,text,64);
    for(int i=0;i<n && text[i];i++) if(kb_char_ok(text[i])) kb_insert(text[i]);
}
static BOOL profiles_wants_text(void) {return prof_kb_up();}
/* Physical keyboard while the virtual keyboard is up: typing arrives as text; Backspace edits,
   Enter is Done; arrows and Escape keep their navigation/back meaning. TRUE = consumed. */
static BOOL kb_enter_pending;
static BOOL profiles_pc_key(WORD key,BOOL down) {
    if(!prof_kb_up()) return FALSE;
    if(key==VK_BACK) {if(down) kb_backspace();return TRUE;}
    /* Enter waits for the tick: SDL hands out the text typed in the same message pump only after this. */
    if(key==VK_RETURN) {if(down) kb_enter_pending=TRUE;return TRUE;}
    if(key==VK_DELETE) {if(down && kb_cur<kb_len) {kb_move(1);kb_backspace();}return TRUE;}
    if(key==VK_HOME) {if(down) kb_move(-99);return TRUE;}
    if(key==VK_END) {if(down) kb_move(99);return TRUE;}
    if(key==VK_SPACE || (key>='0' && key<='9') || (key>='A' && key<='Z') || (key>=VK_OEM_1 && key<=VK_OEM_8) || (key>=VK_NUMPAD0 && key<=VK_DIVIDE)) return TRUE;
    return FALSE;
}
/* Gamepad/host keys while the virtual keyboard is up: X Backspace, Y Space, LB/RB cursor. */
static BOOL profiles_key(WORD key,BOOL down) {
    if(!prof_kb_up()) return FALSE;
    if(key==0x5802) {if(down) kb_backspace();return TRUE;}
    if(key==0x5803 || key=='Y') {if(down) kb_insert(L' ');return TRUE;}
    if(key==VK_PRIOR) {if(down) kb_move(-1);return TRUE;}
    if(key==VK_NEXT) {if(down) kb_move(1);return TRUE;}
    return FALSE;
}

/* create profile */
static void profiles_open_create(void) {
    if(!prof_loaded) prof_load();
    if(!prof_flow_begin()) return;
    prof_go(L"createprofile_cp.xzp",L"CreateGamerProfile.xur");
}

/* Congratulations focus transitions (frames authored on the scene; timelines move the meta panels). */
static DWORD prof_congrats_focus;
static void prof_congrats_tick(void) {
    BladesPage *p=blades_ready?blades_page():0;
    if(!p || !prof_scene_is(p->catalog,L"Congratulations.xur")) {prof_congrats_focus=0;return;}
    DWORD f=xui_focused();if(f==prof_congrats_focus) return;
    const WCHAR *from=blades_id(prof_congrats_focus),*to=blades_id(f);
    const WCHAR *a=0,*b=0;
    if(!lstrcmpW(from,L"doneButton") && !lstrcmpW(to,L"editProfileButton")) {a=L"DoneToEditProfile";b=L"EndDoneToEditProfile";}
    if(!lstrcmpW(from,L"editProfileButton") && !lstrcmpW(to,L"doneButton")) {a=L"EditProfileToDone";b=L"EndEditProfileToDone";}
    if(!lstrcmpW(from,L"editProfileButton") && !lstrcmpW(to,L"getLiveButton")) {a=L"EditProfileToGetLive";b=L"EndEditProfileToGetLive";}
    if(!lstrcmpW(from,L"getLiveButton") && !lstrcmpW(to,L"editProfileButton")) {a=L"GetLiveToEditProfile";b=L"EditGetLiveToEditProfile";}
    if(a) {DWORD root=ui_find_deep(p->scene,L"congratulationsScene",0);prof_frame(root?root:p->scene,a,b);}
    prof_congrats_focus=f;
}
static void prof_congrats_page(DWORD page) {
    DWORD root=ui_find_deep(page,L"congratulationsScene",0);
    prof_frame(root?root:page,L"Normal",0);
    dash_set_text(page,L"gamerTagText",prof_new_name);
    dash_set_text(page,L"GSTextnumber",L"0");dash_set_text(page,L"titlesPlayednumber",L"0");dash_set_text(page,L"achievementsnumber",L"0");
    DWORD img=ui_find_deep(page,L"gamerTileImage",0);if(img) xui_string(img,L"ImagePath",prof_picture_uri(prof_new_picture,64));
    prof_congrats_focus=xui_focused();
}

/* gamer profile pages (gamerprofile_gp.xzp) */
static const WCHAR *prof_live_only(const WCHAR *id) {
    if(!lstrcmpW(id,L"accountManagementButton")) return L"You must be signed in to Xbox Live to manage your Xbox Live membership.";
    if(!lstrcmpW(id,L"gamerZoneButton")) return L"You must be signed in to Xbox Live to change your Xbox Live Gamer Zone.";
    if(!lstrcmpW(id,L"gamerTagButton")) return L"You must be signed in to Xbox Live to change your gamertag.";
    if(!lstrcmpW(id,L"gamerMottoButton")) return L"You must be signed in to Xbox Live to change your motto.";
    if(!lstrcmpW(id,L"viewReputationButton")) return L"You must be signed in to Xbox Live to view your reputation.";
    if(!lstrcmpW(id,L"downloadTilesButton")) return L"You must be signed in to Xbox Live to download gamer pictures.";
    return 0;
}
static void prof_autosignin_text(DWORD page) {
    BOOL on=profile_signed>=0 && console_get_int(L"AutoSignIn",0)==profile_signed+1;
    DWORD group=ui_find_deep(page,L"meOfflineButtonGroup",0);
    if(group) dash_set_text(group,L"autoSignInButton",on?PS_AUTO_OFF:PS_AUTO_ON);
}
static int prof_tile_count(void *ctx) {return PROF_PICTURES;}
static const WCHAR *prof_tile_text(void *ctx,int i) {return L"";}
static const WCHAR *prof_tile_image(void *ctx,int i) {return prof_picture_uri(i,64);}
static DWORD prof_tile_list;
static void prof_gp_page(DWORD page,int catalog) {
    const WCHAR *scene=blades_scenes[catalog].scene;
    DWORD slot=ui_find_deep(page,L"gamerCard",0);
    if(!lstrcmpiW(scene,L"GamerCardScene.xur")) {
        prof_card_attach(slot,L"Normal",TRUE);
        prof_hide_scene(ui_find_deep(page,L"meOnlineButtonGroup",0),FALSE);
        DWORD group=ui_find_deep(page,L"meOfflineButtonGroup",0);prof_hide_scene(group,TRUE);
        prof_autosignin_text(page);
        DWORD first=group?ui_find_deep(group,L"viewGamesButton",0):0;if(first) xui_focus(first);
    } else if(!lstrcmpiW(scene,L"808_editProfile.xur")) {
        prof_card_attach(slot,L"Normal",FALSE);
        prof_hide_scene(ui_find_deep(page,L"onlineButtonGroup",0),FALSE);
        DWORD group=ui_find_deep(page,L"offlineButtonGroup",0);prof_hide_scene(group,TRUE);
        guide_show(page,L"waitCursorControl",FALSE);
        DWORD first=group?ui_find_deep(group,L"gamerTileButton",0):0;if(first) xui_focus(first);
    } else if(!lstrcmpiW(scene,L"809_ChangeGamerTile.xur")) {
        prof_card_attach(slot,L"Normal",FALSE);
        guide_show(page,L"waitCursorControl",FALSE);
        prof_tile_list=ui_find_deep(page,L"gamerTileList",0);
        guide_list_bind(prof_tile_list,prof_tile_count,prof_tile_text,prof_tile_image,0);
        if(prof_tile_list) {xui_focus(prof_tile_list);if(profile_signed>=0) xui_set_cursel(prof_tile_list,prof[profile_signed].picture);}
    } else if(slot) prof_card_attach(slot,L"Normal",FALSE);
}
static void profiles_open_gamercard(void) {
    if(profile_signed<0) return;
    if(!prof_flow_begin()) return;
    prof_go(L"gamerprofile_gp.xzp",L"GamerCardScene.xur");
}

/* ---------- module hooks ---------- */
static BOOL profiles_dispatch(DWORD button,const WCHAR *id) {
    if(!prof_loaded) prof_load();
    int top=prof_top_catalog();
    if(prof_kb_up()) return kb_press(id);
    if(!lstrcmpW(id,L"btnSignIn") || !lstrcmpW(id,L"fakeGamerCard")) {profiles_open_signin();return TRUE;}
    if(!lstrcmpW(id,L"btnNewProfile") || !lstrcmpW(id,L"navCreateProfile")) {profiles_open_create();return TRUE;}
    if(!lstrcmpW(id,L"btnX") && blades_guide_open && !blades_guide_stack.depth) {
        if(profile_signed>=0) profiles_sign_out_confirm(); else profiles_open_signin();
        return TRUE;
    }
    if((!lstrcmpW(id,L"baseMetaPane") || !lstrcmpW(id,L"ctlGamerCard")) && profile_signed>=0) {
        if(top>=0 && !lstrcmpiW(blades_scenes[top].package,L"gamerprofile_gp.xzp")) return TRUE;
        profiles_open_gamercard();return TRUE;
    }
    if(prof_scene_is(top,L"CreateGamerProfile.xur") && !lstrcmpW(id,L"nextButton")) {
        if(prof_new_name[0]) prof_go(L"createprofile_cp.xzp",L"Congratulations.xur");
        else prof_open_keyboard(KB_CREATE,L"");
        return TRUE;
    }
    if(prof_scene_is(top,L"Congratulations.xur")) {
        BOOL done=!lstrcmpW(id,L"doneButton"),edit=!lstrcmpW(id,L"editProfileButton"),live=!lstrcmpW(id,L"getLiveButton");
        if(!done && !edit && !live) return FALSE;
        log_text("Congratulations button");log_wide(id);
        int index=prof_create(prof_new_name,prof_new_picture);
        prof_new_name[0]=0;
        if(index<0) {log_text("No free profile slot");return TRUE;}
        if(edit) {
            /* Customize Profile: signed in, then Edit Gamer Profile in the Guide; B leaves as the flow began. */
            BOOL dash=prof_from_dash;prof_from_dash=FALSE;
            profiles_sign_in(index,TRUE);
            prof_from_dash=dash;prof_flow_depth=blades_guide_stack.depth;
            prof_go(L"gamerprofile_gp.xzp",L"808_editProfile.xur");
        } else {
            profiles_sign_in(index,TRUE);
            if(live) live_join();
        }
        return TRUE;
    }
    if(prof_scene_is(top,L"GamerCardScene.xur")) {
        if(!lstrcmpW(id,L"editProfileButton")) {prof_go(L"gamerprofile_gp.xzp",L"808_editProfile.xur");return TRUE;}
        if(!lstrcmpW(id,L"viewGamesButton")) {prof_go(L"gamerprofile_gp.xzp",L"820_GameShowcaseMe.xur");return TRUE;}
        if(!lstrcmpW(id,L"autoSignInButton") && profile_signed>=0) {
            BOOL on=console_get_int(L"AutoSignIn",0)==profile_signed+1;
            console_set_int(L"AutoSignIn",on?0:profile_signed+1);
            log_value("Auto sign-in slot",on?0:profile_signed+1);
            prof_autosignin_text(blades_page()->scene);return TRUE;
        }
        if(!lstrcmpW(id,L"addLiveButton")) {live_join();return TRUE;}
    }
    if(prof_scene_is(top,L"808_editProfile.xur")) {
        if(!lstrcmpW(id,L"gamerTileButton")) {prof_go(L"gamerprofile_gp.xzp",L"807_EditGamerTile.xur");return TRUE;}
        if(!lstrcmpW(id,L"profileNameButton") && profile_signed>=0) {prof_open_keyboard(KB_RENAME,prof[profile_signed].name);return TRUE;}
    }
    if(top>=0 && !lstrcmpiW(blades_scenes[top].package,L"gamerprofile_gp.xzp")) {
        const WCHAR *live=prof_live_only(id);
        if(live) {prof_message(L"Gamer Profile",live);return TRUE;}
    }
    return FALSE;
}
static int prof_signin_pending,prof_tile_pending;
/* Gamer picture chosen in 809: save it, return to Edit Gamer Profile and refresh every gamercard. */
static void prof_set_picture(int picture) {
    if(profile_signed<0 || picture<0 || picture>=PROF_PICTURES) return;
    prof[profile_signed].picture=picture;prof_save(profile_signed);
    log_value("Gamer picture",(DWORD)picture);
    BladesStack *s=blades_stack();int depth=s->depth;
    for(int i=s->depth-1;i>=0;i--) if(prof_scene_is(s->pages[i].catalog,L"808_editProfile.xur")) {depth=i+1;break;}
    prof_pop_to(depth);
    prof_cards_refresh();
}
static void profiles_notify(DWORD src,DWORD code,DWORD *data) {
    if(code!=1 || !src || !blades_ready) return;
    if(src==prof_userlist_list && prof_top_is(L"signin1.xur") && !prof_signin_wait) {
        int row=xui_cursel(src),index=prof_list_slot(row);
        log_value("Sign-in list press",(DWORD)row);
        if(index>=0) prof_signin_pending=index+1;   /* swap the pane after the list has finished its press */
        return;
    }
    if(src==prof_tile_list && prof_top_is(L"809_ChangeGamerTile.xur") && profile_signed>=0) {
        int picture=xui_cursel(src);
        if(picture>=0 && picture<PROF_PICTURES) prof_tile_pending=picture+1;   /* pop after the list's press */
        return;
    }
    /* Dashboard X legend "Sign Out" (outside the blade content, so blades_dispatch never sees it). */
    if(!lstrcmpW(blades_id(src),L"legend_x") && !blades_guide_open && profile_signed>=0 && !blades_dash_stack.depth && !blades_contains(blades_current(),src,0))
        profiles_sign_out_confirm();
}
static void profiles_page(DWORD page,int catalog) {
    if(catalog<0) return;
    const WCHAR *scene=blades_scenes[catalog].scene;
    if(!lstrcmpiW(scene,L"signin1.xur")) prof_signin_page(page);
    else if(!lstrcmpiW(scene,L"CreateGamerProfile.xur")) {
        /* the console gives a new profile one of its built-in gamer pictures */
        prof_new_name[0]=0;prof_new_picture=(int)(GetTickCount()%PROF_PICTURES);
        prof_open_keyboard(KB_CREATE,L"");
    } else if(!lstrcmpiW(scene,L"Congratulations.xur")) prof_congrats_page(page);
    else if(!lstrcmpiW(blades_scenes[catalog].package,L"gamerprofile_gp.xzp")) prof_gp_page(page,catalog);
}
static BOOL profiles_back(void) {
    if(prof_popping) return FALSE;
    if(prof_signin_wait || prof_signout_wait) return TRUE;
    if(prof_kb_up()) {kb_cancel();return TRUE;}
    if(prof_top_is(L"Congratulations.xur")) {          /* back to the keyboard with the chosen name */
        prof_back_one();
        prof_open_keyboard(KB_CREATE,prof_new_name);
        return TRUE;
    }
    if(!blades_guide_open || !prof_from_dash || blades_guide_stack.depth>1) {
        if(prof_top_is(L"CreateGamerProfile.xur")) {prof_back_one();return TRUE;}
        return FALSE;
    }
    prof_back_one();prof_close_if_dash();
    return TRUE;
}
static int prof_auto_toast;
/* 807 Edit Gamer Picture: its MetaPanelScene shows the focused button's description in Text1. */
static void prof_807_tick(void) {
    static DWORD last;
    BladesPage *top=blades_page();
    if(!top || !prof_top_is(L"807_EditGamerTile.xur")) {last=0;return;}
    if(!last) guide_text(top->scene,L"Text1",L"");   /* authored placeholder "Text 1" */
    DWORD f=xui_focused();if(f==last) return;
    last=f;const WCHAR *id=blades_id(f),*text=0;
    if(id && !lstrcmpW(id,L"changeGamerTileButton")) text=L"View available gamer pictures.";
    else if(id && !lstrcmpW(id,L"downloadTilesButton")) text=L"Download new gamer pictures from Xbox Live Marketplace.";
    if(text) guide_text(top->scene,L"Text1",text);   /* keeps the last text while a message box has focus */
}
/* hudbkgnd's HalfToFull / FullToHalf timelines key the Legend_* texts, so during and after a frame transition
   the legend shows the previous page's texts again: re-apply the top page's legend every few frames while
   the Guide is open (the texts only change when they differ). */
static void prof_legend_tick(void) {
    static int n;
    if(!blades_guide_open || ++n%8) return;
    BladesPage *page=blades_guide_stack.depth?blades_page():0;
    DWORD top=page?page->scene:guide_menu;
    if(top) guide_legend(top);
    {   /* After HalfToFull the frame can keep drawing Legend_X/Y although their Show is FALSE (seen on the
           sign-in page): give a hidden legend an empty text so it cannot name the previous page's action. */
        const GuideLegend *l=top?guide_module_legend(top):0;
        const WCHAR *ids[2]={L"Legend_X",L"Legend_Y"},*texts[2]={l?l->x:0,l?l->y:0};
        for(int i=0;l && i<2;i++) if(!texts[i]) {DWORD e=ui_find_deep(guide_scene,ids[i],0);if(e) xui_string(e,L"Text",L"");}
    }
}
static void profiles_tick(void) {
    if(kb_enter_pending) {kb_enter_pending=FALSE;if(prof_kb_up()) kb_done();}
    prof_legend_tick();
    prof_807_tick();
    if(prof_signin_pending) {int i=prof_signin_pending-1;prof_signin_pending=0;prof_signin_status(i);}
    if(prof_tile_pending) {int picture=prof_tile_pending-1;prof_tile_pending=0;if(profile_signed>=0) prof_set_picture(picture);}
    if(prof_signin_wait && !--prof_signin_wait) {profiles_sign_in(prof_signin_index,TRUE);prof_signin_index=-1;}
    if(prof_signout_wait && !--prof_signout_wait) {guide_status_hide();profiles_sign_out();}
    if(prof_auto_toast && !--prof_auto_toast && profile_signed>=0) prof_toast(L"%s\nsigned in",prof[profile_signed].name,L"signed_in.png");
    if(kb_page && !prof_kb_up()) {
        BOOL alive=FALSE;BladesStack *s=blades_stack();
        for(int i=0;i<s->depth;i++) alive|=s->pages[i].scene==kb_page;
        if(!alive) {kb_page=0;kb_mode=KB_NONE;}
    }
    prof_congrats_tick();
}
static BOOL profiles_filter(DWORD element,DWORD *message) {
    (void)element;(void)message;
    return FALSE;
}
static void profiles_guide_opened(void) {
    if(!prof_loaded) prof_load();
    prof_from_dash=FALSE;prof_flow_depth=0;
    profiles_guide_menu();
}
/* Called by ui_blade_contents once the dashboard blades are built: auto sign-in, then the blades by state. */
static BOOL prof_started;
static void profiles_dash_ready(void) {
    if(!prof_loaded) prof_load();
    if(!prof_started) {
        prof_started=TRUE;
        int autosign=console_get_int(L"AutoSignIn",0);
        if(autosign>0 && autosign<=PROF_MAX && prof[autosign-1].used) {
            profile_signed=autosign-1;log_text("Auto sign-in");log_wide(prof[profile_signed].name);prof_auto_toast=30;
        }
        console_set_int(L"SignedIn",profile_signed+1);
    }
    profiles_dash_blades();
}
static const GuideLegend *profiles_legend(DWORD scene) {
    static const GuideLegend signed_in={L"sceneMainMenu",L"Select",L"Back",L"Sign Out",L"Xbox Dashboard"};
    static const GuideLegend keyboard={L"XuiKeyboardScene",L"Select",L"Back",L"Backspace",L"Space"};
    static const GuideLegend select_back={L"profilePage",L"Select",L"Back"};
    static const GuideLegend back_only={L"profilePage",0,L"Back"};
    if(!scene) return 0;
    if(ui_find_deep(scene,L"sceneMainMenu",0) && ui_find_deep(scene,L"btnSignInLive",0)) return &signed_in;
    if(ui_find_deep(scene,L"XuiKeyboardScene",0)) return &keyboard;
    int c=prof_catalog(scene);
    if(c>=0) {
        const WCHAR *pkg=blades_scenes[c].package;
        if(!lstrcmpiW(pkg,L"signin_signin.xzp") || !lstrcmpiW(pkg,L"createprofile_cp.xzp") || !lstrcmpiW(pkg,L"gamerprofile_gp.xzp"))
            return prof_scene_is(c,L"820_GameShowcaseMe.xur")?&back_only:&select_back;
    }
    return 0;
}
