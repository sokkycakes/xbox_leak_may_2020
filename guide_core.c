/* Guide services shared by the Guide pages (profiles, settings, music, message boxes).
 * Included at the end of ui.c, after blades.c. Engine message ids and addresses are the
 * xlive 1.2.241.0 ones verified in the cloud probes (see README "Xbox Guide").
 *
 * Hooks into the host:
 *   ui_message_filter -> guide_filter()   first; answers list data queries, routes notifies
 *   blades_message    -> guide_keep_press() presses on checkboxes/radios/sliders/lists stay native
 *   blades_dispatch   -> guide_dispatch()  feature buttons before the authored-path fallback
 *   blades_back       -> guide_back()      a feature can consume B (volume mode, message box)
 *   blades_navigate   -> guide_page_shown() after a page is pushed and prepared
 *   blades_tick       -> guide_tick()
 */

/* ---------- engine helpers ---------- */
static HRESULT xui_send(DWORD e,DWORD id,void *data,DWORD size) {
    DWORD m[5]={20,id,0,size,(DWORD)data};
    HRESULT hr=XUI_FN(0x7dbd06,HRESULT,DWORD,void *)(e,m);
    return SUCCEEDED(hr) && !m[2]?S_FALSE:hr;
}
static HRESULT xui_get(DWORD e,const WCHAR *name,XuiValue *v) {
    DWORD id=0;memset(v,0,sizeof *v);
    HRESULT hr=XUI_FN(0x7d83a1,HRESULT,DWORD,const WCHAR *,DWORD *)(e,name,&id);
    return FAILED(hr)?hr:XUI_FN(0x7d85ce,HRESULT,DWORD,DWORD,DWORD,XuiValue *)(e,id,0,v);
}
static void xui_value_free(XuiValue *v) {XUI_FN(0x7d6f2e,void,XuiValue *)(v);}
static HRESULT xui_int(DWORD e,const WCHAR *name,int n) {
    XuiValue v={.type=2,.integer=(DWORD)n};return xui_property(e,name,&v);
}
static int xui_get_int(DWORD e,const WCHAR *name,int fallback) {
    XuiValue v;if(FAILED(xui_get(e,name,&v))) return fallback;
    int n=(int)v.integer;xui_value_free(&v);return n;
}
static BOOL xui_is(DWORD e,const WCHAR *cls) {
    DWORD c=XUI_FN(0x7dec34,DWORD,const WCHAR *)(cls);
    return e && c && XUI_FN(0x7d876c,DWORD,DWORD,DWORD)(e,c);
}
static BOOL xui_checked(DWORD e) {DWORD d[1]={0};xui_send(e,0x7d0,d,sizeof d);return d[0]!=0;}
static void xui_set_checked(DWORD e,BOOL on) {DWORD d[1]={(DWORD)on};xui_send(e,0x7d1,d,sizeof d);}
/* Radio groups and lists share the current-selection messages. */
static int xui_cursel(DWORD e) {DWORD d[2]={(DWORD)-1,0};xui_send(e,0x7d7,d,sizeof d);return (int)d[0];}
static void xui_set_cursel(DWORD e,int i) {DWORD d[1]={(DWORD)i};xui_send(e,0x7d8,d,sizeof d);}
static void xui_list_refresh(DWORD list) {xui_send(list,0x7dd,0,0);}
static DWORD xui_focused(void) {return XUI_FN(0x7d4a95,DWORD,DWORD)(ui_root);}
/* Ids repeat across loaded scenes (two sliderVolume), so search from a page, not the frame. */
static DWORD guide_find(DWORD scene,const WCHAR *id) {return scene?ui_find_deep(scene,id,0):0;}
static void guide_show(DWORD scene,const WCHAR *id,BOOL show) {DWORD e=guide_find(scene,id);if(e) xui_bool(e,L"Show",show);}
static void guide_text(DWORD scene,const WCHAR *id,const WCHAR *text) {DWORD e=guide_find(scene,id);if(e) xui_string(e,L"Text",text);}
/* Package resource URI, e.g. guide_uri(buf,L"hud_hud.xzp",L"mediaPause.png"). */
static const WCHAR *guide_uri(WCHAR *out,const WCHAR *package,const WCHAR *file) {
    wsprintfW(out,L"%s%s#%s",dash_base,package,file);return out;
}
/* Scene id of a page: the id of the scene element under the canvas root. */
static const WCHAR *guide_page_id(DWORD page) {
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(page,&child);
    const WCHAR *id=blades_id(page);
    while(child) {
        if(xui_is(child,L"XuiScene")) return blades_id(child);
        XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);
    }
    return id;
}
static BOOL guide_page_is(DWORD page,const WCHAR *scene_id) {return page && ui_find_deep(page,scene_id,0)!=0;}

/* ---------- storage (profiles\*.ini beside the exe) ---------- */
static WCHAR store_dir[MAX_PATH];
static int profile_signed=-1;   /* index of the signed-in local profile, -1 when signed out */
static const WCHAR *store_folder(void) {
    if(!store_dir[0]) {
        GetModuleFileNameW(0,store_dir,MAX_PATH);
        WCHAR *slash=wcsrchr(store_dir,L'\\');if(slash) slash[1]=0;
        lstrcatW(store_dir,L"profiles\\");
        CreateDirectoryW(store_dir,0);
    }
    return store_dir;
}
/* UTF-16 ini files: create with a BOM so the profile API keeps Unicode. */
static void store_file(WCHAR *path,const WCHAR *name) {
    lstrcpyW(path,store_folder());lstrcatW(path,name);
    if(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES) {
        HANDLE f=CreateFileW(path,GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
        if(f!=INVALID_HANDLE_VALUE) {DWORD n;WriteFile(f,"\xff\xfe",2,&n,0);CloseHandle(f);}
    }
}
static void store_profile_file(WCHAR *path,int index) {WCHAR name[32];wsprintfW(name,L"%d.ini",index+1);store_file(path,name);}
static int ini_get_int(const WCHAR *file,const WCHAR *section,const WCHAR *key,int fallback) {
    WCHAR path[MAX_PATH];store_file(path,file);return (int)GetPrivateProfileIntW(section,key,fallback,path);
}
static void ini_set_int(const WCHAR *file,const WCHAR *section,const WCHAR *key,int value) {
    WCHAR path[MAX_PATH],text[16];store_file(path,file);wsprintfW(text,L"%d",value);WritePrivateProfileStringW(section,key,text,path);
}
static void ini_get_str(const WCHAR *file,const WCHAR *section,const WCHAR *key,const WCHAR *fallback,WCHAR *out,int size) {
    WCHAR path[MAX_PATH];store_file(path,file);GetPrivateProfileStringW(section,key,fallback,out,size,path);
}
static void ini_set_str(const WCHAR *file,const WCHAR *section,const WCHAR *key,const WCHAR *value) {
    WCHAR path[MAX_PATH];store_file(path,file);WritePrivateProfileStringW(section,key,value,path);
}
/* Console-wide settings: profiles\console.ini [Console]. */
static int console_get_int(const WCHAR *key,int fallback) {return ini_get_int(L"console.ini",L"Console",key,fallback);}
static void console_set_int(const WCHAR *key,int value) {ini_set_int(L"console.ini",L"Console",key,value);}
/* Gamer-profile settings follow the signed-in profile; signed out they are console settings
   (the Blades Guide offers Personal Settings in both states). */
static int setting_get_int(const WCHAR *key,int fallback) {
    if(profile_signed<0) return console_get_int(key,fallback);
    WCHAR name[32];wsprintfW(name,L"%d.ini",profile_signed+1);return ini_get_int(name,L"Profile",key,fallback);
}
static void setting_set_int(const WCHAR *key,int value) {
    if(profile_signed<0) {console_set_int(key,value);return;}
    WCHAR name[32];wsprintfW(name,L"%d.ini",profile_signed+1);ini_set_int(name,L"Profile",key,value);
}

/* ---------- list data sources ---------- */
typedef struct {
    DWORD list;int (*count)(void *ctx);const WCHAR *(*text)(void *ctx,int i);const WCHAR *(*image)(void *ctx,int i);void *ctx;
} GuideListSource;
static GuideListSource guide_lists[16];
/* The list asks itself for its count/texts every frame: answers must stay valid (static storage). */
static void guide_list_bind(DWORD list,int (*count)(void *),const WCHAR *(*text)(void *,int),const WCHAR *(*image)(void *,int),void *ctx) {
    if(!list) return;
    int slot=-1;
    for(int i=0;i<16;i++) if(guide_lists[i].list==list) slot=i;
    for(int i=0;i<16 && slot<0;i++) if(!guide_lists[i].list) slot=i;
    if(slot<0) slot=15;
    guide_lists[slot]=(GuideListSource){list,count,text,image,ctx};
    xui_bool(list,L"Show",TRUE);
    xui_list_refresh(list);
}
static void guide_list_unbind_tree(DWORD root) {
    for(int i=0;i<16;i++) if(guide_lists[i].list && blades_contains(root,guide_lists[i].list,0)) memset(&guide_lists[i],0,sizeof guide_lists[i]);
}
static BOOL guide_list_answer(DWORD element,DWORD *message) {
    if(message[1]!=0x7dc && message[1]!=0x7df && message[1]!=0x7e1) return FALSE;
    for(int i=0;i<16;i++) {
        GuideListSource *s=&guide_lists[i];
        if(!s->list || s->list!=element) continue;
        DWORD *d=(DWORD *)message[4];if(!d) return FALSE;
        int n=s->count?s->count(s->ctx):0;
        if(message[1]==0x7dc) {if(d[0]!=0) return FALSE;d[1]=(DWORD)n;message[2]=1;return TRUE;}
        if((int)d[0]<0 || (int)d[0]>=n) return FALSE;
        const WCHAR *t=message[1]==0x7df?(s->text?s->text(s->ctx,(int)d[0]):0):(s->image?s->image(s->ctx,(int)d[0]):0);
        if(!t) return FALSE;
        d[3]=(DWORD)t;message[2]=1;return TRUE;
    }
    return FALSE;
}

/* ---------- Guide frame states (hudbkgnd: Closed / Half / Full / Error) ---------- */
/* Scenes 855 or more units wide use the Full Guide (the Guide's own pages are Half). */
static const WCHAR *guide_full_scenes[]={
    L"InfoUpsellLive.xur",L"signin1.xur",L"signin2.xur",L"signin4.xur",L"CreateGamerProfile.xur",L"Keyboard.xur",
    L"Congratulations.xur",L"AddXboxLive.xur",L"DownloadLiveSignup.xur",L"GamerCardScene.xur",L"807_EditGamerTile.xur",
    L"808_editProfile.xur",L"809_ChangeGamerTile.xur",L"579_SelectMediaSource.xur",L"570_MusicLibrary.xur",
    L"572_SelectMediaContainer.xur",L"576_SelectSong.xur",L"FileView.xur",L"BeginPlayback.xur",0};
static const WCHAR *guide_state_for(int catalog) {
    if(catalog<0) return L"Half";
    for(int i=0;guide_full_scenes[i];i++) if(!lstrcmpiW(blades_scenes[catalog].scene,guide_full_scenes[i])) return L"Full";
    return L"Half";
}
static void guide_go(const WCHAR *state) {
    if(!guide_scene || !lstrcmpW(guide_state,state) || !lstrcmpW(guide_state,L"Closed")) return;
    guide_play(state);
}

/* ---------- message boxes ---------- */
typedef void (*GuideChoice)(int choice,void *ctx);
static void guide_msgbox_impl(const WCHAR *title,const WCHAR *body,const WCHAR **buttons,int count,int focus,GuideChoice done,void *ctx);
/* Console message box (hud skin XuiMessageBox2..4 in the Guide's Error panel).
   buttons: 1..3 labels; done(choice) gets the button index, or -1 for B. */
static void guide_msgbox(const WCHAR *title,const WCHAR *body,const WCHAR **buttons,int count,int focus,GuideChoice done,void *ctx) {
    log_text("Guide message box");log_wide(title);
    guide_msgbox_impl(title,body,buttons,count,focus,done,ctx);
}

static DWORD guide_last_notify[3];
/* ---------- feature modules ---------- */
/* Each module may implement these; the core calls them in order. Return TRUE when handled. */
static BOOL profiles_dispatch(DWORD button,const WCHAR *id);
static BOOL settings_dispatch(DWORD button,const WCHAR *id);
static BOOL music_dispatch(DWORD button,const WCHAR *id);
static BOOL live_dispatch(DWORD button,const WCHAR *id);
static void profiles_notify(DWORD src,DWORD code,DWORD *data);
static void settings_notify(DWORD src,DWORD code,DWORD *data);
static void music_notify(DWORD src,DWORD code,DWORD *data);
static void live_notify(DWORD src,DWORD code,DWORD *data);
static void profiles_page(DWORD page,int catalog);
static void settings_page(DWORD page,int catalog);
static void music_page(DWORD page,int catalog);
static void live_page(DWORD page,int catalog);
static BOOL profiles_back(void);
static BOOL settings_back(void);
static BOOL music_back(void);
static BOOL live_back(void);
static void profiles_tick(void);
static void settings_tick(void);
static void music_tick(void);
static void live_tick(void);
static BOOL profiles_filter(DWORD element,DWORD *message);
static BOOL music_filter(DWORD element,DWORD *message);
static void profiles_guide_opened(void);
static void music_guide_opened(void);

static const GuideLegend *profiles_legend(DWORD scene);
static const GuideLegend *settings_legend(DWORD scene);
static const GuideLegend *music_legend(DWORD scene);
static const GuideLegend *live_legend(DWORD scene);
/* Cross-module services (implemented by the live module). */
static void live_join(void);          /* Join Xbox Live: console's own connect attempt + can't-connect result */
static void live_recover(void);       /* Recover Gamertag from Xbox Live */
static void guide_toast(const WCHAR *text,const WCHAR *image);   /* xam notify.xur popup; honours Notifications settings */
static void guide_status_show(const WCHAR *text);                /* hud Status.xur ("Signing out") */
static void guide_status_hide(void);
static const GuideLegend *guide_module_legend(DWORD scene) {
    const GuideLegend *l=live_legend(scene);
    if(!l) l=profiles_legend(scene);
    if(!l) l=settings_legend(scene);
    if(!l) l=music_legend(scene);
    return l;
}
static BOOL guide_dispatch(DWORD button,const WCHAR *id) {
    return live_dispatch(button,id) || profiles_dispatch(button,id) || settings_dispatch(button,id) || music_dispatch(button,id);
}
static BOOL guide_back(void) {return live_back() || music_back() || profiles_back() || settings_back();}
static void guide_page_shown(DWORD page,int catalog) {
    if(blades_guide_open) guide_go(guide_state_for(catalog));
    profiles_page(page,catalog);settings_page(page,catalog);music_page(page,catalog);live_page(page,catalog);
}
/* A page was popped: restore the frame state of the one now on top. */
static void guide_page_restored(void) {
    if(!blades_guide_open) return;
    BladesPage *page=blades_page();
    guide_go(guide_state_for(page?page->catalog:-1));
}
static void guide_tick(void) {memset(guide_last_notify,0,sizeof guide_last_notify);profiles_tick();settings_tick();music_tick();live_tick();}

/* Native controls keep their own press handling: the engine toggles a checkbox/radio or
   moves a list before the press notify, so the host must not turn those into actions. */
static BOOL guide_keep_press(DWORD src) {
    return xui_is(src,L"XuiCheckbox") || xui_is(src,L"XuiRadioButton") || xui_is(src,L"XuiRadioGroup") ||
           xui_is(src,L"XuiSlider") || xui_is(src,L"XuiListItem") || xui_is(src,L"XuiList") || xui_is(src,L"XuiEdit");
}
/* Notifies bubble to every parent and the filter runs once per class level: deliver once. */
static BOOL guide_filter(DWORD element,DWORD *message) {
    if(guide_list_answer(element,message)) return TRUE;
    if(profiles_filter(element,message) || music_filter(element,message)) return TRUE;
    if(message[1]==0xe && message[4]) {
        DWORD *n=(DWORD *)message[4];
        if(guide_last_notify[0]==(DWORD)message && guide_last_notify[1]==n[0] && guide_last_notify[2]==n[1]) return FALSE;
        guide_last_notify[0]=(DWORD)message;guide_last_notify[1]=n[0];guide_last_notify[2]=n[1];
        if(n[0]==1 || n[0]==4 || n[0]==0xB || n[0]==2) {
            profiles_notify(n[1],n[0],n);settings_notify(n[1],n[0],n);music_notify(n[1],n[0],n);live_notify(n[1],n[0],n);
        }
    }
    return FALSE;
}
