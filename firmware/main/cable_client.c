// Message layer of the Harness "cable" protocol. See cable_client.h and
// PROTOCOL.md (repository root) — the protocol document wins any disagreement.
//
// Derived from devices/harness-device/firmware/main/cable_client.c of
// autonomous-ai/openharness (MIT), trimmed to the v1 Harness-C3 vocabulary:
// no voice, no machine wheel, no swarms, no scroll, no models, and fw.offer is
// never answered (SPEC.md §9 — upstream offers ESP32-S3 images).

#include "cable_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cable_frame.h"
#include "cable_link.h"

#ifdef CABLE_HOST_TEST
// Host build: logging is a no-op (but a real call, so arguments stay "used"
// under -Wall -Wextra), locking is single-threaded, no FreeRTOS.
static void host_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}
#define ESP_LOGI(tag, ...)      host_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...)      host_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...)      host_log(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...)      host_log(tag, __VA_ARGS__)
#define CLIENT_LOCK()           ((void)0)
#define CLIENT_UNLOCK()         ((void)0)
#else
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static SemaphoreHandle_t s_lock;
#define CLIENT_LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define CLIENT_UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)
#endif

static const char *TAG = "cable_client";

// ── state ───────────────────────────────────────────────────────────────────

static char s_fw[40]  = "0.0.0-c3";
static char s_hw[24]  = CABLE_HW_NAME;
static char s_mac[24] = "00:00:00:00:00:00";

static cable_agent_t s_agents[CABLE_MAX_AGENTS];
static int           s_agent_count;
static bool          s_agents_building;
static int           s_agents_total;
static bool          s_has_window;

static bool     s_session;
static uint32_t s_last_rx_ms;
static uint32_t s_next_hello_ms;
static char     s_machine_name[CABLE_NAME_MAX];
static char     s_machine_id[ID_MAX];
static char     s_selected[ID_MAX];

static cable_question_t s_question;
static bool             s_question_pending;

static uint32_t s_bad, s_unknown;

static cable_client_ui_t s_ui;

// ── helpers ─────────────────────────────────────────────────────────────────

static void ui_call0(void (*fn)(void *))
{
    if (fn) fn(s_ui.ctx);
}

static const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static bool bool_of(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

// ── outbound ────────────────────────────────────────────────────────────────

// Serialise and send. Takes ownership of `root` — every caller builds a tree
// for exactly one message, and making the send own it keeps an early return
// from leaking one. cJSON does the escaping, which is not optional: an agent
// name can carry quotes, backslashes and newlines.
static bool send_json(cJSON *root)
{
    if (!root) return false;
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return false;
    const bool ok = cable_link_send(CABLE_TYPE_JSON, (const uint8_t *)text, strlen(text));
    cJSON_free(text);
    return ok;
}

static cJSON *msg(const char *t)
{
    cJSON *root = cJSON_CreateObject();
    if (root) cJSON_AddStringToObject(root, "t", t);
    return root;
}

static void send_hello(void)
{
    cJSON *root = msg("hello");
    if (!root) return;
    // Field order is the construction order of PROTOCOL.md §3.2.
    cJSON_AddStringToObject(root, "fw", s_fw);
    cJSON_AddStringToObject(root, "product", CABLE_PRODUCT);   // positive match — see header
    cJSON_AddNumberToObject(root, "proto", CABLE_PROTO_VERSION);
    cJSON_AddStringToObject(root, "hw", s_hw);
    cJSON_AddStringToObject(root, "mac", s_mac);
    send_json(root);
}

void cable_client_request_agents(void)
{
    send_json(msg("agents.list"));
}

void cable_client_send_focus(const char *agent_id)
{
    if (!agent_id || !agent_id[0]) return;
    cJSON *root = msg("focus");
    if (!root) return;
    cJSON_AddStringToObject(root, "agentId", agent_id);
    send_json(root);
}

void cable_client_send_open(const char *agent_id, const char *reason)
{
    if (!agent_id || !agent_id[0]) return;
    cJSON *root = msg("agent.open");
    if (!root) return;
    cJSON_AddStringToObject(root, "agentId", agent_id);
    // Absent for a tap: an older daemon reads the frame exactly as before.
    if (reason && reason[0]) cJSON_AddStringToObject(root, "reason", reason);
    send_json(root);
}

void cable_client_answer(const char *agent_id, const char *request_id, const cJSON *answers)
{
    if (!agent_id || !request_id || !answers) return;
    cJSON *root = msg("answer");
    if (!root) return;
    cJSON_AddStringToObject(root, "agentId", agent_id);
    // The daemon's opaque id, echoed byte-for-byte. `answers` is the UI's own
    // object and travels verbatim — re-deriving it is how a rename becomes an
    // answer nobody gave. Duplicated, not adopted: the caller keeps its tree.
    cJSON_AddStringToObject(root, "requestId", request_id);
    cJSON *copy = cJSON_Duplicate(answers, true);
    if (!copy) { cJSON_Delete(root); return; }
    cJSON_AddItemToObject(root, "answers", copy);
    send_json(root);

    CLIENT_LOCK();
    if (s_question_pending && strcmp(s_question.request_id, request_id) == 0) {
        s_question_pending = false;
    }
    CLIENT_UNLOCK();
}

// ── agent store ─────────────────────────────────────────────────────────────

static cable_agent_t *find_agent(const char *agent_id)
{
    if (!agent_id) return NULL;
    for (int i = 0; i < s_agent_count; i++) {
        if (strcmp(s_agents[i].id, agent_id) == 0) return &s_agents[i];
    }
    return NULL;
}

static void set_agent_state(const char *agent_id, const char *state, const char *text,
                            bool notify, bool beep)
{
    CLIENT_LOCK();
    cable_agent_t *a = find_agent(agent_id);
    if (a) {
        copy_str(a->state, sizeof(a->state), state);
        if (text && text[0]) copy_str(a->summary, sizeof(a->summary), text);
    }
    CLIENT_UNLOCK();
    if (s_ui.agent_event) s_ui.agent_event(agent_id, state, text ? text : "", notify, beep, s_ui.ctx);
}

static void clear_agents(void)
{
    CLIENT_LOCK();
    s_agent_count = 0;
    s_agents_building = false;
    s_agents_total = 0;
    s_has_window = false;
    CLIENT_UNLOCK();
    ui_call0(s_ui.agents_changed);
}

int cable_client_list_agents(cable_agent_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    CLIENT_LOCK();
    for (int i = 0; i < s_agent_count && n < max; i++) {
        out[n] = s_agents[i];
        n++;
    }
    CLIENT_UNLOCK();
    return n;
}

int cable_client_agent_total(void)
{
    CLIENT_LOCK();
    const int n = s_agents_total;
    CLIENT_UNLOCK();
    return n;
}

bool cable_client_has_window(void)
{
    CLIENT_LOCK();
    const bool w = s_has_window;
    CLIENT_UNLOCK();
    return w;
}

// ── session ─────────────────────────────────────────────────────────────────

static void session_up(const cJSON *p)
{
    const cJSON *machine = p ? cJSON_GetObjectItemCaseSensitive(p, "machine") : NULL;
    const char *name = str_of(machine, "name");
    copy_str(s_machine_name, sizeof(s_machine_name), name ? name : "Machine");
    // The CABLED computer's identity, and what `selected` falls back to.
    const char *mid = str_of(machine, "id");
    if (mid) copy_str(s_machine_id, sizeof(s_machine_id), mid);
    const char *selected = str_of(p, "selected");
    copy_str(s_selected, sizeof(s_selected), selected && selected[0] ? selected : s_machine_id);

    const bool was = s_session;
    s_session = true;
    if (s_ui.session) s_ui.session(true, s_machine_name, s_ui.ctx);
    if (!was) {
        // Route the log through the link only once a peer is listening;
        // unplugged, the port stays an ordinary console for idf.py monitor.
        cable_link_set_log_framing(true);
        ESP_LOGI(TAG, "session up: %s (proto %d)", s_machine_name,
                 (int)(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(p, "proto"))
                           ? cJSON_GetObjectItemCaseSensitive(p, "proto")->valuedouble : 0));
        // ASK EXPLICITLY. A rebooted dial greets with the same mac/fw, which
        // the daemon reads as a keepalive and pushes nothing.
        send_json(msg("agents.list"));
    }
}

static void session_down(const char *why)
{
    if (!s_session) return;
    s_session = false;

    // FORGET THE AGENTS. A tile is a claim that something is running right
    // now, and with the daemon gone nothing on this side can vouch for it.
    // s_selected is kept deliberately: a session that comes back on the same
    // computer should land where it was.
    clear_agents();
    s_machine_name[0] = '\0';
    s_machine_id[0] = '\0';
    CLIENT_LOCK();
    s_question_pending = false;
    CLIENT_UNLOCK();
    // Restore the plain console FIRST: whatever follows should be readable by
    // a developer with a serial monitor — exactly this situation.
    cable_link_set_log_framing(false);
    ESP_LOGI(TAG, "session down (%s)", why);
    if (s_ui.session) s_ui.session(false, "", s_ui.ctx);
    ui_call0(s_ui.question_closed);
}

// ── inbound handlers ────────────────────────────────────────────────────────

static void handle_agent(const cJSON *p)
{
    const char *id = str_of(p, "id");
    if (!id || !id[0]) return;   // rows with a missing/empty id are dropped
    CLIENT_LOCK();
    if (s_agents_building && s_agent_count < CABLE_MAX_AGENTS) {
        cable_agent_t *a = &s_agents[s_agent_count++];
        memset(a, 0, sizeof(*a));
        copy_str(a->id, sizeof(a->id), id);
        const char *name = str_of(p, "name");
        copy_str(a->name, sizeof(a->name), name && name[0] ? name : id);  // falls back to id
        copy_str(a->engine, sizeof(a->engine), str_of(p, "engine"));
        copy_str(a->machine_id, sizeof(a->machine_id), str_of(p, "machineId"));
        copy_str(a->machine, sizeof(a->machine), str_of(p, "machine"));
        copy_str(a->state, sizeof(a->state), "idle");
    }
    CLIENT_UNLOCK();
}

// The list arrives STREAMED — begin, one message per agent, end — because a
// frame is capped at CABLE_MAX_PAYLOAD and one agent per message bounds the
// length by construction.
static void handle_agents_begin(void)
{
    CLIENT_LOCK();
    s_agent_count = 0;
    s_agents_building = true;
    CLIENT_UNLOCK();
}

static void handle_agents_end(const cJSON *p)
{
    CLIENT_LOCK();
    s_agents_building = false;
    const int n = s_agent_count;
    // `total` is the account-wide fleet count (the badge); `tab` names the
    // active tab and is "" when no window is open — how an empty list reads
    // as "the app is shut" rather than "this tab is empty".
    const cJSON *total = p ? cJSON_GetObjectItemCaseSensitive(p, "total") : NULL;
    const char *tab = p ? str_of(p, "tab") : NULL;
    s_agents_total = cJSON_IsNumber(total) ? (int)total->valuedouble : n;
    s_has_window = tab && tab[0];
    CLIENT_UNLOCK();
    ESP_LOGI(TAG, "agents: %d of %d%s", n, s_agents_total, s_has_window ? "" : " (no window)");
    ui_call0(s_ui.agents_changed);
}

static void handle_notif_replace(const cJSON *p)
{
    static cable_notif_t rows[CABLE_NOTIF_MAX];   // static: reader-task stack is bounded
    int n = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(p, "items")) {
        if (n >= CABLE_NOTIF_MAX) break;
        const char *aid = str_of(it, "agentId");
        if (!aid || !aid[0]) continue;   // agentId is required
        memset(&rows[n], 0, sizeof(rows[n]));
        copy_str(rows[n].agent_id, sizeof(rows[n].agent_id), aid);
        copy_str(rows[n].name, sizeof(rows[n].name), str_of(it, "name"));
        copy_str(rows[n].machine, sizeof(rows[n].machine), str_of(it, "machine"));
        copy_str(rows[n].summary, sizeof(rows[n].summary), str_of(it, "summary"));
        rows[n].question = bool_of(it, "question");
        n++;
    }
    if (s_ui.notif) s_ui.notif(rows, n, s_ui.ctx);
}

static void handle_question(const cJSON *p, const char *agent_id)
{
    const char *req_id = str_of(p, "id");
    const cJSON *questions = cJSON_GetObjectItemCaseSensitive(p, "questions");
    if (!agent_id || !req_id || !req_id[0] || !cJSON_IsArray(questions)) return;

    CLIENT_LOCK();
    // Dedup on id: the streaming and complete paths share the request id, and
    // a reconnect re-shows every unanswered question. One beep, one screen.
    if (s_question_pending && strcmp(s_question.request_id, req_id) == 0) {
        CLIENT_UNLOCK();
        return;
    }
    memset(&s_question, 0, sizeof(s_question));
    copy_str(s_question.agent_id, sizeof(s_question.agent_id), agent_id);
    copy_str(s_question.request_id, sizeof(s_question.request_id), req_id);
    copy_str(s_question.name, sizeof(s_question.name), str_of(p, "name"));
    copy_str(s_question.machine, sizeof(s_question.machine), str_of(p, "machine"));
    const cJSON *qi = NULL;
    cJSON_ArrayForEach(qi, questions) {
        if (s_question.count >= CABLE_Q_MAX) break;
        cable_question_item_t *item = &s_question.items[s_question.count];
        copy_str(item->key, sizeof(item->key), str_of(qi, "key"));
        copy_str(item->q, sizeof(item->q), str_of(qi, "q"));
        item->multi = bool_of(qi, "multi");
        const cJSON *opt = NULL;
        cJSON_ArrayForEach(opt, cJSON_GetObjectItemCaseSensitive(qi, "options")) {
            if (item->opt_count >= CABLE_OPT_MAX) break;
            if (!cJSON_IsString(opt)) continue;
            copy_str(item->options[item->opt_count], sizeof(item->options[0]), opt->valuestring);
            item->opt_count++;
        }
        s_question.count++;
    }
    s_question_pending = true;
    cable_agent_t *a = find_agent(agent_id);
    if (a) copy_str(a->state, sizeof(a->state), "waiting");
    CLIENT_UNLOCK();

    // A question is a job, not news: wake, beep, show — and ask the window to
    // bring the agent forward ONLY if it is already on screen.
    cable_client_send_open(agent_id, "question");
    if (s_ui.question) s_ui.question(&s_question, s_ui.ctx);
}

static void handle_question_close(const char *agent_id, const cJSON *p)
{
    (void)agent_id;
    const char *id = str_of(p, "id");
    if (!id) return;
    // Carries `id` so a dial showing a DIFFERENT question cannot be closed by
    // a stale message.
    CLIENT_LOCK();
    const bool match = s_question_pending && strcmp(s_question.request_id, id) == 0;
    if (match) s_question_pending = false;
    CLIENT_UNLOCK();
    if (match) ui_call0(s_ui.question_closed);
}

static void handle_summary(const cJSON *p, const char *agent_id)
{
    if (!agent_id) return;
    const bool restore = bool_of(p, "restore");   // history refill, not news
    const bool quiet   = bool_of(p, "quiet");     // desktop already shows it
    const bool silent  = bool_of(p, "silent");    // a sub-agent's turn
    const char *recap  = str_of(p, "recap");
    const char *text   = str_of(p, "text");
    const char *line   = recap && recap[0] ? recap : (text ? text : "");
    // The beep and the notification hang off the SUMMARY, never the bare
    // `done` — an empty/phantom turn emits only `done` and stays silent.
    // Absent flags read as false, so an older daemon notifies as before.
    set_agent_state(agent_id, "done", line, !restore && !quiet && !silent, !restore && !silent);
}

static void handle_message(const cJSON *root)
{
    const char *t = str_of(root, "t");
    if (!t) { s_bad++; return; }
    // Fields may nest under "p", but flat messages are equally legal.
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
    if (!p) p = root;

    if (strcmp(t, "welcome") == 0) { session_up(p); return; }
    if (strcmp(t, "ping") == 0) { send_json(msg("pong")); return; }
    if (strcmp(t, "agents.begin") == 0) { handle_agents_begin(); return; }
    if (strcmp(t, "agent") == 0) { handle_agent(p); return; }
    if (strcmp(t, "agents.end") == 0) { handle_agents_end(p); return; }
    if (strcmp(t, "notif.replace") == 0) { handle_notif_replace(p); return; }

    const char *agent_id = str_of(p, "agentId");

    if (strcmp(t, "focus") == 0) {
        // The daemon's surface moved — follow it, so the two screens are one desk.
        if (agent_id && s_ui.focus) s_ui.focus(agent_id, s_ui.ctx);
        return;
    }
    if (strcmp(t, "agent.updated") == 0) {
        // RESERVED, never sent by the current daemon: treat as "re-fetch".
        send_json(msg("agents.list"));
        return;
    }
    if (strcmp(t, "turn.started") == 0) {
        // `text` is the status line the tile draws; dropping it leaves a live
        // turn reading exactly like a turn that never started.
        if (agent_id) set_agent_state(agent_id, "running", str_of(p, "text"), false, false);
        return;
    }
    if (strcmp(t, "turn.done") == 0) {
        // Stops the spinner and deliberately does NOT ring: completion is
        // announced by the summary that follows.
        if (agent_id) set_agent_state(agent_id, "done", NULL, false, false);
        return;
    }
    if (strcmp(t, "turn.error") == 0) {
        const char *message = str_of(p, "message");
        if (agent_id) set_agent_state(agent_id, "error", message, false, false);
        if (s_ui.toast) s_ui.toast(message ? message : "", s_ui.ctx);
        return;
    }
    if (strcmp(t, "summary") == 0) { handle_summary(p, agent_id); return; }
    if (strcmp(t, "question") == 0) { handle_question(p, agent_id); return; }
    if (strcmp(t, "question.close") == 0) { handle_question_close(agent_id, p); return; }
    if (strcmp(t, "toast") == 0) {
        if (s_ui.toast) s_ui.toast(str_of(p, "text") ? str_of(p, "text") : "", s_ui.ctx);
        return;
    }
    if (strcmp(t, "fw.offer") == 0) {
        // SPEC.md §9 (anti-brick): NEVER answer. Upstream offers ESP32-S3
        // images; accepting one would flash the wrong architecture. Declining
        // is silence — the daemon offers again on the next hello, and the
        // dual-OTA plumbing stays available to a future C3-aware updater.
        // One log line per offer (a LOG frame while the session is live).
        ESP_LOGI(TAG, "fw offer ignored: upstream images target esp32s3");
        return;
    }

    // A message this build has no case for (voice.*, machines.*, swarms,
    // models, …): counted, never fatal — a daemon running ahead of this
    // firmware is a version mismatch someone can act on, not a fault.
    s_unknown++;
    ESP_LOGD(TAG, "unhandled message '%s'", t);
}

void cable_client_handle_frame(uint8_t version, uint8_t type,
                               const uint8_t *payload, size_t payload_len,
                               void *ctx)
{
    (void)version;
    (void)ctx;
    // ANY inbound frame refreshes the silence window — the daemon's 5 s ping
    // cadence alone holds the session up.
    s_last_rx_ms = cable_platform_millis();

    if (type != CABLE_TYPE_JSON) {
        // PCM travels the other way, FW slices are never accepted (SPEC §9);
        // anything else is a peer that knows a payload kind this build does
        // not. Counted, handed up by the framing layer, never fatal.
        s_unknown++;
        return;
    }

    // cJSON needs a NUL-terminated string and the payload is not one. Copied
    // rather than parsed in place: the decoder's buffer is reused on return.
    if (payload_len == 0 || payload_len >= CABLE_JSON_MAX) { s_bad++; return; }
    static char text[CABLE_JSON_MAX];   // only ever touched on the reader task
    memcpy(text, payload, payload_len);
    text[payload_len] = '\0';

    cJSON *root = cJSON_Parse(text);
    if (!root) { s_bad++; return; }
    handle_message(root);
    cJSON_Delete(root);
}

// ── session machine ─────────────────────────────────────────────────────────

void cable_client_poll(void)
{
    const uint32_t now = cable_platform_millis();

    if (s_session && (uint32_t)(now - s_last_rx_ms) > CABLE_SILENCE_MS) {
        session_down("silence");
    }
    // The cable leaving the machine is the other way a session ends — the
    // driver sees the host go even when no frame ever said goodbye.
    if (s_session && !cable_link_host_present()) {
        session_down("host gone");
    }

    if ((int32_t)(now - s_next_hello_ms) >= 0) {
        send_hello();
        s_next_hello_ms = now + (s_session ? CABLE_HELLO_SESSION_MS : CABLE_HELLO_ALONE_MS);
    }
}

// ── lifecycle ───────────────────────────────────────────────────────────────

void cable_client_init(void)
{
    CLIENT_LOCK();
    s_agent_count = 0;
    s_agents_building = false;
    s_agents_total = 0;
    s_has_window = false;
    s_session = false;
    s_question_pending = false;
    s_bad = 0;
    s_unknown = 0;
    s_machine_name[0] = '\0';
    s_machine_id[0] = '\0';
    s_selected[0] = '\0';
    CLIENT_UNLOCK();
    s_last_rx_ms = cable_platform_millis();
    s_next_hello_ms = s_last_rx_ms;   // greet immediately at boot
}

void cable_client_set_identity(const char *fw, const char *hw, const char *mac)
{
    if (fw && fw[0]) copy_str(s_fw, sizeof(s_fw), fw);
    if (hw && hw[0]) copy_str(s_hw, sizeof(s_hw), hw);
    if (mac && mac[0]) copy_str(s_mac, sizeof(s_mac), mac);
}

void cable_client_set_ui(const cable_client_ui_t *ui)
{
    if (ui) s_ui = *ui;
    else memset(&s_ui, 0, sizeof(s_ui));
}

bool cable_client_is_connected(void) { return s_session; }
const char *cable_client_machine_name(void) { return s_machine_name; }
const char *cable_client_machine_id(void) { return s_machine_id; }

const cable_question_t *cable_client_pending_question(void)
{
    return s_question_pending ? &s_question : NULL;
}

void cable_client_counters(uint32_t *bad, uint32_t *unknown)
{
    if (bad) *bad = s_bad;
    if (unknown) *unknown = s_unknown;
}

#ifndef CABLE_HOST_TEST
// The 250 ms session loop (PROTOCOL.md §3.1 "device session loop").
static void session_task(void *arg)
{
    (void)arg;
    for (;;) {
        cable_client_poll();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

bool cable_client_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "no memory for the store lock — link disabled");
        return false;
    }
    cable_client_init();
    if (xTaskCreate(session_task, "cable_session", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "session task create failed — no handshake");
        return false;
    }
    ESP_LOGI(TAG, "cable client started (fw %s, proto %d)", s_fw, CABLE_PROTO_VERSION);
    return true;
}
#endif
