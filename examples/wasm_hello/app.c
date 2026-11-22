/*
 * Clock — a managed (WASM) app that fits in a few KB.
 *
 * It shows the whole managed model: a retained tree built once, nodes updated
 * with set_text inside begin/end, clicks arriving as events, and a tick that
 * keeps the clock current. Compare with examples/native_hello, which needs a
 * reboot to start and one to leave.
 */
#include "frame_app.h"

/* Minimal freestanding bits clang may call behind our back. */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) {
        *dp++ = *sp++;
    }
    return d;
}

static frame_node_t s_time;
static frame_node_t s_count;
static frame_node_t s_bar;
static frame_node_t s_plus;
static frame_node_t s_exit;
static int s_clicks;

static void two(char *out, int v)
{
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

/* "h:mm:ss" from the 32-bit millisecond counter (wraps after ~49 days). */
static void format_time(char out[10], uint32_t ms)
{
    const uint32_t secs = ms / 1000u;
    const unsigned h = (secs / 3600u) % 10u;
    const unsigned m = (secs / 60u) % 60u;
    const unsigned s = secs % 60u;
    out[0] = (char)('0' + h);
    out[1] = ':';
    two(out + 2, (int)m);
    out[4] = ':';
    two(out + 5, (int)s);
    out[7] = '\0';
}

static void redraw(void)
{
    char t[10];
    format_time(t, frame_now_ms());
    char c[24] = "clicked 0 times";
    c[8] = (char)('0' + (s_clicks % 10));
    char pct[4];
    pct[0] = (char)('0' + (s_clicks / 10) % 10);
    pct[1] = '\0';

    frame_ui_begin();
    frame_ui_set_text(s_time, t);
    frame_ui_set_text(s_count, c);
    frame_ui_set_text(s_bar, pct);       /* a progress bar takes its value as text */
    frame_ui_end();
}

void FRAME_EXPORT("frame_app_main") frame_app_main(void)
{
    frame_ui_begin();
    frame_ui_title("Clock");
    s_time = frame_ui_node(0, FRAME_NODE_LABEL,
                           "text_font=28;text_color=#2DD4BF;align=center;width=fill");
    frame_ui_set_text(s_time, "0:00:00");
    frame_node_t row = frame_ui_node(0, FRAME_NODE_ROW, "align=center;height=80;width=fill");
    s_plus = frame_ui_node(row, FRAME_NODE_BUTTON, "bg=#1A212B;pad=14");
    frame_ui_set_text(s_plus, "+1");
    s_exit = frame_ui_node(row, FRAME_NODE_BUTTON, "bg=#1A212B;pad=14");
    frame_ui_set_text(s_exit, "Exit");
    s_count = frame_ui_node(0, FRAME_NODE_LABEL, "text_font=20;align=center");
    frame_ui_set_text(s_count, "clicked 0 times");
    s_bar = frame_ui_node(0, FRAME_NODE_PROGRESS, "width=300;align=center;height=14");
    frame_ui_set_text(s_bar, "0");
    frame_ui_end();
    redraw();
}

void FRAME_EXPORT("frame_app_event") frame_app_event(int type, int a, int b)
{
    (void)b;
    if (type == FRAME_EVT_STOP) {
        frame_log_i("clock", "stopping");
        return;
    }
    if (type != FRAME_EVT_CLICK) {
        return;
    }
    if (a == s_exit) {
        frame_app_exit();
        return;
    }
    if (a == s_plus) {
        s_clicks++;
        redraw();
    }
}

void FRAME_EXPORT("frame_app_tick") frame_app_tick(void)
{
    redraw();
}
