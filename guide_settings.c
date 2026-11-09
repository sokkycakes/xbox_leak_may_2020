/* Guide module: settings.
 * Personal Settings (hud_hud.xzp Options*.xur), Themes / Customize (SkinSelect, OptionsPersonalization,
 * DashStyleSelect, TileSelect, ColorSelect), Shut Down, the main menu battery icon and the
 * frame's ring of light. Values are gamer-profile settings (setting_get_int/setting_set_int:
 * the signed-in profile's ini, console.ini when signed out) and are re-applied at startup and
 * whenever the signed-in profile changes.
 */

/* ---------- stored settings (keys and console defaults) ---------- */
#define SETTING_ONLINE      L"OnlineState"        /* radgrpOnlineState: 0 Online, 1 Away, 2 Busy, 3 Appear Offline */
#define SETTING_VOLUME      L"VoiceVolume"        /* sliderVolume 0..10, authored Value 10 */
#define SETTING_MUTE        L"VoiceMute"          /* chkMute */
#define SETTING_OUTPUT      L"VoiceOutput"        /* radgrpOutputLocation: 0 Headset, 1 TV, 2 Both */
#define SETTING_VIBRATION   L"Vibration"          /* chkVibration, console default on */
#define SETTING_SHOW        L"ShowNotifications"  /* chkShow (read by guide_toast) */
#define SETTING_SOUND       L"NotificationSound"  /* chkSound (read by guide_toast) */
#define SETTING_THEME       L"Theme"              /* SkinsList index, -1 = customized */
#define SETTING_TRIM        L"DashTrim"           /* StyleList index */
#define SETTING_TILE        L"GuideBackground"    /* TileList index (last = Custom Color) */
#define SETTING_COLOR       L"GuideColor"         /* 0xRRGGBB of the custom Guide colour */

static BOOL settings_vibration_enabled(void) {return setting_get_int(SETTING_VIBRATION,1)!=0;}

/* ---------- theme data (hud Strings.xus names; art from the packages) ---------- */
/* SkinsList: the built-in themes. A theme is a dashboard trim (dash_dashskn*.xzp BladeSkin.xur)
   plus a Guide background; "Customize" opens OptionsPersonalization. */
static const WCHAR *settings_themes[]={L"Xbox 360 (Default)",L"Carbon",L"Glass",L"Customize"};
static const int settings_theme_trim[]={0,1,2},settings_theme_tile[]={0,2,0};
#define SETTINGS_THEME_COUNT 4
#define SETTINGS_THEME_CUSTOMIZE 3
/* StyleList: Default = the dashboard skin's own blade visuals (dash_dashuisk.xzp), Carbon/Glass =
   dash_dashskn1/2.xzp BladeSkin.xur (dark grey / blue-white blade gradients). */
static const WCHAR *settings_trims[]={L"Default Trim",L"Carbon Trim",L"Glass Trim"};
static const WCHAR *settings_trim_packages[]={0,L"dash_dashskn1.xzp",L"dash_dashskn2.xzp"};
#define SETTINGS_TRIM_COUNT 3
/* TileList: Default = HUD_background's own glass.png (huduiskin_skin.xzp); the tiles are the
   hud_hud.xzp textures in package order, named with the hud strings that match their art
   (Bubbles.png = Bubblegum, Sahara.png = Stone, H2O.png = Dew, Fiber.png = Tangerine).
   "Smoke" (hud string 1) has no texture in any package and is not listed. */
static const WCHAR *settings_tiles[]={L"Default",L"Pearl",L"Carbon",L"Bubblegum",L"Stone",L"Dew",L"Tangerine",L"Wood",L"Custom Color"};
static const WCHAR *settings_tile_files[][2]={
    {L"huduiskin_skin.xzp",L"glass.png"},{L"hud_hud.xzp",L"Pearl.png"},{L"hud_hud.xzp",L"Carbon.png"},
    {L"hud_hud.xzp",L"Bubbles.png"},{L"hud_hud.xzp",L"Sahara.png"},{L"hud_hud.xzp",L"H2O.png"},
    {L"hud_hud.xzp",L"Fiber.png"},{L"hud_hud.xzp",L"Wood.png"}};
#define SETTINGS_TILE_COUNT 9
#define SETTINGS_TILE_CUSTOM 8
#define SETTINGS_COLOR_DEFAULT 0x3c3c3c   /* ColorSelect sliders are authored at 60,60,60 */

static int settings_list_count(void *ctx) {return (int)(INT_PTR)ctx;}
static const WCHAR *settings_theme_text(void *ctx,int i) {return settings_themes[i];}
static const WCHAR *settings_trim_text(void *ctx,int i) {return settings_trims[i];}
static const WCHAR *settings_tile_text(void *ctx,int i) {return settings_tiles[i];}

/* ---------- state ---------- */
static DWORD settings_page_scene;    /* the settings page on top (one of the scenes below) */
static int settings_page_kind;
enum {SP_NONE,SP_ONLINE,SP_VOICE,SP_CONTROLLER,SP_NOTIFY,SP_SKINS,SP_CUSTOMIZE,SP_TRIM,SP_TILE,SP_COLOR};
static BOOL settings_loading;        /* our own control writes: do not store them back */
static int settings_nav_pending=-1;  /* catalog index to push next tick (never push inside a notify) */
static int settings_applied_profile=-2;
static DWORD settings_applied_guide,settings_applied_dash;
static int settings_trim_now=-1;
static BOOL settings_trim_loaded[SETTINGS_TRIM_COUNT];
static DWORD settings_frame;

/* ---------- Guide background: HUD_background's 'background' figure fill ---------- */
/* XuiFigure class data: Fill is an inline object at +0x14 (FillColor +0xc, TextureFileName +0x10,
   FillType +0x88); its native sub-property setters (native-classes.log) take (handle, fill, value). */
static void settings_fill(DWORD figure,const WCHAR *texture,DWORD color) {
    BYTE *data=figure?(BYTE *)blades_class_data(figure,L"XuiFigure"):0;
    if(!data) return;
    BYTE *fill=data+0x14;
    XuiValue v;
    if(texture) {
        v=(XuiValue){.type=5,.text=texture};XUI_FN(0x7d9700,HRESULT,DWORD,void *,XuiValue *)(figure,fill,&v);
        /* The type setter marks the brush dirty only when the type changes. */
        v=(XuiValue){.type=3,.integer=1};XUI_FN(0x7d9754,HRESULT,DWORD,void *,XuiValue *)(figure,fill,&v);
        v=(XuiValue){.type=3,.integer=4};XUI_FN(0x7d9754,HRESULT,DWORD,void *,XuiValue *)(figure,fill,&v);
    } else {
        v=(XuiValue){.type=6,.integer=0xff000000|color};XUI_FN(0x7d7759,HRESULT,DWORD,void *,XuiValue *)(figure,fill,&v);
        v=(XuiValue){.type=3,.integer=4};XUI_FN(0x7d9754,HRESULT,DWORD,void *,XuiValue *)(figure,fill,&v);
        v=(XuiValue){.type=3,.integer=1};XUI_FN(0x7d9754,HRESULT,DWORD,void *,XuiValue *)(figure,fill,&v);
    }
    if(diagnostics) {char line[128];wsprintfA(line,"Settings: fill %08X type %u colour %08X",figure,*(DWORD *)(fill+0x88),*(DWORD *)(fill+0xc));log_text(line);}
}
static void settings_fill_tile(DWORD figure,int tile,DWORD color) {
    static WCHAR uri[SETTINGS_TILE_CUSTOM][1024];
    if(tile<0 || tile>SETTINGS_TILE_CUSTOM) tile=0;
    if(tile==SETTINGS_TILE_CUSTOM) {settings_fill(figure,0,color&0xffffff);return;}
    if(!uri[tile][0]) guide_uri(uri[tile],settings_tile_files[tile][0],settings_tile_files[tile][1]);
    settings_fill(figure,uri[tile],0);
}
/* The fill lives in the visual instance, which the engine recreates when visuals are refreshed
   (device reset, xui_visuals): remember the figures and re-apply when they change. */
static DWORD settings_bg_figure[2];
static void settings_apply_background_to(BOOL force) {
    if(!guide_scene) return;
    int tile=setting_get_int(SETTING_TILE,0);DWORD color=(DWORD)setting_get_int(SETTING_COLOR,SETTINGS_COLOR_DEFAULT)&0xffffff;
    const WCHAR *bases[]={L"graphic-base",L"graphic-base-error"};
    BOOL applied=FALSE;
    for(int i=0;i<2;i++) {
        DWORD base=ui_find_deep(guide_scene,bases[i],0);
        DWORD figure=base?ui_find_deep(base,L"background",0):0;
        if(figure && (force || figure!=settings_bg_figure[i])) {settings_fill_tile(figure,tile,color);applied=TRUE;}
        settings_bg_figure[i]=figure;
    }
    if(applied) {char line[96];wsprintfA(line,"Settings: Guide background %d colour %06X",tile,color);log_text(line);}
}
static void settings_apply_background(void) {settings_apply_background_to(TRUE);}

/* ---------- dashboard trim: BladeSkin.xur visuals under a prefix ---------- */
/* The trim skins redefine the blade visuals of dash_dashuisk.xzp (blade_*_jewel, wing, ...).
   Each is loaded once under the prefix "trimN." and the dashboard's elements are rebound to the
   prefixed copy (the same way the Guide uses the hud skin), or back to the base visual. */
static int settings_rebound;
/* Visuals defined by dash_dashskn1/2.xzp BladeSkin.xur (XuiScene, also defined there, is the same
   transparent default and is left alone). */
static const WCHAR *settings_trim_visuals[]={L"BG_animation",L"WingCover",L"blad_top_back",L"blade_1_bgcolor",L"blade_1_highlight",
    L"blade_1_jewel",L"blade_1_txt_color",L"blade_2_bgcolor",L"blade_2_highlight",L"blade_2_jewel",L"blade_2_txt_color",
    L"blade_3_bgcolor",L"blade_3_highlight",L"blade_3_jewel",L"blade_3_txt_color",L"blade_4_bgcolor",L"blade_4_highlight",
    L"blade_4_jewel",L"blade_4_txt_color",L"blade_grey_left",L"blade_grey_rt",L"blade_left_bend_tween",L"blade_rt_bend_tween",
    L"blade_straight_tween",L"blade_top_face",L"blade_txt_grey",L"content_panel",L"wing",0};
static void settings_trim_rebind(DWORD e,int trim,int depth) {
    if(!e || depth>40) return;
    DWORD visual=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(e,&visual);
    if(visual && !xui_is(visual,L"XuiVisual")) visual=0;
    /* The visual instance keeps the bare visual name as its id (no skin prefix). */
    const WCHAR *name=visual?ui_element_id(visual):0;
    BOOL trimmed=FALSE;
    for(int i=0;name && settings_trim_visuals[i];i++) if(!lstrcmpW(name,settings_trim_visuals[i])) trimmed=TRUE;
    if(trimmed) {
        WCHAR want[128];
        if(trim) wsprintfW(want,L"trim%d.%s",trim,name);else lstrcpynW(want,name,128);
        if(SUCCEEDED(xui_string(e,L"Visual",want))) settings_rebound++;
        else if(diagnostics) {char line[200];wsprintfA(line,"Settings: trim rebind failed %ls -> %ls",blades_id(e),want);log_text(line);}
        XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(e,&visual);
    }
    DWORD child=0;XUI_FN(0x7d32eb,HRESULT,DWORD,DWORD *)(e,&child);
    while(child) {if(child!=visual) settings_trim_rebind(child,trim,depth+1);XUI_FN(0x7d3363,HRESULT,DWORD,DWORD *)(child,&child);}
}
static void settings_apply_trim(void) {
    if(!dash_app || !ui_external) return;
    int trim=setting_get_int(SETTING_TRIM,0);
    if(trim<0 || trim>=SETTINGS_TRIM_COUNT) trim=0;
    if(trim && !settings_trim_loaded[trim]) {
        WCHAR uri[4096],prefix[16];
        wsprintfW(uri,L"%s%s#BladeSkin.xur",dash_base,settings_trim_packages[trim]);wsprintfW(prefix,L"trim%d.",trim);
        HRESULT hr=XUI_FN(0x7d6909,HRESULT,const WCHAR *,const WCHAR *)(uri,prefix);
        log_wide(uri);log_value("Settings: trim skin",hr);
        if(FAILED(hr)) return;
        settings_trim_loaded[trim]=TRUE;
    }
    if(trim==settings_trim_now) return;
    settings_rebound=0;
    settings_trim_rebind(ui_external,trim,0);
    for(int i=0;i<blades_dash_stack.depth;i++) settings_trim_rebind(blades_dash_stack.pages[i].scene,trim,0);
    settings_trim_now=trim;
    log_value("Settings: dashboard trim",trim);log_value("Settings: trim visuals rebound",settings_rebound);
}

/* ---------- controller: rumble, battery, ring of light ---------- */
static SDL_Gamepad *settings_pad(void) {for(int i=0;i<4;i++) if(ui_pads[i]) return ui_pads[i];return 0;}
static void settings_rumble(void) {
    SDL_Gamepad *pad=settings_pad();
    if(!pad) {log_text("Settings: vibration on (no controller open to rumble)");return;}
    log_value("Settings: vibration rumble",SDL_RumbleGamepad(pad,0xc000,0xc000,300));
}
/* imgBatteryIcon frames Little/Low/Medium/High switch imgBatteryState between ico_32x_Battery1..4.
   The 360 Guide shows the meter for a battery-powered (wireless) controller; a wired pad or no
   pad has no battery, so the icon is hidden. Probe override: XUI_BATTERY=<percent>. */
static int settings_battery_shown=-2;
static void settings_battery(DWORD menu) {
    if(!menu) return;
    DWORD icon=ui_find_deep(menu,L"imgBatteryIcon",0);if(!icon) return;
    int percent=-1,level=-1;SDL_PowerState state=SDL_POWERSTATE_NO_BATTERY;
    SDL_Gamepad *pad=settings_pad();
    if(pad) state=SDL_GetGamepadPowerInfo(pad,&percent);
    char probe[16];
    if(GetEnvironmentVariableA("XUI_BATTERY",probe,sizeof probe)) {state=SDL_POWERSTATE_ON_BATTERY;percent=atoi(probe);}
    if(state==SDL_POWERSTATE_ON_BATTERY || state==SDL_POWERSTATE_CHARGING || state==SDL_POWERSTATE_CHARGED)
        /* SDL reports XInput battery levels empty/low/medium/full as 10/40/70/100 percent. */
        level=state==SDL_POWERSTATE_CHARGED || percent<0?3:percent<=10?0:percent<=40?1:percent<=70?2:3;
    xui_bool(icon,L"Show",level>=0);
    if(level>=0) {
        static const WCHAR *frames[]={L"Little",L"Low",L"Medium",L"High"};
        int f=-1;XUI_FN(0x7d389d,HRESULT,DWORD,const WCHAR *,int *)(icon,frames[level],&f);
        if(f>=0) XUI_FN(0x7d3867,HRESULT,DWORD,int,int,int,BOOL,BOOL)(icon,f,f,f,FALSE,FALSE);
    }
    if(level!=settings_battery_shown) {
        char line[96];wsprintfA(line,"Settings: battery state %d percent %d level %d",(int)state,percent,level);log_text(line);
        settings_battery_shown=level;
    }
}
/* hudbkgnd ROL (ringOfLight_Group): four lit quadrants light1..light4 (top-left, top-right,
   bottom-left, bottom-right). The Guide shows the quadrant of the controller that opened it;
   the host is player 1, so only light1 stays lit. */
static DWORD settings_rol_light;
static void settings_ring_of_light(BOOL force) {
    DWORD rol=guide_scene?ui_find_deep(guide_scene,L"ROL",0):0;if(!rol) return;
    DWORD probe=ui_find_deep(rol,L"light2",0);
    if(!force && probe==settings_rol_light) return;
    settings_rol_light=probe;
    const WCHAR *lights[]={L"light1",L"light2",L"light3",L"light4"};
    for(int i=0;i<4;i++) {DWORD e=ui_find_deep(rol,lights[i],0);if(e) xui_bool(e,L"Show",i==0);}
}

/* ---------- apply everything (startup, Guide created, profile change) ---------- */
static void settings_apply_all(void) {
    if(guide_scene && settings_applied_guide!=guide_scene) {settings_applied_guide=guide_scene;settings_ring_of_light(TRUE);settings_apply_background();}
    else if(guide_scene && settings_frame%10==0) {settings_ring_of_light(FALSE);settings_apply_background_to(FALSE);}
    if(dash_app && ui_external && settings_applied_dash!=ui_external) {settings_applied_dash=ui_external;settings_trim_now=-1;settings_apply_trim();}
}

/* ---------- pages ---------- */
static DWORD settings_find(const WCHAR *id) {return guide_find(settings_page_scene,id);}
static void settings_set_check(const WCHAR *id,const WCHAR *key,int fallback) {
    DWORD e=settings_find(id);if(e) xui_set_checked(e,setting_get_int(key,fallback)!=0);
}
static void settings_set_radio(const WCHAR *id,const WCHAR *key,int fallback,int count) {
    DWORD e=settings_find(id);int v=setting_get_int(key,fallback);
    if(v<0 || v>=count) v=fallback;
    if(e) xui_set_cursel(e,v);
}
static DWORD settings_slider_color(void) {
    DWORD r=settings_find(L"sliderRed"),g=settings_find(L"sliderGreen"),b=settings_find(L"sliderBlue");
    return (xui_get_int(r,L"Value",60)&255)<<16|(xui_get_int(g,L"Value",60)&255)<<8|(xui_get_int(b,L"Value",60)&255);
}
static void settings_page(DWORD page,int catalog) {
    static const struct {const WCHAR *scene;int kind;} kinds[]={
        {L"scnOptionsOnline",SP_ONLINE},{L"scnOptionsVoice",SP_VOICE},{L"scnOptionsController",SP_CONTROLLER},
        {L"scnNotificationsOnline",SP_NOTIFY},{L"scnSkinSelect",SP_SKINS},{L"scnOptionsPersonalization",SP_CUSTOMIZE},
        {L"scnDashStyle",SP_TRIM},{L"scnTileSelect",SP_TILE},{L"scnColorSelect",SP_COLOR}};
    settings_page_scene=0;settings_page_kind=SP_NONE;
    for(int i=0;i<(int)(sizeof kinds/sizeof *kinds);i++) if(guide_page_is(page,kinds[i].scene)) {settings_page_scene=page;settings_page_kind=kinds[i].kind;}
    if(!settings_page_scene) return;
    settings_loading=TRUE;
    switch(settings_page_kind) {
    case SP_ONLINE:settings_set_radio(L"radgrpOnlineState",SETTING_ONLINE,0,4);break;
    case SP_VOICE: {
        DWORD slider=settings_find(L"sliderVolume");
        if(slider) xui_int(slider,L"Value",setting_get_int(SETTING_VOLUME,xui_get_int(slider,L"Value",10)));
        settings_set_check(L"chkMute",SETTING_MUTE,0);
        settings_set_radio(L"radgrpOutputLocation",SETTING_OUTPUT,0,3);
        break;
    }
    case SP_CONTROLLER:settings_set_check(L"chkVibration",SETTING_VIBRATION,1);break;
    case SP_NOTIFY:settings_set_check(L"chkShow",SETTING_SHOW,1);settings_set_check(L"chkSound",SETTING_SOUND,1);break;
    case SP_SKINS: {
        DWORD list=settings_find(L"SkinsListId");
        guide_show(page,L"txtSelectSkin",TRUE);
        guide_list_bind(list,settings_list_count,settings_theme_text,0,(void *)SETTINGS_THEME_COUNT);
        int theme=setting_get_int(SETTING_THEME,0);
        xui_set_cursel(list,theme>=0 && theme<SETTINGS_THEME_CUSTOMIZE?theme:SETTINGS_THEME_CUSTOMIZE);
        xui_focus(list);
        break;
    }
    case SP_TRIM: {
        DWORD list=settings_find(L"StyleListId");
        guide_list_bind(list,settings_list_count,settings_trim_text,0,(void *)SETTINGS_TRIM_COUNT);
        xui_set_cursel(list,setting_get_int(SETTING_TRIM,0));xui_focus(list);
        break;
    }
    case SP_TILE: {
        DWORD list=settings_find(L"TileListId");
        guide_list_bind(list,settings_list_count,settings_tile_text,0,(void *)SETTINGS_TILE_COUNT);
        int tile=setting_get_int(SETTING_TILE,0);
        xui_set_cursel(list,tile);xui_focus(list);
        settings_fill_tile(settings_find(L"PreviewId"),tile,(DWORD)setting_get_int(SETTING_COLOR,SETTINGS_COLOR_DEFAULT));
        break;
    }
    case SP_COLOR: {
        DWORD color=(DWORD)setting_get_int(SETTING_COLOR,SETTINGS_COLOR_DEFAULT);
        const WCHAR *ids[]={L"sliderRed",L"sliderGreen",L"sliderBlue"};
        for(int i=0;i<3;i++) {DWORD s=settings_find(ids[i]);if(s) xui_int(s,L"Value",(color>>(16-8*i))&255);}
        settings_fill(settings_find(L"PreviewId"),0,settings_slider_color());
        break;
    }
    }
    settings_loading=FALSE;
    log_value("Settings page",settings_page_kind);
}
static void settings_save_int(const WCHAR *key,int value) {
    setting_set_int(key,value);
    char line[96];wsprintfA(line,"Settings: %ls = %d",key,value);log_text(line);
}
static void settings_save_check(DWORD src,const WCHAR *key) {settings_save_int(key,xui_checked(src)?1:0);}
static void settings_choose_theme(int theme) {
    settings_save_int(SETTING_THEME,theme);
    settings_save_int(SETTING_TRIM,settings_theme_trim[theme]);
    settings_save_int(SETTING_TILE,settings_theme_tile[theme]);
    settings_apply_trim();settings_apply_background();
}
static BOOL settings_is_color_slider(const WCHAR *id) {return !lstrcmpW(id,L"sliderRed") || !lstrcmpW(id,L"sliderGreen") || !lstrcmpW(id,L"sliderBlue");}
static void settings_notify(DWORD src,DWORD code,DWORD *data) {
    if(!settings_page_scene || settings_loading || !blades_guide_open || blades_current()!=settings_page_scene) return;
    const WCHAR *id=blades_id(src);
    if(diagnostics) {char line[160];wsprintfA(line,"Settings notify %u src %ls",code,id);log_text(line);}
    switch(settings_page_kind) {
    case SP_ONLINE:
        if((code==1 || code==4) && !lstrcmpW(id,L"radgrpOnlineState")) settings_save_int(SETTING_ONLINE,xui_cursel(src));
        break;
    case SP_VOICE:
        if(code==0xB && !lstrcmpW(id,L"sliderVolume")) settings_save_int(SETTING_VOLUME,xui_get_int(src,L"Value",10));
        if(code==1 && !lstrcmpW(id,L"chkMute")) settings_save_check(src,SETTING_MUTE);
        if((code==1 || code==4) && !lstrcmpW(id,L"radgrpOutputLocation")) settings_save_int(SETTING_OUTPUT,xui_cursel(src));
        break;
    case SP_CONTROLLER:
        if(code==1 && !lstrcmpW(id,L"chkVibration")) {
            settings_save_check(src,SETTING_VIBRATION);
            if(settings_vibration_enabled()) settings_rumble();
        }
        break;
    case SP_NOTIFY:
        if(code==1 && !lstrcmpW(id,L"chkShow")) settings_save_check(src,SETTING_SHOW);
        if(code==1 && !lstrcmpW(id,L"chkSound")) settings_save_check(src,SETTING_SOUND);
        break;
    case SP_SKINS:
        if(code==1 && !lstrcmpW(id,L"SkinsListId")) {
            int i=xui_cursel(src);
            if(i==SETTINGS_THEME_CUSTOMIZE) settings_nav_pending=blades_catalog(L"hud_hud.xzp",L"OptionsPersonalization.xur");
            else if(i>=0 && i<SETTINGS_THEME_CUSTOMIZE) {settings_choose_theme(i);blades_pending_back=1;}
        }
        break;
    case SP_CUSTOMIZE:
        if(code==1 && !lstrcmpW(id,L"CustomizeList")) {
            int i=xui_cursel(src);
            if(i==0) settings_nav_pending=blades_catalog(L"hud_hud.xzp",L"DashStyleSelect.xur");
            if(i==1) settings_nav_pending=blades_catalog(L"hud_hud.xzp",L"TileSelect.xur");
        }
        break;
    case SP_TRIM:
        if(code==1 && !lstrcmpW(id,L"StyleListId")) {
            int i=xui_cursel(src);
            if(i>=0 && i<SETTINGS_TRIM_COUNT) {settings_save_int(SETTING_TRIM,i);settings_save_int(SETTING_THEME,-1);settings_apply_trim();blades_pending_back=1;}
        }
        break;
    case SP_TILE:
        if(code==4 && !lstrcmpW(id,L"TileListId")) {
            int i=xui_cursel(src);
            if(i>=0 && i<SETTINGS_TILE_COUNT) settings_fill_tile(settings_find(L"PreviewId"),i,(DWORD)setting_get_int(SETTING_COLOR,SETTINGS_COLOR_DEFAULT));
        }
        if(code==1 && !lstrcmpW(id,L"TileListId")) {
            int i=xui_cursel(src);
            if(i==SETTINGS_TILE_CUSTOM) settings_nav_pending=blades_catalog(L"hud_hud.xzp",L"ColorSelect.xur");
            else if(i>=0 && i<SETTINGS_TILE_CUSTOM) {settings_save_int(SETTING_TILE,i);settings_save_int(SETTING_THEME,-1);settings_apply_background();blades_pending_back=1;}
        }
        break;
    case SP_COLOR:
        if(code==0xB && settings_is_color_slider(id)) settings_fill(settings_find(L"PreviewId"),0,settings_slider_color());
        /* A on a slider reaches the scene's legend button btnA (UnfocusedInput). */
        if(code==1 && (settings_is_color_slider(id) || !lstrcmpW(id,L"btnA"))) {
            settings_save_int(SETTING_COLOR,(int)settings_slider_color());settings_save_int(SETTING_TILE,SETTINGS_TILE_CUSTOM);settings_save_int(SETTING_THEME,-1);
            settings_apply_background();blades_pending_back=1;
        }
        break;
    }
}

/* ---------- Shut Down ---------- */
/* Blades Guide (hud.xex) confirmation: hud Strings.xus 45 "Are you sure you want to turn off the
   console?" with 47 "Yes" / 29 "No". (The xam "Shut Down Console"/"Turn Off Controller" choice
   belongs to xam's power menu, not to Personal Settings.) Yes closes the app. */
static void settings_shutdown_done(int choice,void *ctx) {
    char probe[8];
    if(choice<0 && GetEnvironmentVariableA("XUI_PROBE_SHUTDOWN",probe,sizeof probe)) choice=atoi(probe);  /* probe runs only */
    log_value("Settings: Shut Down choice",choice);
    if(choice==0) {log_text("Settings: Shut Down - closing the application");ui_quit=TRUE;}
}
static BOOL settings_dispatch(DWORD button,const WCHAR *id) {
    if(!lstrcmpW(id,L"btnTurnOff")) {
        static const WCHAR *buttons[]={L"Yes",L"No"};
        guide_msgbox(L"Shut Down",L"Are you sure you want to turn off the console?",buttons,2,1,settings_shutdown_done,0);
        return TRUE;
    }
    return FALSE;
}
static BOOL settings_back(void) {return FALSE;}
static void settings_tick(void) {
    settings_frame++;
    if(settings_applied_profile!=profile_signed) {
        settings_applied_profile=profile_signed;settings_applied_guide=settings_applied_dash=0;
    }
    settings_apply_all();
    if(settings_nav_pending>=0) {int index=settings_nav_pending;settings_nav_pending=-1;blades_navigate(index);}
    /* A settings page was popped: refresh the one now on top (its values may have changed). */
    if(settings_page_scene && blades_current()!=settings_page_scene) {
        settings_page_scene=0;settings_page_kind=SP_NONE;
        BladesPage *page=blades_page();
        if(page && blades_guide_open) settings_page(page->scene,page->catalog);
    }
    if(blades_guide_open && settings_frame%30==0) {settings_battery(guide_menu);if(blades_guide_home!=guide_menu) settings_battery(blades_guide_home);}
}
static const GuideLegend *settings_legend(DWORD scene) {return 0;}
