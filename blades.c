/* Host translation of the supplied Blades scene navigation.
 * Paths and meta panels come from the XUI files. Native property offsets below
 * are verified by --dump-classes on the fingerprinted PC XUI engine.
 * Console/Xbox Live operations are never reported as completed by this host.
 */
#include "blades_catalog.h"
#define BLADES_STACK_MAX 32
typedef struct {DWORD scene,focus,meta;int catalog,shown;} BladesPage;
typedef struct {BladesPage pages[BLADES_STACK_MAX];int depth;DWORD root_focus;} BladesStack;
static BladesStack blades_dash_stack,blades_guide_stack;
static DWORD blades_guide_root,blades_guide_home,blades_guide_host;
static DWORD blades_music_scene;
static DWORD blades_pending,blades_notice_root,blades_notice_back;
static DWORD blades_saved_focus;
static DWORD blades_dashboard_focus;
static int blades_pending_back;
static BOOL blades_ready,blades_guide_open;
static int blades_audit_at=-1,blades_audit_failures;
static DWORD blades_audit_scene;
static const WCHAR *blades_id(DWORD element);
static DWORD *blades_class_data(DWORD element,const WCHAR *name);
static const WCHAR *blades_nav_path(DWORD element);
static BOOL blades_is_back(DWORD element);
static int blades_resolve(const WCHAR *path,int source);
static DWORD blades_load(int index);
static BOOL blades_audit_done;
/* Guide live module (guide_live.c): message box / status page input, console notices. */
static BOOL live_key(WORD key,BOOL down);
static BOOL live_guide_toggle_blocked(void);
static void live_notice(const WCHAR *text);
static int blades_audit_routes,blades_audit_unresolved,blades_audit_handlers;
static void blades_audit_controls(DWORD element,int index,int depth) {
    if(depth>32) return;
    const WCHAR *id=blades_id(element),*path=blades_nav_path(element);
    if(path[0]) {
        blades_audit_routes++;
        int target=blades_resolve(path,index);
        if(target<0) {blades_audit_unresolved++;log_text("Unresolved authored route");log_wide(id);log_wide(path);}
    } else if((!wcsncmp(id,L"btn",3) || !wcsncmp(id,L"nav",3)) && !blades_is_back(element) && blades_class_data(element,L"XuiButton")) {
        blades_audit_handlers++;log_text("Native action control");log_wide(id);
    }
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(element,&child);
    while(child) {blades_audit_controls(child,index,depth+1);XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
}
static void blades_audit_tick(DWORD frame) {
    if(!audit_scenes || !blades_ready || frame<60 || frame%3) return;
    if(blades_audit_scene) {XUI_FN(0x7dec45,HRESULT,DWORD)(blades_audit_scene);blades_audit_scene=0;}
    if(blades_audit_at<0) blades_audit_at=0;else blades_audit_at++;
    if(blades_audit_at>=(int)BLADES_SCENE_COUNT) {blades_audit_done=TRUE;log_value("Scene audit failures",blades_audit_failures);return;}
    const BladesScene *info=&blades_scenes[blades_audit_at];
    log_value("Audit scene index",blades_audit_at);log_wide(info->package);log_wide(info->scene);
    if(!lstrcmpiW(info->scene,L"skin.xur") || !lstrcmpiW(info->scene,L"BladeSkin.xur")) {
        WCHAR uri[4096],prefix[32];lstrcpyW(uri,dash_base);
        lstrcatW(uri,info->package);lstrcatW(uri,L"#");lstrcatW(uri,info->scene);
        wsprintfW(prefix,L"audit%u.",blades_audit_at);
        HRESULT hr=XUI_FN(0x7d6909,HRESULT,const WCHAR *,const WCHAR *)(uri,prefix);
        log_value("Scene audit skin",hr);if(FAILED(hr))blades_audit_failures++;return;
    }
    blades_audit_scene=blades_load(blades_audit_at);
    if(!blades_audit_scene) {blades_audit_failures++;log_text("Scene audit load failed");return;}
    HRESULT hr=xui_add(ui_root,blades_audit_scene);
    if(SUCCEEDED(hr)) hr=xui_visuals(blades_audit_scene);
    log_value("Scene audit visuals",hr);
    if(FAILED(hr)) blades_audit_failures++;
    blades_audit_routes=blades_audit_unresolved=blades_audit_handlers=0;
    blades_audit_controls(blades_audit_scene,blades_audit_at,0);
    log_value("Scene audit routes",blades_audit_routes);
    log_value("Scene audit unresolved",blades_audit_unresolved);
    log_value("Scene audit native handlers",blades_audit_handlers);
}

static const WCHAR *blades_id(DWORD element) {
    DWORD *data=element?XUI_FN(0x7d268f,void *,DWORD,DWORD)(element,XUI_GLOBAL(0x8f1188,DWORD)):0;
    return data && data[1]?(const WCHAR *)data[1]:L"";
}
static DWORD *blades_class_data(DWORD element,const WCHAR *name) {
    DWORD cls=XUI_FN(0x7dec34,DWORD,const WCHAR *)(name);
    return cls?XUI_FN(0x7d268f,void *,DWORD,DWORD)(element,cls):0;
}
static const WCHAR *blades_nav_path(DWORD element) {
    DWORD *data=blades_class_data(element,L"XuiNavButton");
    return data && data[1]?(const WCHAR *)data[1]:L"";
}
static BOOL blades_is_back(DWORD element) {
    return blades_class_data(element,L"XuiBackButton")!=0;
}
static BOOL blades_contains(DWORD parent,DWORD element,int depth) {
    if(parent==element) return TRUE;
    if(!parent || depth>32) return FALSE;
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(parent,&child);
    while(child) {
        if(blades_contains(child,element,depth+1)) return TRUE;
        XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);
    }
    return FALSE;
}
static void blades_visible(DWORD scene,BOOL show) {
    if(!scene) return;
    xui_bool(scene,L"Show",show);
    xui_bool(scene,L"DisableFocusRecursion",!show);
}
static BladesStack *blades_stack(void) {
    return blades_guide_open?&blades_guide_stack:&blades_dash_stack;
}
static BladesPage *blades_page(void) {
    BladesStack *stack=blades_stack();
    return stack->depth?&stack->pages[stack->depth-1]:0;
}
static DWORD blades_current(void) {
    BladesPage *page=blades_page();
    if(page) return page->scene;
    return blades_guide_open?blades_guide_home:dash_root[ui_blade-1];
}
static void blades_basename(const WCHAR *path,WCHAR *name,int size) {
    const WCHAR *at=path;
    for(const WCHAR *c=path;*c;c++) if(*c==L'#' || *c==L'/' || *c==L'\\') at=c+1;
    lstrcpynW(name,at,size);
}
/* An authored relative scene name can cross packages in the original dashboard.
 * Resolve within its own package first, then accept only a unique exact match.
 */
static int blades_resolve(const WCHAR *path,int source) {
    WCHAR name[256];blades_basename(path,name,256);
    if(!name[0]) return -1;
    WCHAR *suffix=wcsrchr(name,L'.');
    if(suffix && !lstrcmpiW(suffix,L".xui")) lstrcpyW(suffix,L".xur");
    if(source>=0 && !lstrcmpiW(name,L"DeviceSelector.xur") && !lstrcmpiW(blades_scenes[source].scene,L"dashmain.xur")) {
        for(unsigned i=0;i<BLADES_SCENE_COUNT;i++)
            if(!lstrcmpiW(blades_scenes[i].package,L"dash_memory.xzp") && !lstrcmpiW(name,blades_scenes[i].scene)) return (int)i;
    }
    int found=-1;
    for(unsigned i=0;i<BLADES_SCENE_COUNT;i++) if(!lstrcmpiW(name,blades_scenes[i].scene)) {
        if(source>=0 && !lstrcmpiW(blades_scenes[source].package,blades_scenes[i].package)) return (int)i;
        if(found>=0) return -2;
        found=(int)i;
    }
    return found;
}
static int blades_catalog(const WCHAR *package,const WCHAR *scene) {
    for(unsigned i=0;i<BLADES_SCENE_COUNT;i++)
        if(!lstrcmpiW(package,blades_scenes[i].package) && !lstrcmpiW(scene,blades_scenes[i].scene)) return (int)i;
    return -1;
}
static DWORD blades_load(int index) {
    if(index<0 || index>=(int)BLADES_SCENE_COUNT) return 0;
    WCHAR file[512];wsprintfW(file,L"%s#%s",blades_scenes[index].package,blades_scenes[index].scene);
    DWORD scene=dash_scene(file);
    if(scene) {log_text("Blades scene loaded");log_wide(file);}
    return scene;
}
static int blades_source(void) {
    BladesPage *page=blades_page();
    if(page) return page->catalog;
    if(blades_guide_open) return guide_nxe?nxe_catalog():blades_catalog(L"hud_hud.xzp",profiles_menu_scene());
    if(ui_blade==4) return blades_catalog(L"dash_dashmain.xzp",L"dashmain.xur");
    return blades_catalog(dash_blades[ui_blade-1].package,dash_blades[ui_blade-1].content);
}
static BOOL blades_focus_first(DWORD scene,int depth) {
    if(depth>32) return FALSE;
    const WCHAR *id=blades_id(scene);
    BOOL legend=!wcsncmp(id,L"legend_",7) || !lstrcmpW(id,L"btnA") || !lstrcmpW(id,L"btnB") || !lstrcmpW(id,L"btnX") || !lstrcmpW(id,L"btnY") ||
        !lstrcmpW(id,L"navA") || !lstrcmpW(id,L"navB") || !lstrcmpW(id,L"navX") || !lstrcmpW(id,L"navY");
    const WCHAR *types[]={L"XuiButton",L"XuiCheckbox",L"XuiRadioButton",L"XuiList",L"XuiEdit",L"XuiSlider"};
    if(!legend) for(int i=0;i<6;i++)
        if(blades_class_data(scene,types[i]) && SUCCEEDED(xui_focus(scene))) return TRUE;
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(scene,&child);
    while(child) {if(blades_focus_first(child,depth+1))return TRUE;XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
    return FALSE;
}
static void blades_focus(DWORD scene,int index) {
    DWORD element=0;
    if(index>=0 && blades_scenes[index].focus[0]) {
        element=xui_find(scene,blades_scenes[index].focus);
        if(!element) element=ui_find_deep(scene,blades_scenes[index].focus,0);
    }
    if(!element) {
        DWORD *data=blades_class_data(scene,L"XuiScene");
        if(data && data[1]) {
            element=xui_find(scene,(const WCHAR *)data[1]);
            if(!element) element=ui_find_deep(scene,(const WCHAR *)data[1],0);
        }
    }
    if(element && SUCCEEDED(xui_focus(element))) {log_text("Authored default focus");log_wide(blades_id(element));return;}
    if(!blades_focus_first(scene,0)) ui_focus_first(scene);
}
static void blades_prepare(DWORD scene,int index) {
    if(index<0) return;
    /* These authored placeholder fields are normally populated by console
       hardware/profile callbacks. Display the actual host availability. */
    DWORD current=ui_find_deep(scene,L"labCurrentSetting",0);
    if(current) xui_string(current,L"Text",L"Console not connected");
    if(!lstrcmpiW(blades_scenes[index].package,L"dash_memory.xzp") && !lstrcmpiW(blades_scenes[index].scene,L"DeviceSelector.xur")) {
        DWORD empty=ui_find_deep(scene,L"txt_EmptyList",0);
        if(empty) xui_bool(empty,L"Show",TRUE);
        dash_set_text(scene,L"labTotal",L"");
        DWORD dots=ui_find_deep(scene,L"labDots",0);if(dots) xui_bool(dots,L"Show",FALSE);
        const WCHAR *legends[]={L"legend_x",L"legend_y",L"legend_a"};
        for(int i=0;i<3;i++) {DWORD e=ui_find_deep(scene,legends[i],0);if(e) {xui_bool(e,L"Show",FALSE);xui_bool(e,L"Enabled",FALSE);}}
    }
}
static BOOL blades_list_item(const WCHAR *list,int index,WCHAR *out,int size) {
    const WCHAR *at=list;
    for(int i=0;i<index;i++) {const WCHAR *next=wcsstr(at,L"\\0");if(!next)return FALSE;at=next+2;}
    const WCHAR *end=wcsstr(at,L"\\0");int length=end?(int)(end-at):lstrlenW(at);
    if(length>=size) length=size-1;
    memcpy(out,at,length*sizeof(WCHAR));out[length]=0;return TRUE;
}
static void blades_notice_close(void) {
    if(!blades_notice_root) return;
    XUI_FN(0x7dec45,HRESULT,DWORD)(blades_notice_root);
    blades_notice_root=blades_notice_back=0;
    if(blades_saved_focus) xui_focus(blades_saved_focus);
}
static void blades_notice(const WCHAR *reason) {
    log_text("Console notice");log_wide(reason);
    live_notice(reason);
}
/* Blades 1-3 keep their content in scContainer (dash_root): the blade scene around it (TabN: txt_Header,
   legend_a/b/x/y) is hidden too while a dash page is open, as the page authors its own header and legends. */
static void blades_tab_sync(void) {
    static DWORD tabs[4],tabs_of;
    if(tabs_of!=ui_external) {tabs_of=ui_external;for(int i=0;i<4;i++) tabs[i]=ui_external?ui_find_deep(ui_external,dash_blades[i].tab,0):0;}
    for(int i=0;i<4;i++) if(tabs[i] && tabs[i]!=dash_root[i] && i==ui_blade-1) blades_visible(tabs[i],!blades_dash_stack.depth);
}
static void blades_back(void) {
    if(blades_notice_root) {blades_notice_close();return;}
    if(guide_back()) return;
    BladesStack *stack=blades_stack();
    if(stack->depth) {
        BladesPage *page=&stack->pages[--stack->depth];
        guide_list_unbind_tree(page->scene);
        XUI_FN(0x7dec45,HRESULT,DWORD)(page->scene);
        memset(page,0,sizeof(*page));
        DWORD scene=blades_current();blades_visible(scene,TRUE);
        if(!blades_guide_open && ui_mode==2) blades_tab_sync();   /* the blade scene takes focus again */
        DWORD focus=stack->depth?stack->pages[stack->depth-1].focus:stack->root_focus;
        if(!focus || FAILED(xui_focus(focus))) blades_focus(scene,blades_source());
        if(!blades_guide_open && !stack->depth) {
            WCHAR start[32],end[32];wsprintfW(start,L"%dClose",ui_blade);wsprintfW(end,L"%sEnd",start);
            ui_play_tree(ui_external,start,end,0);
        }
        guide_page_restored();
        log_value("Blades Back depth",stack->depth);return;
    }
    if(blades_guide_open) blades_pending_back=2;
}
/* The Guide frame (hudbkgnd: panel, its pages and message boxes) is a child of the UI root and is
   created once, the first time it is needed. A sign-in notification at start-up (an auto sign-in
   profile) creates it before any dashboard page exists, so a dash page added to the root later was
   its younger sibling and drew over the opened Guide (page text and legends through the panel).
   Keep the frame the root's last scene below the notification popup (live_toast_tick raises that). */
static DWORD live_toast_scene(void);
static void blades_raise_guide(void) {
    if(!guide_scene || !ui_root) return;
    DWORD child=0,parent=0,last=0,toast=live_toast_scene();
    XUI_FN(0x7d32af,HRESULT,DWORD,DWORD *)(guide_scene,&parent);   /* XuiElementGetParent */
    if(parent!=ui_root) return;
    XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(ui_root,&child);
    while(child) {if(child!=toast) last=child;XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
    if(last==guide_scene) return;
    XUI_FN(0x7d3289,HRESULT,DWORD)(guide_scene);   /* XuiElementUnlink */
    if(FAILED(xui_add(ui_root,guide_scene))) {log_text("Guide frame could not be raised");return;}
    log_text("Guide frame kept above a dashboard page");
}
static BOOL blades_navigate(int index) {
    BladesStack *stack=blades_stack();
    if(index<0) {
        log_text(index==-2?"Navigation: scene name has more than one supplied destination":"Navigation: scene not in the converted packages");
        return FALSE;
    }
    if(stack->depth>=BLADES_STACK_MAX) {log_text("Navigation: menu history full");return FALSE;}
    DWORD scene=blades_load(index);if(!scene) {log_text("Navigation: scene could not be loaded");return FALSE;}
    DWORD parent=blades_guide_open?(nxe_page(index)?nxe_scene:blades_guide_host):ui_root;   /* NXE pages (Quick Launch) open on GuideMain */
    HRESULT hr=xui_add(parent,scene);
    if(SUCCEEDED(hr)) hr=xui_visuals(scene);
    if(FAILED(hr)) {XUI_FN(0x7dec45,HRESULT,DWORD)(scene);return FALSE;}
    if(parent==ui_root) blades_raise_guide();
    blades_prepare(scene,index);
    DWORD focus=XUI_FN(0x7d4a95,DWORD,DWORD)(ui_root);
    if(stack->depth) stack->pages[stack->depth-1].focus=focus;else stack->root_focus=focus;
    blades_visible(blades_current(),FALSE);
    BladesPage *page=&stack->pages[stack->depth++];
    *page=(BladesPage){.scene=scene,.catalog=index,.shown=-1};
    if(blades_guide_open) {guide_prepare(scene);guide_top=scene;guide_legend(scene);}
    blades_visible(scene,TRUE);blades_focus(scene,index);
    guide_page_shown(scene,index);
    if(!blades_guide_open && stack->depth==1) {
        WCHAR start[32],end[32];wsprintfW(start,L"%dOpen",ui_blade);wsprintfW(end,L"%sEnd",start);
        ui_play_tree(ui_external,start,end,0);
    }
    log_value("Blades submenu depth",stack->depth);return TRUE;
}
/* Drop the Guide page on top without animating the frame (the Guide is closed or closing). */
static void blades_guide_pop(void) {
    if(!blades_guide_stack.depth) return;
    BladesPage *page=&blades_guide_stack.pages[--blades_guide_stack.depth];
    guide_list_unbind_tree(page->scene);
    XUI_FN(0x7dec45,HRESULT,DWORD)(page->scene);
    memset(page,0,sizeof(*page));
}
static void blades_guide(BOOL open) {
    if(!blades_ready) return;
    blades_notice_close();
    if(open==blades_guide_open) return;
    if(open) {
        blades_dashboard_focus=XUI_FN(0x7d4a95,DWORD,DWORD)(ui_root);
        /* The console's Guide button always opens the Guide at its main menu: pages left open
           when it was closed are dropped (they stay on screen during the close slide). */
        while(blades_guide_stack.depth) blades_guide_pop();
        if(blades_guide_home) {blades_visible(blades_guide_home,TRUE);guide_top=blades_guide_home;}
        ui_guide_toggle();
        blades_guide_root=guide_scene;blades_guide_home=guide_menu;blades_guide_host=guide_host;
        if(!guide_open || !guide_host) return;
        profiles_guide_opened();  /* Guide main menu by sign-in state */
        blades_visible(ui_demo,FALSE);blades_visible(ui_background,FALSE);
        blades_visible(ui_external,TRUE);blades_visible(guide_scene,TRUE);
        if(!blades_music_scene) {
            /* hud.xex 913e8dd8 -> 913e94d0 loads the 913f2064 path into scnMusic. */
            DWORD holder=ui_find_deep(guide_nxe?nxe_scene:guide_menu,L"scnMusic",0);   /* NXE: the Media blade's */
            int music=blades_catalog(guide_nxe?L"hud_nxe.xzp":L"hud_hud.xzp",L"MiniMediaPlayer.xur");
            if(holder && music>=0) {
                blades_music_scene=blades_load(music);
                if(blades_music_scene) {
                    dash_attach(holder,blades_music_scene,0,0);guide_prepare(blades_music_scene);
                    dash_set_text(blades_music_scene,L"btnPlaylist",L"Select Music");  /* hud Strings.xus 28 */
                    {DWORD e=ui_find_deep(blades_music_scene,L"sliderVolume",0);if(e) xui_bool(e,L"Show",FALSE);
                     e=ui_find_deep(blades_music_scene,L"imgVolume",0);if(e) xui_bool(e,L"Show",FALSE);}
                }
            }
        }
        blades_guide_open=TRUE;ui_mode=2;
        xui_bool(ui_external,L"DisableFocusRecursion",TRUE);
        if(blades_dash_stack.depth) xui_bool(blades_dash_stack.pages[blades_dash_stack.depth-1].scene,L"DisableFocusRecursion",TRUE);
        xui_bool(guide_scene,L"DisableFocusRecursion",FALSE);
        if(nxe_scene) xui_bool(nxe_scene,L"DisableFocusRecursion",FALSE);
        blades_focus(blades_current(),blades_source());
    } else {
        if(guide_open) ui_guide_toggle();
        blades_guide_open=FALSE;ui_mode=2;
        xui_bool(guide_scene,L"DisableFocusRecursion",TRUE);
        if(nxe_scene) xui_bool(nxe_scene,L"DisableFocusRecursion",TRUE);
        xui_bool(ui_external,L"DisableFocusRecursion",FALSE);
        if(blades_dash_stack.depth) xui_bool(blades_dash_stack.pages[blades_dash_stack.depth-1].scene,L"DisableFocusRecursion",FALSE);
        if(!blades_dashboard_focus || FAILED(xui_focus(blades_dashboard_focus))) blades_focus(blades_current(),blades_source());
    }
    log_value("Xbox Guide open",open);
}
static BOOL blades_switch(int mode) {
    if(!blades_ready) return FALSE;
    if(mode==0) {if(!live_guide_toggle_blocked()) blades_guide(!blades_guide_open);return TRUE;}
    /* F2 / F3 (host keys) wait like the Guide button while a message box is up: the box keeps the focus
       and is closed only through its own buttons / B. */
    if(live_guide_toggle_blocked()) {log_value("Host switch blocked by message box",mode);return TRUE;}
    if(mode==2) {
        if(blades_guide_open) blades_guide(FALSE);
        ui_mode=2;blades_visible(ui_demo,FALSE);blades_visible(ui_background,FALSE);
        blades_visible(ui_external,TRUE);
        BladesPage *page=blades_page();if(page) blades_visible(page->scene,TRUE);
        blades_focus(blades_current(),blades_source());
        return TRUE;
    }
    if(mode==1) {
        if(blades_guide_open) blades_guide(FALSE);
        blades_visible(blades_guide_root,FALSE);
        if(blades_dash_stack.depth) blades_visible(blades_dash_stack.pages[blades_dash_stack.depth-1].scene,FALSE);
    }
    return FALSE;
}
static BOOL blades_in_submenu(void) {return blades_ready && blades_stack()->depth>0;}
static BOOL blades_key(WORD key,BOOL down) {
    if(!blades_ready || ui_mode!=2) return FALSE;
    if(live_key(key,down)) return TRUE;
    if(nxe_key(key,down)) return TRUE;   /* NXE Guide: LB/RB, Left/Right switch blades */
    if(key==VK_BACK || key==0x5801) {
        if(down) blades_pending_back=1;return TRUE;
    }
    if(blades_guide_open && (key=='Y' || key==0x5803)) {
        if(music_key_y(down)) return TRUE;   /* music picker pages: Y = Play All Music / Play Album / Play Song */
        if(down) {blades_pending_back=2;fse_to_dash=TRUE;}return TRUE;
    }
    if(blades_notice_root && (key==VK_UP || key==VK_DOWN || key==VK_LEFT || key==VK_RIGHT)) return TRUE;
    if(blades_in_submenu() && (key==VK_PRIOR || key==VK_NEXT)) return TRUE;
    return FALSE;
}
static void blades_initialize(void) {
    if(!dash_app || !ui_external) return;
    blades_ready=TRUE;
    log_text("Blades submenu navigation ready");
}
static BOOL blades_message(DWORD element,DWORD *message) {
    if(!blades_ready || message[1]!=0xe || !message[4]) return FALSE;
    DWORD *data=(DWORD *)message[4];
    if(data[0]!=1) return FALSE;
    if(blades_notice_root && blades_contains(blades_notice_root,data[1],0)) {
        blades_pending_back=1;message[2]=1;return TRUE;
    }
    if((ui_mode!=2 && ui_mode!=3) || !blades_contains(blades_current(),data[1],0)) return FALSE;
    if(guide_keep_press(data[1])) return FALSE;
    blades_pending=data[1];message[2]=1;return TRUE;
}
static void blades_dispatch(DWORD button) {
    const WCHAR *id=blades_id(button);
    if(blades_is_back(button) || !lstrcmpW(id,L"btnB") || !lstrcmpW(id,L"legend_b") || !lstrcmpW(id,L"navB")) {blades_back();return;}
    if(!lstrcmpW(id,L"btnA") || !lstrcmpW(id,L"legend_a") || !lstrcmpW(id,L"navA")) {
        DWORD focus=XUI_FN(0x7d4a95,DWORD,DWORD)(ui_root);
        if(focus && focus!=button && !guide_keep_press(focus)) blades_dispatch(focus);return;
    }
    if(guide_dispatch(button,id)) return;
    /* XamShowSigninUI / XamShowCreateProfileUI are invoked by the original blade
       handlers; their native storage/authentication work is unavailable here. */
    if(!lstrcmpW(id,L"fakeGamerCard") || !lstrcmpW(id,L"btnSignIn")) {
        blades_navigate(blades_catalog(L"signin_signin.xzp",L"signin1.xur"));return;
    }
    if(!lstrcmpW(id,L"navCreateProfile") || !lstrcmpW(id,L"btnNewProfile")) {
        int index=blades_catalog(L"createprofile_cp.xzp",L"CreateGamerProfile.xur");
        if(index>=0) blades_navigate(index);
        return;
    }
    /* Initial Setup and Media Center need console services with no console string for their
       absence here: the buttons do not react. */
    if(!lstrcmpW(id,L"navSystemSetUp") || !lstrcmpW(id,L"navMCX")) {log_text("Console service not available (no reaction)");log_wide(id);return;}
    /* dash.xex 920a5f58 chooses Memory.xzp for this authored name;
       gamer-profile device selection has a different scene with the same name. */
    if(!lstrcmpW(id,L"navMemory")) {
        blades_navigate(blades_catalog(L"dash_memory.xzp",L"DeviceSelector.xur"));return;
    }
    if(!lstrcmpW(id,L"btnY") && blades_guide_open) {
        while(blades_guide_stack.depth) blades_back();blades_guide(FALSE);return;
    }
    int source=blades_source();
    if(source>=0 && !lstrcmpiW(blades_scenes[source].scene,L"CreateGamerProfile.xur") && !lstrcmpW(id,L"nextButton")) {
        blades_notice(L"Creating a gamer profile requires console profile storage and identity services. A profile has not been created.");return;
    }
    const WCHAR *path=blades_nav_path(button);
    if(path[0]) {log_text("Authored navigation");log_wide(id);log_wide(path);blades_navigate(blades_resolve(path,source));return;}
    /* No console string exists for these (Initial Setup, Media Center, Open Tray, other console
       services): like a console with the service missing, the button does not react. */
    log_text("Original application handler required (no reaction)");log_wide(id);
}
static void blades_meta(void) {
    BladesPage *page=blades_page();if(!page || page->catalog<0) return;
    const BladesScene *info=&blades_scenes[page->catalog];
    DWORD focus=XUI_FN(0x7d4a95,DWORD,DWORD)(ui_root);
    const WCHAR *id=blades_id(focus);
    WCHAR entry[256],path[512],text[4096];int selected=-1;
    for(int i=0;i<64 && blades_list_item(info->panel_ids,i,entry,256);i++)
        if(entry[0] && !lstrcmpW(entry,id)) {selected=i;break;}
    if(selected<0 || page->shown==selected) return;
    DWORD panel=ui_find_deep(page->scene,L"metaPanelScene",0);if(!panel) return;
    blades_list_item(info->panel_paths,selected,path,512);
    blades_list_item(info->panel_texts,selected,text,4096);
    if(page->meta) {XUI_FN(0x7dec45,HRESULT,DWORD)(page->meta);page->meta=0;}
    if(path[0]) {
        int index=blades_resolve(path,page->catalog);
        page->meta=blades_load(index);
        if(page->meta) dash_attach(panel,page->meta,0,0);
    }
    if(text[0]) {
        DWORD label=ui_find_deep(page->scene,L"labDescription",0);
        if(!label) label=ui_find_deep(panel,L"labDefaultText",0);
        if(!label) label=ui_find_deep(panel,L"Pane_txt",0);
        if(label) xui_string(label,L"Text",text);
    }
    page->shown=selected;
}
/* Blade/scene timelines contain Show keyframes. Reapply scene ownership after
 * advancing timelines so a hidden parent cannot become visible behind a submenu.
 */
static void blades_visibility(void) {
    if(!blades_ready || ui_mode!=2) return;
    for(int i=0;i<4;i++) blades_visible(dash_root[i],i==ui_blade-1 && !blades_dash_stack.depth);
    blades_tab_sync();
    for(int i=0;i<blades_dash_stack.depth;i++)
        blades_visible(blades_dash_stack.pages[i].scene,i==blades_dash_stack.depth-1);
    if(blades_guide_open) {
        blades_visible(blades_guide_home,!blades_guide_stack.depth);
        for(int i=0;i<blades_guide_stack.depth;i++)
            blades_visible(blades_guide_stack.pages[i].scene,i==blades_guide_stack.depth-1);
        xui_bool(ui_external,L"DisableFocusRecursion",TRUE);
        for(int i=0;i<blades_dash_stack.depth;i++) xui_bool(blades_dash_stack.pages[i].scene,L"DisableFocusRecursion",TRUE);
    }
    nxe_visibility();
}
static void blades_tick(void) {
    if(!blades_ready) return;
    if(blades_pending_back) {int action=blades_pending_back;blades_pending_back=0;blades_pending=0;if(action==2)blades_guide(FALSE);else blades_back();}
    if(blades_pending) {DWORD button=blades_pending;blades_pending=0;blades_dispatch(button);}
    if((ui_mode==2 || ui_mode==3) && !blades_notice_root) blades_meta();
    guide_tick();
    nxe_tick();
}
static void blades_reset(void) {
    if(!blades_ready) return;
    blades_ready=FALSE;blades_pending=0;blades_pending_back=0;
    blades_notice_close();
    for(int i=0;i<blades_dash_stack.depth;i++) XUI_FN(0x7dec45,HRESULT,DWORD)(blades_dash_stack.pages[i].scene);
    memset(&blades_dash_stack,0,sizeof(blades_dash_stack));
    /* Guide pages and music are children of the Guide frame. */
    if(guide_scene) XUI_FN(0x7dec45,HRESULT,DWORD)(guide_scene);
    if(nxe_scene) {XUI_FN(0x7dec45,HRESULT,DWORD)(nxe_scene);nxe_scene=0;}   /* NXE GuideMain is the root's child */
    memset(&blades_guide_stack,0,sizeof(blades_guide_stack));
    guide_scene=guide_menu=guide_host=guide_top=0;guide_seen_count=0;guide_open=guide_at_main=FALSE;
    lstrcpyW(guide_state,L"Closed");
    blades_guide_root=blades_guide_home=blades_guide_host=blades_music_scene=0;
    blades_guide_open=FALSE;
    memset(dash_root,0,sizeof(dash_root));memset(dash_panel,0,sizeof(dash_panel));memset(dash_meta,0,sizeof(dash_meta));
    dash_app=0;ui_blade_root=0;
}
