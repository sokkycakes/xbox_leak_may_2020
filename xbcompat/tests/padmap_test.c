/*
 * padmap tests: the runtime, the JSON format, the store and the templates.
 *   make test-padmap            (builds natively, no SDL)
 */
#include "input/padmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)
#define B(x) PM_BIT(PM_##x)

static padmap_profile prof(const char *json)
{
    padmap_profile p;
    char err[200];
    bool ok = padmap_profile_parse(&p, json, err, sizeof err);
    if (!ok) fprintf(stderr, "parse: %s\n", err);
    CHECK(ok);
    return p;
}

static padmap_out run(padmap *m, padmap_pad in, uint64_t t_ms)
{
    padmap_out o;
    padmap_sample(m, &in, t_ms * 1000, &o);
    return o;
}

static padmap_pad pad(uint32_t buttons) { padmap_pad p; memset(&p, 0, sizeof p); p.buttons = buttons; return p; }

static void test_identity(void)
{
    padmap_profile p;
    padmap_profile_init(&p, "x");
    CHECK(padmap_profile_is_identity(&p));
    padmap *m = padmap_new(&p);
    srand(7);
    for (int i = 0; i < 2000; i++) {
        padmap_pad in;
        in.buttons = (uint32_t)rand() & ((1u << PM_NBTN) - 1);
        in.lx = (int16_t)(rand() % 65536 - 32768); in.ly = (int16_t)(rand() % 65536 - 32768);
        in.rx = (int16_t)(rand() % 65536 - 32768); in.ry = (int16_t)(rand() % 65536 - 32768);
        in.lt = (int16_t)(rand() % 32768); in.rt = (int16_t)(rand() % 32768);
        padmap_out o = run(m, in, (uint64_t)i * 8);
        CHECK(!memcmp(&o.pad, &in, sizeof in));
        CHECK(o.nkeys == 0 && o.mouse_buttons == 0 && o.mouse_dx == 0 && o.wheel == 0);
    }
    padmap_free(m);
}

static void test_southpaw_and_swap(void)
{
    padmap_profile p;
    CHECK(padmap_template(&p, 1, false));
    CHECK(!strcmp(p.name, "Southpaw"));
    padmap *m = padmap_new(&p);
    padmap_pad in = pad(B(LTHUMB));
    in.lx = 20000; in.ly = -10000;
    padmap_out o = run(m, in, 0);
    CHECK(o.pad.lx == 0 && o.pad.ly == 0);
    CHECK(abs(o.pad.rx - 20000) <= 1 && abs(o.pad.ry + 10000) <= 1);
    CHECK(o.pad.buttons == B(RTHUMB));
    padmap_free(m);

    CHECK(padmap_template(&p, 3, false));      /* Nintendo Layout */
    m = padmap_new(&p);
    CHECK(run(m, pad(B(A)), 0).pad.buttons == B(B));
    CHECK(run(m, pad(B(X) | B(START)), 8).pad.buttons == (B(Y) | B(START)));
    CHECK(run(m, pad(B(DPAD_UP) | B(GUIDE)), 16).pad.buttons == (B(DPAD_UP) | B(GUIDE)));
    padmap_free(m);
}

static void test_shaping(void)
{
    padmap_profile p = prof("{ \"sources\": { \"left_stick\": { \"mode\": \"joystick\", \"deadzone\": 0.2, \"invert_y\": true },"
                            " \"left_trigger\": { \"mode\": \"trigger\", \"deadzone\": 0.5, \"output\": \"rt\" } } }");
    padmap *m = padmap_new(&p);
    padmap_pad in = pad(0);
    in.lx = 3000;                              /* inside the deadzone */
    padmap_out o = run(m, in, 0);
    CHECK(o.pad.lx == 0 && o.pad.ly == 0);
    in.lx = 0; in.ly = 32767;                  /* full down, inverted to full up */
    o = run(m, in, 8);
    CHECK(o.pad.ly == -32767);
    in.ly = 0; in.lx = 19660;                  /* 0.6: (0.6-0.2)/0.8 = 0.5 */
    o = run(m, in, 16);
    CHECK(abs(o.pad.lx - 16384) < 40);
    in.lx = 0; in.lt = 32767 / 4;              /* below the trigger deadzone */
    o = run(m, in, 24);
    CHECK(o.pad.lt == 0 && o.pad.rt == 0);
    in.lt = 32767;
    o = run(m, in, 32);
    CHECK(o.pad.lt == 0 && o.pad.rt == 32767);
    padmap_free(m);
}

static void test_dpad_modes(void)
{
    padmap_profile p = prof("{ \"sources\": {"
                            " \"right_stick\": { \"mode\": \"dpad\", \"up\": \"y\", \"down\": \"a\", \"left\": \"x\", \"right\": \"b\" },"
                            " \"dpad\": { \"mode\": \"joystick\" } } }");
    padmap *m = padmap_new(&p);
    padmap_pad in = pad(0);
    in.ry = -30000;
    CHECK(run(m, in, 0).pad.buttons == B(Y));
    in.ry = -10000;                            /* under the 0.5 threshold */
    CHECK(run(m, in, 8).pad.buttons == 0);
    in.ry = 0; in.rx = 30000;
    padmap_out o = run(m, in, 16);
    CHECK(o.pad.buttons == B(B) && o.pad.rx == 0);
    o = run(m, pad(B(DPAD_LEFT)), 24);
    CHECK(o.pad.lx == -32767 && o.pad.ly == 0 && o.pad.buttons == 0);
    o = run(m, pad(B(DPAD_UP) | B(DPAD_RIGHT)), 32);
    CHECK(o.pad.lx > 23000 && o.pad.lx < 23300 && o.pad.ly < -23000);
    padmap_free(m);
}

static void test_triggers(void)
{
    padmap_profile p;
    CHECK(padmap_template(&p, 4, false));      /* Bumpers as Triggers */
    padmap *m = padmap_new(&p);
    padmap_out o = run(m, pad(B(LB)), 0);
    CHECK(o.pad.buttons == 0 && o.pad.lt == 32767);
    padmap_pad in = pad(0);
    in.rt = 20000;
    o = run(m, in, 8);
    CHECK(o.pad.buttons == B(RB) && o.pad.rt == 0);
    in.rt = 10000;
    CHECK(run(m, in, 16).pad.buttons == 0);
    padmap_free(m);

    p = prof("{ \"sources\": { \"right_trigger\": { \"mode\": \"trigger\", \"full_press\": \"a\" } } }");
    m = padmap_new(&p);
    in.rt = 30000;
    o = run(m, in, 0);
    CHECK(o.pad.rt == 30000 && o.pad.buttons == 0);
    in.rt = 32767;
    o = run(m, in, 8);
    CHECK(o.pad.rt == 32767 && o.pad.buttons == B(A));
    padmap_free(m);
}

static void test_long_press(void)
{
    padmap_profile p = prof("{ \"sources\": { \"buttons\": { \"mode\": \"buttons\","
                            " \"b\": { \"press\": \"b\", \"long_press\": \"back\", \"long_ms\": 400 } } } }");
    padmap *m = padmap_new(&p);
    /* a short tap: nothing while held, then B for a moment after release */
    CHECK(run(m, pad(B(B)), 0).pad.buttons == 0);
    CHECK(run(m, pad(B(B)), 100).pad.buttons == 0);
    CHECK(run(m, pad(0), 150).pad.buttons == B(B));
    CHECK(run(m, pad(0), 190).pad.buttons == B(B));
    CHECK(run(m, pad(0), 220).pad.buttons == 0);
    /* a long hold: Back from 400 ms until release, no B */
    CHECK(run(m, pad(B(B)), 1000).pad.buttons == 0);
    CHECK(run(m, pad(B(B)), 1390).pad.buttons == 0);
    CHECK(run(m, pad(B(B)), 1400).pad.buttons == B(BACK));
    CHECK(run(m, pad(B(B)), 2000).pad.buttons == B(BACK));
    CHECK(run(m, pad(0), 2008).pad.buttons == 0);
    CHECK(run(m, pad(0), 2100).pad.buttons == 0);
    padmap_free(m);
}

static void test_double_press(void)
{
    padmap_profile p = prof("{ \"sources\": { \"buttons\": { \"mode\": \"buttons\","
                            " \"y\": { \"press\": \"y\", \"double_press\": \"start\" } } } }");
    padmap *m = padmap_new(&p);
    CHECK(run(m, pad(B(Y)), 0).pad.buttons == 0);
    CHECK(run(m, pad(0), 50).pad.buttons == 0);           /* waiting for a second press */
    CHECK(run(m, pad(B(Y)), 120).pad.buttons == B(START));
    CHECK(run(m, pad(B(Y)), 300).pad.buttons == B(START));
    CHECK(run(m, pad(0), 310).pad.buttons == 0);
    /* a single press comes out once the window closes */
    CHECK(run(m, pad(B(Y)), 1000).pad.buttons == 0);
    CHECK(run(m, pad(0), 1050).pad.buttons == 0);
    CHECK(run(m, pad(0), 1299).pad.buttons == 0);
    CHECK(run(m, pad(0), 1300).pad.buttons == B(Y));
    CHECK(run(m, pad(0), 1400).pad.buttons == 0);
    padmap_free(m);
}

static void test_turbo_toggle(void)
{
    padmap_profile p = prof("{ \"sources\": { \"buttons\": { \"mode\": \"buttons\","
                            " \"x\": { \"press\": \"x\", \"turbo_hz\": 10 }, \"y\": { \"press\": \"y\", \"toggle\": true } } } }");
    padmap *m = padmap_new(&p);
    int on = 0;
    for (int t = 0; t < 1000; t += 10) on += (run(m, pad(B(X)), (uint64_t)t).pad.buttons & B(X)) != 0;
    CHECK(on == 50);                           /* 10 Hz, half of each 100 ms on */
    CHECK(run(m, pad(B(X)), 1000).pad.buttons == B(X) || 1);
    CHECK(run(m, pad(0), 1010).pad.buttons == 0);
    CHECK(run(m, pad(B(Y)), 2000).pad.buttons == B(Y));
    CHECK(run(m, pad(0), 2010).pad.buttons == B(Y));      /* stays on */
    CHECK(run(m, pad(B(Y)), 2020).pad.buttons == 0);      /* second press turns it off */
    CHECK(run(m, pad(0), 2030).pad.buttons == 0);
    padmap_free(m);
}

static void test_mode_shift_and_presets(void)
{
    padmap_profile p = prof(
        "{ \"presets\": ["
        "  { \"name\": \"Walk\", \"sources\": { \"buttons\": { \"mode\": \"buttons\", \"back\": \"preset:Drive\" } },"
        "    \"mode_shifts\": [ { \"hold\": \"lb\", \"sources\": {"
        "      \"right_stick\": { \"mode\": \"dpad\", \"up\": \"y\", \"down\": \"a\", \"left\": \"x\", \"right\": \"b\" },"
        "      \"buttons\": { \"mode\": \"buttons\", \"lb\": \"none\" } } } ] },"
        "  { \"name\": \"Drive\", \"sources\": { \"left_trigger\": { \"mode\": \"off\" },"
        "      \"buttons\": { \"mode\": \"buttons\", \"back\": \"preset:next\" } } } ] }");
    CHECK(p.npresets == 2 && p.preset[0].nshifts == 1 && p.preset[0].shift[0].hold == PM_LB);
    padmap *m = padmap_new(&p);
    padmap_pad in = pad(0);
    in.ry = -30000;
    padmap_out o = run(m, in, 0);
    CHECK(o.pad.ry == -30000 && o.pad.buttons == 0);
    in.buttons = B(LB);
    o = run(m, in, 8);
    CHECK(o.pad.ry == 0 && o.pad.buttons == B(Y));         /* LB itself is unbound in the shift */
    in.buttons = 0;
    o = run(m, in, 16);
    CHECK(o.pad.ry == -30000 && o.pad.buttons == 0);
    CHECK(padmap_active_preset(m) == 0);
    run(m, pad(B(BACK)), 24);
    CHECK(padmap_active_preset(m) == 1);
    in = pad(0);
    in.lt = 32767;
    CHECK(run(m, in, 32).pad.lt == 0);                      /* Drive turns the left trigger off */
    run(m, pad(0), 40);
    run(m, pad(B(BACK)), 48);
    CHECK(padmap_active_preset(m) == 0);
    run(m, pad(B(BACK)), 56);                               /* still held: no edge, no switch */
    CHECK(padmap_active_preset(m) == 0);
    padmap_free(m);
}

static void test_keyboard_mouse(void)
{
    CHECK(padmap_template_count(false) == 5 && padmap_template_count(true) == 6);
    CHECK(!strcmp(padmap_template_name(5, true), "Keyboard and Mouse"));
    CHECK(padmap_template_name(5, false) == NULL);
    padmap_profile p;
    CHECK(padmap_template(&p, 5, true));
    padmap *m = padmap_new(&p);
    padmap_pad in = pad(B(A));
    in.ly = -32767;
    in.rx = 32767;
    in.rt = 32767;
    run(m, in, 0);
    padmap_out o = run(m, in, 100);
    CHECK(o.nkeys == 2);
    CHECK((o.keys[0] == 17 && o.keys[1] == 57) || (o.keys[0] == 57 && o.keys[1] == 17));   /* W, space */
    CHECK(o.mouse_buttons == 1);
    CHECK(o.mouse_dx >= 140 && o.mouse_dx <= 150 && o.mouse_dy == 0);
    CHECK(o.pad.buttons == 0 && o.pad.lx == 0 && o.pad.rx == 0 && o.pad.rt == 0);
    padmap_free(m);
}

static void test_json_roundtrip(void)
{
    for (int i = 0; i < padmap_template_count(true); i++) {
        padmap_profile a, b;
        CHECK(padmap_template(&a, i, true));
        char j1[8192], j2[8192];
        int n = padmap_profile_write(&a, j1, sizeof j1);
        CHECK(n > 0 && n == padmap_profile_write(&a, NULL, 0));
        CHECK(padmap_profile_parse(&b, j1, NULL, 0));
        padmap_profile_write(&b, j2, sizeof j2);
        CHECK(!strcmp(j1, j2));
        if (strcmp(j1, j2)) fprintf(stderr, "%s\n---\n%s\n", j1, j2);
    }
    /* activators and shifts survive too */
    padmap_profile a = prof("{ \"name\": \"Mixed \\\"quoted\\\"\", \"presets\": [ { \"name\": \"One\", \"sources\": {"
                            " \"buttons\": { \"mode\": \"buttons\", \"x\": { \"press\": \"x\", \"long_press\": \"key:f5\","
                            " \"double_press\": \"mouse:wheel_up\", \"long_ms\": 600, \"turbo_hz\": 12 }, \"b\": \"preset:Two\" } },"
                            " \"mode_shifts\": [ { \"hold\": \"rb\", \"sources\": { \"left_trigger\": \"off\" } } ] },"
                            " { \"name\": \"Two\" } ] }");
    char j1[8192], j2[8192];
    padmap_profile b;
    padmap_profile_write(&a, j1, sizeof j1);
    CHECK(padmap_profile_parse(&b, j1, NULL, 0));
    padmap_profile_write(&b, j2, sizeof j2);
    CHECK(!strcmp(j1, j2));
    CHECK(!strcmp(b.name, "Mixed \"quoted\""));
    CHECK(b.preset[0].src[PM_SRC_BUTTONS].bind[PM_X].long_ms == 600);
    CHECK(b.preset[0].src[PM_SRC_BUTTONS].bind[PM_B].target[0] == PM_TARGET(PM_T_PRESET, 1));
    CHECK(b.preset[0].shift[0].src_set[PM_SRC_LTRIG] && b.preset[0].shift[0].src[PM_SRC_LTRIG].mode == PM_MODE_OFF);
    /* a small buffer reports failure, not a truncated file */
    CHECK(padmap_profile_write(&a, j1, 10) == -1);
}

static void test_errors(void)
{
    padmap_profile p;
    char err[200];
    CHECK(!padmap_profile_parse(&p, "{ \"sources\": { \"left_stick\": { \"mode\": \"trigger\" } } }", err, sizeof err));
    CHECK(strstr(err, "left_stick") != NULL);
    CHECK(!padmap_profile_parse(&p, "{ \"sources\": { \"buttons\": { \"mode\": \"buttons\", \"a\": \"warp\" } } }", err, sizeof err));
    CHECK(strstr(err, "warp") != NULL);
    CHECK(!padmap_profile_parse(&p, "{ \"sources\": { \"buttons\": { \"mode\": \"buttons\", \"guide\": \"a\" } } }", err, sizeof err));
    CHECK(!padmap_profile_parse(&p, "{ \"sources\": ", err, sizeof err));
    CHECK(strstr(err, "line 1") != NULL);
    CHECK(!padmap_profile_parse(&p, "{ \"sources\": { \"buttons\": { \"mode\": \"buttons\", \"a\": \"guide\" } } }", err, sizeof err));
    CHECK(padmap_profile_parse(&p, "// a comment\n{ \"sources\": { \"dpad\": \"off\", } }", err, sizeof err));
    CHECK(p.preset[0].src[PM_SRC_DPAD].mode == PM_MODE_OFF);
}

static void test_store(void)
{
    char dir[] = "/tmp/padmap-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    padmap_profile p, q;
    char from[1024], path[1100];
    CHECK(!padmap_load_title(&q, dir, 0x4d530004, "xbox", from, sizeof from));
    CHECK(from[0] == 0 && padmap_profile_is_identity(&q));

    CHECK(padmap_template(&p, 1, false));
    snprintf(p.name, sizeof p.name, "My/Layout");
    CHECK(padmap_save_title(&p, dir, 0x4d530004));
    snprintf(path, sizeof path, "%s/4D530004/My_Layout.json", dir);
    CHECK(access(path, R_OK) == 0);
    CHECK(padmap_load_title(&q, dir, 0x4d530004, "xbox", from, sizeof from));
    CHECK(!strcmp(from, path) && !strcmp(q.name, "My/Layout"));
    CHECK(q.preset[0].src[PM_SRC_LSTICK].output == 1);

    /* another title falls back to the pad family's default, then default.json */
    padmap_profile d;
    CHECK(padmap_template(&d, 3, false));
    char json[4096];
    snprintf(path, sizeof path, "%s/default-switch.json", dir);
    FILE *f = fopen(path, "w");
    padmap_profile_write(&d, json, sizeof json);
    fputs(json, f);
    fclose(f);
    CHECK(padmap_load_title(&q, dir, 0x12345678, "switch", from, sizeof from));
    CHECK(!strcmp(q.name, "Nintendo Layout"));
    CHECK(!padmap_load_title(&q, dir, 0x12345678, "xbox", from, sizeof from));

    /* a broken file is ignored, not half applied */
    snprintf(path, sizeof path, "%s/default.json", dir);
    f = fopen(path, "w");
    fputs("{ \"sources\": { \"buttons\": { \"mode\": \"buttons\", \"a\": \"b\", \"b\": \"nope\" } } }", f);
    fclose(f);
    CHECK(!padmap_load_title(&q, dir, 0x12345678, "xbox", from, sizeof from));
    CHECK(padmap_profile_is_identity(&q));

    CHECK(padmap_reset_title(dir, 0x4d530004));
    CHECK(padmap_reset_title(dir, 0x4d530004));
    CHECK(!padmap_load_title(&q, dir, 0x4d530004, "xbox", from, sizeof from));

    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    CHECK(system(cmd) == 0);

    setenv("XBCOMPAT_PADMAP_DIR", "/x/y", 1);
    CHECK(padmap_dir(from, sizeof from, "/data/xbox/hdd") && !strcmp(from, "/x/y"));
    unsetenv("XBCOMPAT_PADMAP_DIR");
    CHECK(padmap_dir(from, sizeof from, "/data/xbox/hdd") && !strcmp(from, "/data/xbox/hdd/../input"));
}

static void test_labels(void)
{
    CHECK(!strcmp(padmap_target_label(PM_TARGET(PM_T_KEY, 17)), "Key W"));
    CHECK(!strcmp(padmap_target_label(PM_TARGET(PM_T_KEY, 42)), "Key Lshift"));
    CHECK(!strcmp(padmap_target_label(PM_TARGET(PM_T_BUTTON, PM_LB)), "Left Bumper"));
    CHECK(!strcmp(padmap_target_label(0), "None"));
    uint8_t modes[8];
    CHECK(padmap_source_modes(PM_SRC_LTRIG, modes, 8) == 4 && modes[2] == PM_MODE_BUTTON);
}

int main(void)
{
    test_identity();
    test_southpaw_and_swap();
    test_shaping();
    test_dpad_modes();
    test_triggers();
    test_long_press();
    test_double_press();
    test_turbo_toggle();
    test_mode_shift_and_presets();
    test_keyboard_mouse();
    test_json_roundtrip();
    test_errors();
    test_store();
    test_labels();
    printf("padmap_test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
