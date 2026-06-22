#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void buzzer_init(void);
void buzzer_beep(uint32_t duration_ms);
bool buzzer_is_enabled(void);
uint32_t buzzer_get_default_ms(void);

#ifdef __cplusplus
}
#endif
