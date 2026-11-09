/* Guide module: live. */
static BOOL live_dispatch(DWORD button,const WCHAR *id) {return FALSE;}
static void live_notify(DWORD src,DWORD code,DWORD *data) {}
static void live_page(DWORD page,int catalog) {}
static BOOL live_back(void) {return FALSE;}
static void live_tick(void) {}
static const GuideLegend *live_legend(DWORD scene) {return 0;}
static void live_join(void) {}
static void live_recover(void) {}
static void guide_toast(const WCHAR *text,const WCHAR *image) {}
static void guide_status_show(const WCHAR *text) {}
static void guide_status_hide(void) {}
