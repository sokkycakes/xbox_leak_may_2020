/* A new interface built with the original XUI engine, with no GFWL services.
 * Edit this file to start your own dashboard. Press F2 or launch --dashboard.
 */
static XuiElement dashboard_status, dashboard_buttons[3];
static HRESULT dashboard_create(XuiElement root,XuiElement *out) {
    HRESULT hr=xui_create(L"XuiCanvas",out);
    if(FAILED(hr)) return hr;
    xui_bounds(*out,0,0,1280,720);
    XuiElement title=0,subtitle=0;
    xui_create(L"XuiText",&title);
    xui_bounds(title,100,90,1080,80);
    xui_text(title,L"Your dashboard");
    xui_string(title,L"Font",L"Convection");
    log_value("Dashboard font size",xui_number(title,L"PointSize",42));
    xui_color(title,L"TextColor",0xffecf2f7);
    xui_add(*out,title);
    xui_create(L"XuiText",&subtitle);
    xui_bounds(subtitle,104,175,1050,60);
    xui_text(subtitle,L"A standalone interface, running on the original XUI engine.");
    xui_number(subtitle,L"PointSize",22);
    xui_color(subtitle,L"TextColor",0xffb5c5d5);
    xui_add(*out,subtitle);
    const WCHAR *labels[3]={L"Games",L"Media",L"Settings"};
    for(int i=0;i<3;i++) {
        xui_create(L"XuiButton",&dashboard_buttons[i]);
        xui_bounds(dashboard_buttons[i],105,280+i*90,800,76);
        xui_string(dashboard_buttons[i],L"Text",L"");
        log_value("Dashboard button visual",xui_string(dashboard_buttons[i],L"Visual",L"btn_standard"));
        xui_add(*out,dashboard_buttons[i]);
        XuiElement label=0;xui_create(L"XuiText",&label);
        xui_bounds(label,48,6,700,64);xui_text(label,labels[i]);
        xui_number(label,L"PointSize",25);xui_color(label,L"TextColor",0xfff0f4f8);
        xui_add(dashboard_buttons[i],label);
    }
    xui_create(L"XuiText",&dashboard_status);
    xui_bounds(dashboard_status,104,595,1080,65);
    xui_text(dashboard_status,L"Arrow keys / controller: navigate    Enter / A: select    F1: Guide");
    xui_number(dashboard_status,L"PointSize",20);
    xui_color(dashboard_status,L"TextColor",0xffb5c5d5);
    xui_add(*out,dashboard_status);
    return xui_add(root,*out);
}
