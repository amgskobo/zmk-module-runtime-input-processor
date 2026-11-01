/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zmk/pointing/input_rotation.h>
#include "runtime_input_override.h"

struct runtime_override {
    const struct device *processor;
    const struct device *owner;
    uint32_t position;
    uint8_t fields;
    struct runtime_override_values values;
    struct runtime_override *next;
};

static struct runtime_override slots[CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_TEMP_OVERRIDES];
static struct runtime_override *head;
static K_MUTEX_DEFINE(override_lock);

static struct runtime_override **find_owner(const struct device *owner, uint32_t position) {
    struct runtime_override **link = &head;
    while (*link && ((*link)->owner != owner || (*link)->position != position)) {
        link = &(*link)->next;
    }
    return link;
}

static void apply_fields(const struct device *processor, uint8_t fields) {
    struct zmk_input_processor_runtime_config baseline;
    zmk_input_processor_runtime_get_config(processor, NULL, &baseline);
    struct runtime_override_values value = {
        .multiplier = baseline.scale_multiplier,
        .divisor = baseline.scale_divisor,
        .rotation = baseline.rotation_degrees,
        .snap_mode = baseline.axis_snap_mode,
        .snap_threshold = baseline.axis_snap_threshold,
        .snap_timeout_ms = baseline.axis_snap_timeout_ms,
    };
    uint8_t remaining = fields;
    for (struct runtime_override *item = head; item && remaining; item = item->next) {
        if (item->processor != processor) {
            continue;
        }
        uint8_t select = remaining & item->fields;
        if (select & RUNTIME_OVERRIDE_SCALE) {
            value.multiplier = item->values.multiplier;
            value.divisor = item->values.divisor;
        }
        if (select & RUNTIME_OVERRIDE_ROTATION) {
            value.rotation = item->values.rotation;
        }
        if (select & RUNTIME_OVERRIDE_SNAP) {
            value.snap_mode = item->values.snap_mode;
            value.snap_threshold = item->values.snap_threshold;
            value.snap_timeout_ms = item->values.snap_timeout_ms;
        }
        remaining &= ~select;
    }
    if (fields & RUNTIME_OVERRIDE_SCALE) {
        zmk_input_processor_runtime_set_scaling(processor, value.multiplier, value.divisor,
                                                ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    }
    if (fields & RUNTIME_OVERRIDE_ROTATION) {
        zmk_input_processor_runtime_set_rotation(processor, value.rotation,
                                                 ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    }
    if (fields & RUNTIME_OVERRIDE_SNAP) {
        zmk_input_processor_runtime_set_axis_snap(processor, value.snap_mode, value.snap_threshold,
                                                  value.snap_timeout_ms,
                                                  ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    }
}

int runtime_override_press(const struct device *processor, const struct device *owner,
                           uint32_t position, uint8_t fields,
                           const struct runtime_override_values *values) {
    /* Validate every requested field before touching either the pool or the
     * processor, so an unsupported rotation cannot leave scaling applied. */
    if (!processor || !owner || !values ||
        (fields & ~(RUNTIME_OVERRIDE_SCALE | RUNTIME_OVERRIDE_ROTATION | RUNTIME_OVERRIDE_SNAP)) ||
        ((fields & RUNTIME_OVERRIDE_SCALE) &&
         (!values->multiplier || !values->divisor ||
          values->divisor > ZMK_INPUT_PROCESSOR_RUNTIME_MAX_SCALE_DIVISOR)) ||
        ((fields & RUNTIME_OVERRIDE_ROTATION) &&
         (values->rotation < -360 || values->rotation > 360)) ||
        ((fields & RUNTIME_OVERRIDE_SNAP) &&
         (values->snap_mode > ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_Y ||
          values->snap_threshold > UINT16_MAX || values->snap_timeout_ms > UINT16_MAX))) {
        return -EINVAL;
    }
    if ((fields & RUNTIME_OVERRIDE_ROTATION) && values->rotation != 0 &&
        !zmk_runtime_input_rotation_has_processor(processor)) {
        return -ENOTSUP;
    }
    k_mutex_lock(&override_lock, K_FOREVER);
    struct runtime_override **link = find_owner(owner, position);
    struct runtime_override *item = *link;
    uint8_t changed = fields;
    if (item) {
        if (item->processor != processor) {
            k_mutex_unlock(&override_lock);
            return -EINVAL;
        }
        changed |= item->fields;
        *link = item->next;
    } else {
        for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
            if (!slots[i].owner) {
                item = &slots[i];
                break;
            }
        }
        if (!item) {
            k_mutex_unlock(&override_lock);
            return -ENOSPC;
        }
    }
    *item = (struct runtime_override){.processor = processor,
                                      .owner = owner,
                                      .position = position,
                                      .fields = fields,
                                      .values = *values,
                                      .next = head};
    head = item;
    apply_fields(processor, changed);
    k_mutex_unlock(&override_lock);
    return 0;
}

int runtime_override_release(const struct device *owner, uint32_t position) {
    k_mutex_lock(&override_lock, K_FOREVER);
    struct runtime_override **link = find_owner(owner, position);
    struct runtime_override *item = *link;
    if (item) {
        const struct device *processor = item->processor;
        uint8_t fields = item->fields;
        *link = item->next;
        item->owner = NULL;
        apply_fields(processor, fields);
    }
    k_mutex_unlock(&override_lock);
    return 0;
}
