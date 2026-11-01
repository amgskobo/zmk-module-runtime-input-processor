/* SPDX-License-Identifier: MIT */
#pragma once

#include <zmk/pointing/input_processor_runtime.h>

enum runtime_override_field {
    RUNTIME_OVERRIDE_SCALE = BIT(0),
    RUNTIME_OVERRIDE_ROTATION = BIT(1),
    RUNTIME_OVERRIDE_SNAP = BIT(2),
};

struct runtime_override_values {
    uint32_t multiplier;
    uint32_t divisor;
    int32_t rotation;
    uint32_t snap_mode;
    uint32_t snap_threshold;
    uint32_t snap_timeout_ms;
};

/* The behavior/position pair owns only its requested fields. Latest press wins
 * per field; release restores the next held owner or the persistent baseline. */
int runtime_override_press(const struct device *processor, const struct device *owner,
                           uint32_t position, uint8_t fields,
                           const struct runtime_override_values *values);
int runtime_override_release(const struct device *owner, uint32_t position);
