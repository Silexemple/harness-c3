// UI: LVGL v9 screens for the 240×240 round panel. See ui.h and SPEC.md §6.
//
// Threading: cable_client event handlers run ON THE LINK READER TASK and must
// never touch LVGL — they push a small ui_ev_t onto s_ev_queue and return.
// The UI task (and only it) pops events and updates LVGL under display_lock().

#include "ui.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "buzzer.h"
#include "cable_client.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "ui";

// Inscribed square of a 240 px circle: keep critical text inside.
#define TEXT_BOX_W 170

// Low-saturation dark palette; ring colors per agent state (SPEC.md §6).
#define COL_BG       0x0b0f14
#define COL_TEXT     0xd7dde5
#define COL_DIM      0x8b98a5
#define COL_IDLE     0x5b6470   // grey
#define COL_RUNNING  0x3b82f6   // blue
#define COL_WAITING  0xf59e0b   // amber
#define COL_DONE     0x22c55e   // green
#define COL_ERROR    0xef4444   // red

// ── events from the link reader task ────────────────────────────────────────

typedef enum {
    UI_EV_SESSION,
    UI_EV_AGENTS,
    UI_EV_AGENT_EVENT,
    UI_EV_NOTIF,
    UI_EV_QUESTION,
    UI_EV_QUESTION_CLOSE,
    UI_EV_TOAST,
    UI_EV_FOCUS,
} ui_ev_type_t;

typedef struct {
    ui_ev_type_t type;
    union {
        struct { bool up; char name[NAME_MAX]; } session;
        struct { char id[ID_MAX]; char state[12]; bool notify, beep; } agent;
        struct { int count; int questions; } notif;
        struct { char text[104]; } toast;
        struct { char id[ID_MAX]; } focus;
    } d;
} ui_ev_t;

static QueueHandle_t s_btn_queue;
static QueueHandle_t s_ev_queue;

// ── UI state ────────────────────────────────────────────────────────────────

typedef enum { SCR_BOOT, SCR_OFFLINE, SCR_HOME, SCR_QUESTION } screen_t;

static screen_t s_screen = SCR_BOOT;
static bool     s_connected;
static char     s_machine_name[NAME_MAX];

static cable_agent_t s_agents[CABLE_MAX_AGENTS];
static int           s_agent_count;
static int           s_index;          // carousel position
static int           s_fleet_total;
static bool          s_pulse_ring;     // a question-type notif is outstanding

// Question screen state (the question itself lives in cable_client).
static int  s_q_index;                 // which question of the request
static int  s_q_sel;                   // cursor inside the option list
static bool s_q_multi_sel[CABLE_OPT_MAX];
static cJSON *s_answers;               // accumulated {key: label(s)}

// ── LVGL objects ────────────────────────────────────────────────────────────

static lv_obj_t *s_scr_boot, *s_scr_offline, *s_scr_home, *s_scr_question;

// home
static lv_obj_t *s_ring;        // circle object, border color = status ring
static lv_obj_t *s_home_header; // machine name
static lv_obj_t *s_home_name;   // agent name (Montserrat 20)
static lv_obj_t *s_home_sub;    // engine · machine (14)
static lv_obj_t *s_home_summary;// last summary line (14)
static lv_obj_t *s_home_page;   // "2/3"
static lv_obj_t *s_badge;       // fleet total badge
static lv_obj_t *s_badge_label;

// question
static lv_obj_t *s_q_header;    // agent name + Q n/m
static lv_obj_t *s_q_text;      // question prompt
static lv_obj_t *s_q_list;      // option buttons column
static lv_obj_t *s_q_hint;

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static lv_anim_t  s_pulse_anim;
static bool       s_pulse_running;

// ── colors ──────────────────────────────────────────────────────────────────

static uint32_t color_of_state(const char *state)
{
    if (strcmp(state, "running") == 0) return COL_RUNNING;
    if (strcmp(state, "waiting") == 0) return COL_WAITING;
    if (strcmp(state, "done") == 0) return COL_DONE;
    if (strcmp(state, "error") == 0) return COL_ERROR;
    return COL_IDLE;
}

// ── screen builders ─────────────────────────────────────────────────────────

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    return scr;
}

static lv_obj_t *new_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           uint32_t color, lv_align_t align, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, TEXT_BOX_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, align, 0, y);
    return l;
}

static void build_boot(const char *fw_version)
{
    s_scr_boot = new_screen();
    new_label(s_scr_boot, "λ", &lv_font_montserrat_28, COL_TEXT, LV_ALIGN_CENTER, -46);
    new_label(s_scr_boot, "Harness C3", &lv_font_montserrat_20, COL_TEXT, LV_ALIGN_CENTER, -4);
    char ver[48];
    snprintf(ver, sizeof(ver), "v%s", fw_version);
    new_label(s_scr_boot, ver, &lv_font_montserrat_14, COL_DIM, LV_ALIGN_CENTER, 30);
}

static void build_offline(void)
{
    s_scr_offline = new_screen();
    new_label(s_scr_offline, LV_SYMBOL_USB, &lv_font_montserrat_28, COL_DIM, LV_ALIGN_CENTER, -52);
    new_label(s_scr_offline, "Brancher sur un PC\navec Harness", &lv_font_montserrat_14,
              COL_TEXT, LV_ALIGN_CENTER, -8);
    new_label(s_scr_offline, "Plug into a Harness\ndaemon", &lv_font_montserrat_14,
              COL_DIM, LV_ALIGN_CENTER, 36);
}

static void build_home(void)
{
    s_scr_home = new_screen();

    // The status ring: a circle the size of the panel whose border carries
    // the agent state color. Everything else lives inside it.
    s_ring = lv_obj_create(s_scr_home);
    lv_obj_set_size(s_ring, 226, 226);
    lv_obj_center(s_ring);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring, 5, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(COL_IDLE), 0);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE);

    s_home_header = new_label(s_scr_home, "", &lv_font_montserrat_14, COL_DIM,
                              LV_ALIGN_TOP_MID, 36);
    s_home_name = new_label(s_scr_home, "", &lv_font_montserrat_20, COL_TEXT,
                            LV_ALIGN_CENTER, -38);
    s_home_sub = new_label(s_scr_home, "", &lv_font_montserrat_14, COL_DIM,
                           LV_ALIGN_CENTER, -6);
    s_home_summary = new_label(s_scr_home, "", &lv_font_montserrat_14, COL_TEXT,
                               LV_ALIGN_CENTER, 30);
    lv_label_set_long_mode(s_home_summary, LV_LABEL_LONG_DOT);   // ellipsized
    s_home_page = new_label(s_scr_home, "", &lv_font_montserrat_14, COL_DIM,
                            LV_ALIGN_BOTTOM_MID, -36);

    // Fleet badge (agents.end.total), top-right corner of the inscribed area.
    s_badge = lv_obj_create(s_scr_home);
    lv_obj_set_size(s_badge, 34, 34);
    lv_obj_set_style_radius(s_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(0x1f6feb), 0);
    lv_obj_set_style_border_width(s_badge, 0, 0);
    lv_obj_align(s_badge, LV_ALIGN_TOP_RIGHT, -32, 30);
    lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_badge_label = lv_label_create(s_badge);
    lv_obj_set_style_text_font(s_badge_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_badge_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(s_badge_label);
}

static void build_question(void)
{
    s_scr_question = new_screen();
    s_q_header = new_label(s_scr_question, "", &lv_font_montserrat_14, COL_WAITING,
                           LV_ALIGN_TOP_MID, 34);
    s_q_text = new_label(s_scr_question, "", &lv_font_montserrat_14, COL_TEXT,
                         LV_ALIGN_TOP_MID, 58);
    lv_obj_set_height(s_q_text, 56);

    // Options: a flex column of small buttons; selection is a border, not a
    // focus ring (there is no touch on this hardware).
    s_q_list = lv_obj_create(s_scr_question);
    lv_obj_set_size(s_q_list, TEXT_BOX_W, 96);
    lv_obj_align(s_q_list, LV_ALIGN_BOTTOM_MID, 0, -44);
    lv_obj_set_flex_flow(s_q_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_q_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_q_list, 4, 0);
    lv_obj_set_style_pad_all(s_q_list, 2, 0);
    lv_obj_set_style_bg_opa(s_q_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_q_list, 0, 0);
    lv_obj_remove_flag(s_q_list, LV_OBJ_FLAG_SCROLLABLE);

    s_q_hint = new_label(s_scr_question, "A: choisir · A long: OK · B: annuler",
                         &lv_font_montserrat_14, COL_DIM, LV_ALIGN_BOTTOM_MID, -22);
}

// ── ring pulse (a question is outstanding) ─────────────────────────────────

static void pulse_exec(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void ring_pulse_start(void)
{
    if (s_pulse_running) return;
    s_pulse_running = true;
    lv_anim_init(&s_pulse_anim);
    lv_anim_set_var(&s_pulse_anim, s_ring);
    lv_anim_set_values(&s_pulse_anim, LV_OPA_COVER, LV_OPA_40);
    lv_anim_set_duration(&s_pulse_anim, 700);
    lv_anim_set_playback_duration(&s_pulse_anim, 700);
    lv_anim_set_repeat_count(&s_pulse_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&s_pulse_anim, pulse_exec);
    lv_anim_start(&s_pulse_anim);
}

static void ring_pulse_stop(void)
{
    if (!s_pulse_running) return;
    s_pulse_running = false;
    lv_anim_delete(s_ring, pulse_exec);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
}

// ── rendering ───────────────────────────────────────────────────────────────

static void show_screen(screen_t scr)
{
    s_screen = scr;
    lv_obj_t *target = s_scr_boot;
    if (scr == SCR_OFFLINE) target = s_scr_offline;
    else if (scr == SCR_HOME) target = s_scr_home;
    else if (scr == SCR_QUESTION) target = s_scr_question;
    lv_screen_load(target);
}

static void render_home(void)
{
    if (s_agent_count == 0) {
        lv_obj_set_style_border_color(s_ring, lv_color_hex(COL_IDLE), 0);
        lv_label_set_text(s_home_header, s_machine_name);
        lv_label_set_text(s_home_name, cable_client_has_window()
                          ? "Rien sur cet onglet\nNothing on this tab"
                          : "Aucun agent\nNo agents");
        lv_label_set_text(s_home_sub, "");
        lv_label_set_text(s_home_summary, "");
        lv_label_set_text(s_home_page, "");
    } else {
        if (s_index >= s_agent_count) s_index = s_agent_count - 1;
        if (s_index < 0) s_index = 0;
        const cable_agent_t *a = &s_agents[s_index];
        lv_obj_set_style_border_color(s_ring, lv_color_hex(color_of_state(a->state)), 0);
        lv_label_set_text(s_home_header, s_machine_name);
        lv_label_set_text(s_home_name, a->name);
        char sub[NAME_MAX + 32];
        snprintf(sub, sizeof(sub), "%s%s%s%s%s", a->engine,
                 a->engine[0] && a->machine[0] ? " · " : "",
                 a->machine,
                 (a->engine[0] || a->machine[0]) && a->state[0] ? " · " : "",
                 a->state);
        lv_label_set_text(s_home_sub, sub);
        lv_label_set_text(s_home_summary, a->summary);
        char page[16];
        snprintf(page, sizeof(page), "%d/%d", s_index + 1, s_agent_count);
        lv_label_set_text(s_home_page, page);
    }

    char total[12];
    snprintf(total, sizeof(total), "%d", s_fleet_total);
    lv_label_set_text(s_badge_label, total);
    if (s_fleet_total > 0) lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    if (s_pulse_ring) ring_pulse_start();
    else ring_pulse_stop();
}

static void refresh_agents(void)
{
    s_agent_count = cable_client_list_agents(s_agents, CABLE_MAX_AGENTS);
    s_fleet_total = cable_client_agent_total();
    if (s_index >= s_agent_count) s_index = s_agent_count > 0 ? s_agent_count - 1 : 0;
    if (s_screen == SCR_HOME || s_screen == SCR_OFFLINE) render_home();
}

// ── question screen ─────────────────────────────────────────────────────────

static void render_question(void)
{
    const cable_question_t *q = cable_client_pending_question();
    if (!q || s_q_index >= q->count) return;
    const cable_question_item_t *item = &q->items[s_q_index];

    char header[NAME_MAX + 40];
    snprintf(header, sizeof(header), "%s · Q %d/%d", q->name, s_q_index + 1, q->count);
    lv_label_set_text(s_q_header, header);
    lv_label_set_text(s_q_text, item->q[0] ? item->q : item->key);

    lv_obj_clean(s_q_list);
    const int rows = item->opt_count + (item->multi ? 1 : 0);   // + "Done" row
    if (s_q_sel >= rows) s_q_sel = 0;
    for (int i = 0; i < rows; i++) {
        const bool is_done_row = item->multi && i == item->opt_count;
        lv_obj_t *b = lv_obj_create(s_q_list);
        lv_obj_set_size(b, TEXT_BOX_W - 8, 22);
        lv_obj_set_style_radius(b, 11, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        const bool selected = i == s_q_sel;
        const bool checked = !is_done_row && item->multi && s_q_multi_sel[i];
        lv_obj_set_style_bg_color(b, lv_color_hex(selected ? 0x22303e : 0x141b23), 0);
        lv_obj_set_style_border_width(b, selected ? 2 : 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(selected ? COL_RUNNING : 0x2b3642), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(checked ? COL_DONE : COL_TEXT), 0);
        if (is_done_row) lv_label_set_text(l, LV_SYMBOL_OK " Done");
        else lv_label_set_text_fmt(l, "%s%s", checked ? LV_SYMBOL_OK " " : "", item->options[i]);
        lv_obj_center(l);
    }
    lv_label_set_text(s_q_hint, item->multi
                      ? "A: naviguer · A long: cocher/OK · B: annuler"
                      : "A: choisir · A long: OK · B: annuler");
}

static void question_show(void)
{
    const cable_question_t *q = cable_client_pending_question();
    if (!q) return;
    s_q_index = 0;
    s_q_sel = 0;
    memset(s_q_multi_sel, 0, sizeof(s_q_multi_sel));
    if (s_answers) { cJSON_Delete(s_answers); s_answers = NULL; }
    s_answers = cJSON_CreateObject();
    render_question();
    show_screen(SCR_QUESTION);
}

static void question_finish(void)
{
    const cable_question_t *q = cable_client_pending_question();
    if (!q) return;
    // agent_id / request_id are read BEFORE the answer clears the pending
    // question, so keep copies.
    char agent_id[ID_MAX], request_id[ID_MAX];
    snprintf(agent_id, sizeof(agent_id), "%s", q->agent_id);
    snprintf(request_id, sizeof(request_id), "%s", q->request_id);
    if (!s_answers) s_answers = cJSON_CreateObject();
    cable_client_answer(agent_id, request_id, s_answers);
    cJSON_Delete(s_answers);
    s_answers = NULL;
    refresh_agents();
    show_screen(SCR_HOME);
    render_home();
}

static void question_confirm(void)
{
    const cable_question_t *q = cable_client_pending_question();
    if (!q || s_q_index >= q->count || !s_answers) return;
    const cable_question_item_t *item = &q->items[s_q_index];

    if (item->multi) {
        if (s_q_sel == item->opt_count) {
            // The "Done" row: join the checked labels with ", " (PROTOCOL.md
            // §4.14) and record the answer under the question's own key.
            char joined[CABLE_OPT_MAX * (CABLE_OPT_TEXT_MAX + 2)];
            joined[0] = '\0';
            for (int i = 0; i < item->opt_count; i++) {
                if (!s_q_multi_sel[i]) continue;
                if (joined[0]) strncat(joined, ", ", sizeof(joined) - strlen(joined) - 1);
                strncat(joined, item->options[i], sizeof(joined) - strlen(joined) - 1);
            }
            if (!joined[0]) return;   // nothing picked yet — stay
            cJSON_AddStringToObject(s_answers, item->key, joined);
        } else {
            s_q_multi_sel[s_q_sel] = !s_q_multi_sel[s_q_sel];
            render_question();
            return;
        }
    } else {
        if (s_q_sel >= item->opt_count) return;
        cJSON_AddStringToObject(s_answers, item->key, item->options[s_q_sel]);
    }

    if (s_q_index + 1 < q->count) {
        s_q_index++;
        s_q_sel = 0;
        memset(s_q_multi_sel, 0, sizeof(s_q_multi_sel));
        render_question();
    } else {
        question_finish();
    }
}

// ── toast ───────────────────────────────────────────────────────────────────

static void toast_delete(lv_timer_t *t)
{
    (void)t;
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
    s_toast_timer = NULL;
}

static void toast_show(const char *text)
{
    if (!text || !text[0]) return;
    if (s_toast) lv_obj_delete(s_toast);
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast, TEXT_BOX_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1c242e), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x2b3642), 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(s_toast);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, TEXT_BOX_W - 16);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(l);
    s_toast_timer = lv_timer_create(toast_delete, 2500, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

// ── cable_client event handlers (LINK READER TASK — queue and return) ──────

static bool push_ev(const ui_ev_t *ev)
{
    return s_ev_queue && xQueueSend(s_ev_queue, ev, 0) == pdTRUE;
}

static void on_session(bool up, const char *machine_name, void *ctx)
{
    (void)ctx;
    ui_ev_t ev = { .type = UI_EV_SESSION };
    ev.d.session.up = up;
    snprintf(ev.d.session.name, sizeof(ev.d.session.name), "%s", machine_name ? machine_name : "");
    push_ev(&ev);
}

static void on_agents_changed(void *ctx)
{
    (void)ctx;
    push_ev(&(ui_ev_t){ .type = UI_EV_AGENTS });
}

static void on_agent_event(const char *agent_id, const char *state, const char *text,
                           bool notify, bool beep, void *ctx)
{
    (void)text; (void)ctx;
    ui_ev_t ev = { .type = UI_EV_AGENT_EVENT };
    snprintf(ev.d.agent.id, sizeof(ev.d.agent.id), "%s", agent_id ? agent_id : "");
    snprintf(ev.d.agent.state, sizeof(ev.d.agent.state), "%s", state ? state : "");
    ev.d.agent.notify = notify;
    ev.d.agent.beep = beep;
    push_ev(&ev);
}

static void on_notif(const cable_notif_t *items, int count, void *ctx)
{
    (void)ctx;
    ui_ev_t ev = { .type = UI_EV_NOTIF };
    ev.d.notif.count = count;
    ev.d.notif.questions = 0;
    for (int i = 0; i < count; i++) if (items[i].question) ev.d.notif.questions++;
    push_ev(&ev);
}

static void on_question(const cable_question_t *q, void *ctx)
{
    (void)q; (void)ctx;   // the screen re-reads cable_client_pending_question()
    push_ev(&(ui_ev_t){ .type = UI_EV_QUESTION });
}

static void on_question_closed(void *ctx)
{
    (void)ctx;
    push_ev(&(ui_ev_t){ .type = UI_EV_QUESTION_CLOSE });
}

static void on_toast(const char *text, void *ctx)
{
    (void)ctx;
    ui_ev_t ev = { .type = UI_EV_TOAST };
    snprintf(ev.d.toast.text, sizeof(ev.d.toast.text), "%s", text ? text : "");
    push_ev(&ev);
}

static void on_focus(const char *agent_id, void *ctx)
{
    (void)ctx;
    ui_ev_t ev = { .type = UI_EV_FOCUS };
    snprintf(ev.d.focus.id, sizeof(ev.d.focus.id), "%s", agent_id ? agent_id : "");
    push_ev(&ev);
}

// ── event application (UI TASK ONLY) ────────────────────────────────────────

static void apply_ev(const ui_ev_t *ev)
{
    switch (ev->type) {
    case UI_EV_SESSION:
        s_connected = ev->d.session.up;
        snprintf(s_machine_name, sizeof(s_machine_name), "%s", ev->d.session.name);
        if (s_connected) {
            refresh_agents();
            show_screen(SCR_HOME);
            render_home();
        } else {
            s_agent_count = 0;
            s_fleet_total = 0;
            s_pulse_ring = false;
            if (s_screen != SCR_BOOT) show_screen(SCR_OFFLINE);
        }
        break;
    case UI_EV_AGENTS:
        refresh_agents();
        if (s_screen == SCR_HOME) render_home();
        break;
    case UI_EV_AGENT_EVENT:
        if (ev->d.agent.beep) buzzer_beep(BUZZER_BEEP_DONE);
        refresh_agents();
        if (s_screen == SCR_HOME) render_home();
        break;
    case UI_EV_NOTIF:
        s_pulse_ring = ev->d.notif.questions > 0;
        if (s_screen == SCR_HOME) render_home();
        break;
    case UI_EV_QUESTION:
        buzzer_beep(BUZZER_BEEP_QUESTION);
        question_show();
        break;
    case UI_EV_QUESTION_CLOSE:
        if (s_screen == SCR_QUESTION) {
            show_screen(SCR_HOME);
            render_home();
        }
        break;
    case UI_EV_TOAST:
        toast_show(ev->d.toast.text);
        break;
    case UI_EV_FOCUS:
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].id, ev->d.focus.id) == 0) {
                s_index = i;
                if (s_screen == SCR_HOME) render_home();
                break;
            }
        }
        break;
    }
}

// ── buttons (UI TASK ONLY) ──────────────────────────────────────────────────

static void handle_button(btn_event_t ev)
{
    switch (s_screen) {
    case SCR_HOME:
        if (ev == BTN_EVENT_A_SHORT && s_agent_count > 0) {
            s_index = (s_index + 1) % s_agent_count;
            render_home();
            // The tile the carousel settled on — where the user is LOOKING.
            cable_client_send_focus(s_agents[s_index].id);
        } else if (ev == BTN_EVENT_A_LONG && s_agent_count > 0) {
            cable_client_send_open(s_agents[s_index].id, NULL);   // a person's tap
        }
        break;
    case SCR_QUESTION:
        if (ev == BTN_EVENT_A_SHORT) {
            const cable_question_t *q = cable_client_pending_question();
            if (q && s_q_index < q->count) {
                const cable_question_item_t *item = &q->items[s_q_index];
                const int rows = item->opt_count + (item->multi ? 1 : 0);
                if (rows > 0) {
                    s_q_sel = (s_q_sel + 1) % rows;
                    render_question();
                }
            }
        } else if (ev == BTN_EVENT_A_LONG) {
            question_confirm();
        } else if (ev == BTN_EVENT_B_SHORT) {
            // Dismiss without answering — no message (SPEC.md §6). The
            // question stays pending; question.close (or an answer) is the
            // only way it truly leaves.
            show_screen(SCR_HOME);
            render_home();
        }
        break;
    case SCR_OFFLINE:
    case SCR_BOOT:
    default:
        break;
    }
    // BTN_B from anywhere else: back to home.
    if (ev == BTN_EVENT_B_SHORT && s_screen != SCR_HOME && s_screen != SCR_QUESTION
        && s_connected) {
        show_screen(SCR_HOME);
        render_home();
    }
}

// ── boot timer ──────────────────────────────────────────────────────────────

static void boot_timeout(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (!s_connected && s_screen == SCR_BOOT) show_screen(SCR_OFFLINE);
}

// ── UI task ─────────────────────────────────────────────────────────────────

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (display_lock(100)) {
            ui_ev_t ev;
            while (xQueueReceive(s_ev_queue, &ev, 0) == pdTRUE) apply_ev(&ev);
            btn_event_t btn;
            while (xQueueReceive(s_btn_queue, &btn, 0) == pdTRUE) handle_button(btn);
            lv_timer_handler();
            display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── init ────────────────────────────────────────────────────────────────────

QueueHandle_t ui_button_queue(void) { return s_btn_queue; }

bool ui_init(const char *fw_version)
{
    s_btn_queue = xQueueCreate(8, sizeof(btn_event_t));
    s_ev_queue = xQueueCreate(8, sizeof(ui_ev_t));
    if (!s_btn_queue || !s_ev_queue) {
        ESP_LOGE(TAG, "no memory for UI queues");
        return false;
    }

    if (!display_lock(1000)) return false;
    build_boot(fw_version ? fw_version : "?");
    build_offline();
    build_home();
    build_question();
    show_screen(SCR_BOOT);
    lv_timer_create(boot_timeout, 1200, NULL);
    display_unlock();

    cable_client_set_ui(&(cable_client_ui_t){
        .session = on_session,
        .agents_changed = on_agents_changed,
        .agent_event = on_agent_event,
        .notif = on_notif,
        .question = on_question,
        .question_closed = on_question_closed,
        .toast = on_toast,
        .focus = on_focus,
    });

    if (xTaskCreate(ui_task, "ui", 6144, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed");
        return false;
    }
    ESP_LOGI(TAG, "ui up (fw %s)", fw_version ? fw_version : "?");
    return true;
}
