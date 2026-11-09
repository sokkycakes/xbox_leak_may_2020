/* Guide module: music. */
static BOOL music_dispatch(DWORD button,const WCHAR *id) {return FALSE;}
static void music_notify(DWORD src,DWORD code,DWORD *data) {}
static void music_page(DWORD page,int catalog) {}
static BOOL music_back(void) {return FALSE;}
static void music_tick(void) {}
static BOOL music_filter(DWORD element,DWORD *message) {return FALSE;}
static void music_guide_opened(void) {}
static const GuideLegend *music_legend(DWORD scene) {return 0;}
