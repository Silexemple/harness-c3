// Host UI simulator: runs the REAL firmware ui.c on LVGL's software renderer
// and dumps exact 240×240 frames as PPM images (one per scenario step).
//
//   ./uisim /tmp/frames
//
// produces /tmp/frames/01_boot.ppm … /tmp/frames/07_home_done_toast.ppm.
//
// What is real and what is simulated:
//   REAL       — ui.c (every pixel layout, color, font, text), LVGL v9.2
//                with the device's trimmed config (Montserrat 14/20/28 only,
//                16-bit RGB565, 32 KB LV pool, same draw-buffer geometry).
//   SIMULATED  — the cable_client backend (in-memory agent/question store,
//                sim_cable.c) and the FreeRTOS/display/buzzer platform
//                (pthreads + a mutex, sim_freertos.c).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "ui.h"
#include "buttons.h"
#include "cable_client.h"
#include "display.h"   // display_lock/unlock (implemented in sim_freertos.c)

#define W 240
#define H 240

// The rendered panel content, rebuilt by the flush callback.
static uint16_t s_fb[W * H];

// Same geometry as the device: two 240×48 RGB565 buffers, partial refresh.
static uint16_t s_buf1[W * 48];
static uint16_t s_buf2[W * 48];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const uint16_t *src = (const uint16_t *)px_map;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        memcpy(&s_fb[y * W + area->x1], src, (size_t)w * 2);
        src += w;
    }
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Let the UI task drain its queues and render (it runs every 10 ms).
static void pump(int ms)
{
    usleep((useconds_t)ms * 1000);
}

static void snap(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    display_lock(1000);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) {
            uint16_t px = s_fb[i];
            uint8_t rgb[3] = {
                (uint8_t)(((px >> 11) & 0x1f) * 255 / 31),
                (uint8_t)(((px >> 5) & 0x3f) * 255 / 63),
                (uint8_t)((px & 0x1f) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        fprintf(stderr, "[sim] wrote %s\n", path);
    }
    display_unlock();
}

static void press(btn_event_t ev)
{
    xQueueSend(ui_button_queue(), &ev, 0);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "/tmp/frames";

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_buf1, s_buf2, sizeof(s_buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    if (!ui_init("0.1.0-c3")) {
        fprintf(stderr, "ui_init failed\n");
        return 1;
    }

    // ── scenario: a work morning on the dial ────────────────────────────────

    pump(150);                       // LVGL settle + first render
    snap(out, "01_boot");

    pump(1200);                      // boot timer expires, no session yet
    snap(out, "02_offline");

    // Daemon says welcome (machine "studio-mbp") and streams the tab's agents.
    static cable_agent_t agents[3];
    snprintf(agents[0].id, ID_MAX, "9f3a-claude-01");
    snprintf(agents[0].name, NAME_MAX, "Claude Code");
    snprintf(agents[0].engine, 12, "claude");
    snprintf(agents[0].state, 12, "running");
    snprintf(agents[0].summary, 100, "Refactor cable_client.c - 3 fichiers");
    snprintf(agents[0].machine, NAME_MAX, "studio-mbp");

    snprintf(agents[1].id, ID_MAX, "9f3a-codex-02");
    snprintf(agents[1].name, NAME_MAX, "Codex CLI");
    snprintf(agents[1].engine, 12, "codex");
    snprintf(agents[1].state, 12, "waiting");
    snprintf(agents[1].summary, 100, "En attente : choix du framework");
    snprintf(agents[1].machine, NAME_MAX, "studio-mbp");

    snprintf(agents[2].id, ID_MAX, "9f3a-gemini-03");
    snprintf(agents[2].name, NAME_MAX, "Gemini CLI");
    snprintf(agents[2].engine, 12, "gemini");
    snprintf(agents[2].state, 12, "done");
    snprintf(agents[2].summary, 100, "Suite de tests : 344 checks verts");
    snprintf(agents[2].machine, NAME_MAX, "vps-build");

    sim_cable_set_agents(agents, 3, 5);   // fleet badge: 5 account-wide
    sim_cable_session(true, "studio-mbp");
    pump(150);
    snap(out, "03_home_running");

    // One tap: carousel to the waiting agent (amber ring).
    press(BTN_EVENT_A_SHORT);
    pump(150);
    snap(out, "04_home_waiting");

    // Codex stops to ask: single-choice question, 1 of 2.
    cable_question_t q;
    memset(&q, 0, sizeof(q));
    snprintf(q.agent_id, ID_MAX, "9f3a-codex-02");
    snprintf(q.request_id, ID_MAX, "req-7c21");
    snprintf(q.name, NAME_MAX, "Codex CLI");
    snprintf(q.machine, NAME_MAX, "studio-mbp");
    q.count = 2;
    snprintf(q.items[0].key, CABLE_Q_KEY_MAX, "framework");
    snprintf(q.items[0].q, CABLE_Q_TEXT_MAX, "Quel framework de test ?");
    snprintf(q.items[0].options[0], CABLE_OPT_TEXT_MAX, "Unity");
    snprintf(q.items[0].options[1], CABLE_OPT_TEXT_MAX, "CMocka");
    snprintf(q.items[0].options[2], CABLE_OPT_TEXT_MAX, "GoogleTest");
    q.items[0].opt_count = 3;
    q.items[0].multi = false;
    snprintf(q.items[1].key, CABLE_Q_KEY_MAX, "coverage");
    snprintf(q.items[1].q, CABLE_Q_TEXT_MAX, "Activer la couverture de code ?");
    snprintf(q.items[1].options[0], CABLE_OPT_TEXT_MAX, "Oui");
    snprintf(q.items[1].options[1], CABLE_OPT_TEXT_MAX, "Non");
    q.items[1].opt_count = 2;
    q.items[1].multi = false;

    sim_cable_set_question(&q, true);
    sim_cable_question();
    pump(150);
    press(BTN_EVENT_A_SHORT);        // cursor onto "CMocka"
    pump(150);
    snap(out, "05_question_single");

    sim_cable_question_closed();
    pump(150);

    // A multi-select question with a checked row and the "Done" row focused.
    memset(&q, 0, sizeof(q));
    snprintf(q.agent_id, ID_MAX, "9f3a-claude-01");
    snprintf(q.request_id, ID_MAX, "req-7d02");
    snprintf(q.name, NAME_MAX, "Claude Code");
    snprintf(q.machine, NAME_MAX, "studio-mbp");
    q.count = 1;
    snprintf(q.items[0].key, CABLE_Q_KEY_MAX, "steps");
    snprintf(q.items[0].q, CABLE_Q_TEXT_MAX, "Quelles taches lancer ?");
    snprintf(q.items[0].options[0], CABLE_OPT_TEXT_MAX, "lint");
    snprintf(q.items[0].options[1], CABLE_OPT_TEXT_MAX, "build");
    snprintf(q.items[0].options[2], CABLE_OPT_TEXT_MAX, "tests");
    q.items[0].opt_count = 3;
    q.items[0].multi = true;

    sim_cable_set_question(&q, true);
    sim_cable_question();
    pump(150);
    press(BTN_EVENT_A_SHORT);        // move to "build"
    pump(100);
    press(BTN_EVENT_A_LONG);         // check "build"
    pump(100);
    press(BTN_EVENT_A_SHORT);        // "tests"
    press(BTN_EVENT_A_SHORT);        // "Done" row
    pump(150);
    snap(out, "06_question_multi");

    sim_cable_question_closed();
    pump(150);

    // Carousel to the finished agent and a toast lands on top.
    press(BTN_EVENT_A_SHORT);
    pump(100);
    sim_cable_toast("turn.done - Gemini CLI : reponse prete");
    pump(200);
    snap(out, "07_home_done_toast");

    pump(3000);                      // toast expires — nothing else to shoot

    // Empty tab: session still up but no agent staged on this window.
    sim_cable_set_agents(agents, 0, 0);
    sim_cable_agents_changed();
    pump(150);
    snap(out, "08_home_empty");
    return 0;
}
