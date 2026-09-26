// Optional passive buzzer on a spare PWM GPIO (LEDC). Entirely a no-op when
// CONFIG_HARNESS_PIN_BUZZER == -1 (SPEC.md §2) — callers never check.
#pragma once

#include <stdbool.h>

// Beep patterns the UI asks for.
typedef enum {
    BUZZER_BEEP_DONE,      // one short tone: a turn finished (summary)
    BUZZER_BEEP_QUESTION,  // two tones: an agent is waiting for an answer
    BUZZER_BEEP_ERROR,     // one low tone: turn.error / link fault
} buzzer_pattern_t;

// Returns false only when a configured buzzer failed to initialise; with
// CONFIG_HARNESS_PIN_BUZZER == -1 this succeeds as a no-op.
bool buzzer_init(void);

// Play a pattern. Non-blocking; a new pattern replaces the current one.
void buzzer_beep(buzzer_pattern_t pattern);
