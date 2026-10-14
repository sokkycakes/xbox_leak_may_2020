/*
 * Clearing the window framebuffer before the present pass draws into it.
 *
 * The present quad covers every pixel, so the clear changes nothing that
 * shows.  On a tiling GPU (the Pi's VC4) it still pays: a target cleared
 * first need not have each tile's old contents loaded back from memory
 * before the quad is drawn over them.  Off by default; XBCOMPAT_PRESENT_CLEAR=1
 * turns it on.
 *
 * Included by d3d8.c (present_window_framebuffer).
 */
/* XBCOMPAT_PRESENT_CLEAR, read once. */
static bool present_target_clear_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("XBCOMPAT_PRESENT_CLEAR");
        enabled = value && value[0] && value[0] != '0';
    }
    return enabled;
}

/* With the window framebuffer bound and the color mask open (the caller
   has just set it), before the quad. */
static void present_target_clear_color(void)
{
    glClear(GL_COLOR_BUFFER_BIT);
}
