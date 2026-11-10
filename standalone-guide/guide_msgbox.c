/* Console message boxes.
 * On the console xam shows XamShowMessageBoxUI in the Guide frame's Error panel: hudbkgnd's ErrorHUD
 * slides in (frames HalfToError / FullToError / ClosedToError and back) and the box is an element of
 * class XuiMessageBox with the hud skin visual XuiMessageBox2/3/4 (N = buttons + the hidden B button).
 * The GFWL engine has the same function, XuiShowMessageBoxEx at 0x7ee7e6 (create XuiMessageBox, Visual,
 * attach, then 0x7ee63d fills the scene Text/MessageText/ButtonN and focuses), used by xlive's own HUD
 * at 0x49a944 (ErrorHUD > ErrorHostElement > XuiMessageBox2..4). Tested here: it runs (once xlive's
 * string-table lock 0x8ab048 is initialised, which the host's partial xlive start-up skips) and fills
 * the instance, but this engine draws the controls inside an element visual from their first
 * instantiation only: no button labels, title or message text appear, and later Show/Visual changes on
 * them are not drawn either.
 * So the box is built from the visual's own layout, element for element (huduiskin skin.xur
 * XuiMessageBox2/3/4: 515x770 scene, Title 24pt white at 105,54, Icon 32x32 at 65,61, MessageText
 * read-only multiline XuiEdit "edit_Error", Button0..2 420x47, legends btnA/btnB/btnX/btnY), with the
 * hud skin visuals for each control. Presses and B are handled by the host; the box stays on screen
 * while the Error panel slides back out and is destroyed when that timeline has run.
 */
#define MSGBOX_FRAMES 70   /* longest Error-out frame range is 59 frames (ErrorToHalf / ErrorToFull) */
static struct {
    DWORD box,focus;GuideChoice done;void *ctx;int count,pending;
    WCHAR back[8];
    WCHAR title[128],text[1024],labels[3][64];
    DWORD dying;int dying_frames;
} msgbox={.pending=-2};
static BOOL guide_msgbox_open(void) {return msgbox.box!=0;}
static void msgbox_destroy_dying(void) {
    if(msgbox.dying) {XUI_FN(0x7dec45,HRESULT,DWORD)(msgbox.dying);msgbox.dying=0;}
}
/* hudbkgnd's Error keyframes stop ErrorHUD at x=800: its mirrored blade then sits at 806..911 and
   ErrorHostElement at 861, so a 515-wide box would run ~260 units off the 1120-wide frame. The
   panel's contents are moved once by -374.5 so the Error panel lands as the mirror image of the
   Half Guide (blade 431.5..536.5, box content 552..972); the authored slide is kept. */
static DWORD msgbox_layout_done;
static void msgbox_layout(void) {
    if(msgbox_layout_done==guide_scene) return;
    static const struct {const WCHAR *id;float x,y;} parts[]={
        {L"graphic-base-error",1093-374.5f,55},{L"graphic-blade-error",111-374.5f,0},{L"ErrorHostElement",61-374.5f,55}};
    for(int i=0;i<3;i++) {
        DWORD e=ui_find_deep(guide_scene,parts[i].id,0);
        float at[3]={parts[i].x,parts[i].y,0};
        if(e) XUI_FN(0x7d34bc,HRESULT,DWORD,const float *)(e,at);
    }
    msgbox_layout_done=guide_scene;
}
static DWORD msgbox_add(DWORD parent,const WCHAR *cls,const WCHAR *id,const WCHAR *visual,float x,float y,float w,float h) {
    DWORD e=0;
    if(FAILED(xui_create(cls,&e))) return 0;
    xui_bounds(e,x,y,w,h);
    if(id) xui_string(e,L"Id",id);
    if(visual) xui_string(e,L"Visual",visual);
    if(FAILED(xui_add(parent,e))) {XUI_FN(0x7dec45,HRESULT,DWORD)(e);return 0;}
    return e;
}
static void guide_msgbox_impl(const WCHAR *title,const WCHAR *body,const WCHAR **buttons,int count,int focus,GuideChoice done,void *ctx) {
    if(count<1) count=1;
    if(count>3) count=3;
    if(focus<0 || focus>=count) focus=0;
    msgbox_destroy_dying();
    BOOL chained=msgbox.box!=0;
    if(msgbox.box) {XUI_FN(0x7dec45,HRESULT,DWORD)(msgbox.box);msgbox.box=0;}
    if(!guide_frame()) {log_text("Message box: Guide frame not available");if(done) done(-1,ctx);return;}
    msgbox_layout();
    DWORD host=ui_find_deep(guide_scene,L"ErrorHostElement",0);
    if(!host) {log_text("Message box: ErrorHostElement missing");if(done) done(-1,ctx);return;}
    lstrcpynW(msgbox.title,title?title:L"",128);lstrcpynW(msgbox.text,body?body:L"",1024);
    if(!chained) msgbox.focus=xui_focused();
    /* XuiMessageBox2/3/4 layout (N = count+1). */
    static const float button_y[3][3]={{520},{475,520},{428,473,518}},button_x[3][3]={{65},{65,65},{65,65,64}};
    static const float text_y[3]={121,120,121},text_h[3]={372,325,289},title_w[3]={379,380,381};
    DWORD box=msgbox_add(host,L"XuiScene",L"XuiScene1",0,0,0,515,770);
    if(!box) {if(done) done(-1,ctx);return;}
    DWORD edit=msgbox_add(box,L"XuiEdit",L"MessageText",L"hud.edit_Error",65,text_y[count-1],420,text_h[count-1]);
    if(edit) {xui_bool(edit,L"ReadOnly",TRUE);xui_bool(edit,L"Multiline",TRUE);xui_string(edit,L"Text",msgbox.text);}
    DWORD t=msgbox_add(box,L"XuiText",L"Title",0,105,54,title_w[count-1],47);
    if(t) {
        XuiValue style={.type=3,.integer=272};
        xui_color(t,L"TextColor",0xffffffff);xui_number(t,L"PointSize",24);xui_property(t,L"TextStyle",&style);
        xui_string(t,L"Text",msgbox.title);
    }
    msgbox_add(box,L"XuiImage",L"Icon",0,65,61,32,32);   /* xam fills it per message type; none is passed here */
    DWORD btn[3]={0};
    for(int i=count-1;i>=0;i--) {
        WCHAR id[8];wsprintfW(id,L"Button%d",i);
        lstrcpynW(msgbox.labels[i],buttons && buttons[i]?buttons[i]:L"",64);
        btn[i]=msgbox_add(box,L"XuiButton",id,L"hud.XuiButton",button_x[count-1][i],button_y[count-1][i],420,47);
        if(btn[i]) xui_string(btn[i],L"Text",msgbox.labels[i]);
    }
    for(int i=0;i<count;i++) if(btn[i]) {
        WCHAR up[8],down[8];wsprintfW(up,L"Button%d",i-1);wsprintfW(down,L"Button%d",i+1);
        if(i>0) xui_string(btn[i],L"NavUp",up);
        if(i<count-1) xui_string(btn[i],L"NavDown",down);
    }
    /* Legends as authored: Y and X disabled; A/B texts as the Guide's own legends. */
    DWORD l;
    if((l=msgbox_add(box,L"XuiButton",L"btnY",L"hud.legend_Y",80,623,219,46))) xui_bool(l,L"Enabled",FALSE);
    if((l=msgbox_add(box,L"XuiButton",L"btnB",L"hud.legend_B",41,623,420,40))) xui_string(l,L"Text",L"Back");
    if((l=msgbox_add(box,L"XuiButton",L"btnA",L"hud.legend_A",62,651,420,40))) xui_string(l,L"Text",L"Select");
    if((l=msgbox_add(box,L"XuiButton",L"btnX",L"hud.legend_X",59,651,219,46))) xui_bool(l,L"Enabled",FALSE);
    xui_visuals(box);
    msgbox.box=box;msgbox.done=done;msgbox.ctx=ctx;msgbox.count=count;msgbox.pending=-2;
    /* The Guide frame takes focus only while the Guide is open: let the box take it. */
    xui_bool(guide_scene,L"Show",TRUE);
    xui_bool(guide_scene,L"DisableFocusRecursion",FALSE);
    if(!chained) {lstrcpyW(msgbox.back,guide_state);guide_play(L"Error");}
    if(btn[focus]) xui_focus(btn[focus]);
    log_value("Message box buttons",count);
}
/* Close the box with a choice: the panel slides back to the state it came from unless the
   callback moved the Guide on (closed it, opened another box). */
static void msgbox_finish(int choice) {
    if(!msgbox.box) return;
    log_value("Message box choice",choice);
    DWORD box=msgbox.box;GuideChoice done=msgbox.done;void *ctx=msgbox.ctx;
    msgbox.box=0;msgbox.done=0;msgbox.pending=-2;
    msgbox_destroy_dying();
    msgbox.dying=box;msgbox.dying_frames=MSGBOX_FRAMES;
    /* The box keeps drawing during the slide but must not keep focus or take presses. */
    xui_bool(box,L"DisableFocusRecursion",TRUE);
    if(msgbox.focus) xui_focus(msgbox.focus);
    if(!blades_guide_open) xui_bool(guide_scene,L"DisableFocusRecursion",TRUE);
    if(done) done(choice,ctx);
    if(msgbox.box) return;   /* the callback chained another box */
    if(!lstrcmpW(guide_state,L"Error")) guide_play(blades_guide_open?msgbox.back:L"Closed");
}
/* Presses on ButtonN inside the box (legend A is a press on the focused button). */
static BOOL msgbox_filter(DWORD element,DWORD *message) {
    DWORD root=msgbox.box?msgbox.box:msgbox.dying;
    if(!root || message[1]!=0xe || !message[4]) return FALSE;
    DWORD *n=(DWORD *)message[4];
    if(n[0]!=1 || !blades_contains(root,n[1],0)) return FALSE;
    if(msgbox.box && msgbox.pending==-2) {
        DWORD src=n[1];const WCHAR *id=blades_id(src);
        if(!lstrcmpW(id,L"btnA")) {src=xui_focused();id=blades_id(src);}
        if(!lstrcmpW(id,L"btnB")) msgbox.pending=-1;
        else if(!wcsncmp(id,L"Button",6) && id[6]>=L'0' && id[6]<L'0'+msgbox.count && !id[7]) msgbox.pending=id[6]-L'0';
    }
    message[2]=1;return TRUE;
}
static BOOL msgbox_back(void) {
    if(!msgbox.box) return FALSE;
    if(msgbox.pending==-2) msgbox.pending=-1;
    return TRUE;
}
static void msgbox_tick(void) {
    if(msgbox.box && msgbox.pending!=-2) msgbox_finish(msgbox.pending);
    if(msgbox.dying && --msgbox.dying_frames<=0) msgbox_destroy_dying();
}
