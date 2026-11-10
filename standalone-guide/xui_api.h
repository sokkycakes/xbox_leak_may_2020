#pragma once
/* Small source-level API for the verified 1.2.241.0 XUI engine embedded by build.py.
 * Include after the host binds self_image. All UI calls run on the main thread.
 * Handles are 32-bit XUI handles, not pointers. Strings are UTF-16.
 */
typedef DWORD XuiElement;
typedef struct { DWORD type; union { DWORD integer; float number; const WCHAR *text; }; DWORD extra[3]; } XuiValue;
static HRESULT xui_create(const WCHAR *type,XuiElement *out) {
    return XUI_FN(0x7e0455,HRESULT,const WCHAR *,XuiElement *)(type,out);
}
static HRESULT xui_add(XuiElement parent,XuiElement child) {
    return XUI_FN(0x7d3261,HRESULT,XuiElement,XuiElement)(parent,child);
}
static HRESULT xui_property(XuiElement element,const WCHAR *name,const XuiValue *value) {
    DWORD id=0;HRESULT hr=XUI_FN(0x7d83a1,HRESULT,XuiElement,const WCHAR *,DWORD *)(element,name,&id);
    return FAILED(hr)?hr:XUI_FN(0x7dbb64,HRESULT,XuiElement,DWORD,DWORD,const XuiValue *)(element,id,0,value);
}
static HRESULT xui_string(XuiElement element,const WCHAR *name,const WCHAR *text) {
    XuiValue value={.type=5,.text=text};return xui_property(element,name,&value);
}
static HRESULT xui_number(XuiElement element,const WCHAR *name,float number) {
    XuiValue value={.type=4,.number=number};return xui_property(element,name,&value);
}
static HRESULT xui_bool(XuiElement element,const WCHAR *name,BOOL enabled) {
    XuiValue value={.type=1,.integer=enabled};return xui_property(element,name,&value);
}
static HRESULT xui_color(XuiElement element,const WCHAR *name,DWORD argb) {
    XuiValue value={.type=6,.integer=argb};return xui_property(element,name,&value);
}
static HRESULT xui_bounds(XuiElement element,float x,float y,float width,float height) {
    float position[3]={x,y,0};HRESULT hr=XUI_FN(0x7d34bc,HRESULT,XuiElement,const float *)(element,position);
    return FAILED(hr)?hr:XUI_FN(0x7d345b,HRESULT,XuiElement,float,float)(element,width,height);
}
static XuiElement xui_find(XuiElement parent,const WCHAR *id) {
    XuiElement found=0;XUI_FN(0x7d3605,HRESULT,XuiElement,const WCHAR *,XuiElement *)(parent,id,&found);return found;
}
static HRESULT xui_text(XuiElement element,const WCHAR *text) {
    return XUI_FN(0x7d3a21,HRESULT,XuiElement,const WCHAR *)(element,text);
}
static HRESULT xui_focus(XuiElement element) {
    return XUI_FN(0x7d43ef,HRESULT,XuiElement,DWORD)(element,0);
}
static HRESULT xui_visuals(XuiElement root) {
    DWORD message[5]={20,0x29,0,0,0};return XUI_FN(0x7decdf,HRESULT,XuiElement,void *)(root,message);
}
