/* Console message boxes. */
static void guide_msgbox_impl(const WCHAR *title,const WCHAR *body,const WCHAR **buttons,int count,int focus,GuideChoice done,void *ctx) {
    blades_notice(body);
    if(done) done(-1,ctx);
}
