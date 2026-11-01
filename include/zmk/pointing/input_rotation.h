/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>

/* One instance per virtual source. Fractional movement never crosses sources. */
struct zmk_runtime_rotation_state {
    uint32_t trig;
    uint32_t settings_generation;
    int32_t x_remainder;
    int32_t y_remainder;
    bool initialized;
};

uint32_t zmk_input_processor_runtime_settings_generation(const struct device *processor);

/* Transform one complete relative frame. Output saturates at the signed
 * 16-bit pointing boundary; changes in effective rotation retire fractional state. */
int zmk_input_processor_runtime_rotate_xy(const struct device *processor, int32_t x, int32_t y,
                                          struct zmk_runtime_rotation_state *state,
                                          int16_t *rotated_x, int16_t *rotated_y);

bool zmk_input_processor_runtime_rotation_codes(const struct device *processor, uint16_t *x_code,
                                                uint16_t *y_code);

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PIPELINE)
bool zmk_runtime_input_pipeline_has_processor(const struct device *processor);
bool zmk_runtime_input_pipeline_matches(const struct device *processor,
                                        const struct device *source);
#else
static inline bool zmk_runtime_input_pipeline_has_processor(const struct device *processor) {
    ARG_UNUSED(processor);
    return false;
}
static inline bool zmk_runtime_input_pipeline_matches(const struct device *processor,
                                                      const struct device *source) {
    ARG_UNUSED(processor);
    ARG_UNUSED(source);
    return false;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_ROTATION)
bool zmk_runtime_input_rotation_has_processor(const struct device *processor);
bool zmk_runtime_input_rotation_matches(const struct device *processor,
                                        const struct device *source);
#else
static inline bool zmk_runtime_input_rotation_has_processor(const struct device *processor) {
    return zmk_runtime_input_pipeline_has_processor(processor);
}

static inline bool zmk_runtime_input_rotation_matches(const struct device *processor,
                                                      const struct device *source) {
    return zmk_runtime_input_pipeline_matches(processor, source);
}
#endif
