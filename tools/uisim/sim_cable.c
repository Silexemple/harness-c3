// Simulated cable_client backend for the host UI simulator.
//
// The real cable_client.c is NOT linked here: ui.c only consumes a dozen of
// its entry points, which this file reimplements over in-memory scenario
// data. The sim scenario (sim_main.c) fills the store with
// sim_cable_set_*() and then fires the UI callbacks exactly as the link
// reader task would on device.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cable_client.h"

static cable_client_ui_t s_ui;

// Scenario store -----------------------------------------------------------

static cable_agent_t    s_agents[CABLE_MAX_AGENTS];
static int              s_agent_count;
static int              s_fleet_total;
static bool             s_has_window = true;
static cable_question_t s_question;
static bool             s_question_pending;

void sim_cable_set_agents(const cable_agent_t *agents, int count, int fleet_total)
{
    if (count > CABLE_MAX_AGENTS) count = CABLE_MAX_AGENTS;
    memcpy(s_agents, agents, (size_t)count * sizeof(*agents));
    s_agent_count = count;
    s_fleet_total = fleet_total;
}

void sim_cable_set_question(const cable_question_t *q, bool pending)
{
    if (q) s_question = *q;
    s_question_pending = pending;
}

// Fire the UI callbacks ui.c registered via cable_client_set_ui(). -----------

void sim_cable_session(bool up, const char *machine)
{
    if (s_ui.session) s_ui.session(up, machine, s_ui.ctx);
}

void sim_cable_agents_changed(void)
{
    if (s_ui.agents_changed) s_ui.agents_changed(s_ui.ctx);
}

void sim_cable_question(void)
{
    if (s_ui.question) s_ui.question(&s_question, s_ui.ctx);
}

void sim_cable_question_closed(void)
{
    if (s_ui.question_closed) s_ui.question_closed(s_ui.ctx);
}

void sim_cable_toast(const char *text)
{
    if (s_ui.toast) s_ui.toast(text, s_ui.ctx);
}

// cable_client entry points consumed by ui.c --------------------------------

void cable_client_set_ui(const cable_client_ui_t *ui)
{
    s_ui = ui ? *ui : (cable_client_ui_t){0};
}

int cable_client_list_agents(cable_agent_t *out, int max)
{
    int n = s_agent_count < max ? s_agent_count : max;
    memcpy(out, s_agents, (size_t)n * sizeof(*out));
    return n;
}

int cable_client_agent_total(void) { return s_fleet_total; }

bool cable_client_has_window(void) { return s_has_window; }

const cable_question_t *cable_client_pending_question(void)
{
    return s_question_pending ? &s_question : NULL;
}

void cable_client_answer(const char *agent_id, const char *request_id,
                         const cJSON *answers)
{
    char *txt = answers ? cJSON_PrintUnformatted(answers) : NULL;
    fprintf(stderr, "[cable] answer agent=%s req=%s %s\n",
            agent_id, request_id, txt ? txt : "{}");
    free(txt);
    s_question_pending = false;
}

void cable_client_send_focus(const char *agent_id)
{
    fprintf(stderr, "[cable] focus %s\n", agent_id ? agent_id : "");
}

void cable_client_send_open(const char *agent_id, const char *reason)
{
    fprintf(stderr, "[cable] open %s (%s)\n", agent_id ? agent_id : "",
            reason ? reason : "tap");
}
