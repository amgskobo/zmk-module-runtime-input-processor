/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_input_processor_axis_snap

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing/input_processor_runtime.h>
#include <zmk/behavior.h>
#include "../pointing/runtime_input_override.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_input_processor_axis_snap_config {
    const char *processor_name;
    uint32_t timeout_ms;
};

struct behavior_input_processor_axis_snap_data {
    const struct device *processor;
};

static int behavior_input_processor_axis_snap_init(const struct device *dev) {
    struct behavior_input_processor_axis_snap_data *data = dev->data;
    const struct behavior_input_processor_axis_snap_config *cfg = dev->config;

    // Find the processor by name
    data->processor = zmk_input_processor_runtime_find_by_name(cfg->processor_name);
    if (!data->processor) {
        LOG_ERR("Input processor '%s' not found", cfg->processor_name);
        return -ENODEV;
    }

    LOG_DBG("Axis snap behavior initialized for processor: %s", cfg->processor_name);
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_input_processor_axis_snap_data *data = dev->data;
    const struct behavior_input_processor_axis_snap_config *cfg = dev->config;

    if (!data->processor) {
        return -ENODEV;
    }

    struct runtime_override_values values = {.snap_mode = binding->param1,
                                             .snap_threshold = binding->param2,
                                             .snap_timeout_ms = cfg->timeout_ms};
    int ret = runtime_override_press(data->processor, dev, event.position, RUNTIME_OVERRIDE_SNAP,
                                     &values);
    if (ret < 0) {
        return ret;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    runtime_override_release(dev, event.position);

    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_metadata metadata = {
    .sets_len = 0,
    .sets = NULL,
};
#endif

static const struct behavior_driver_api behavior_input_processor_axis_snap_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define AXIS_SNAP_INST(n)                                                                          \
    static struct behavior_input_processor_axis_snap_data                                          \
        behavior_input_processor_axis_snap_data_##n;                                               \
    static const struct behavior_input_processor_axis_snap_config                                  \
        behavior_input_processor_axis_snap_config_##n = {                                          \
            .processor_name = DT_INST_PROP(n, processor_name),                                     \
            .timeout_ms = DT_INST_PROP_OR(n, timeout_ms, 1000),                                    \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_input_processor_axis_snap_init, NULL,                      \
                            &behavior_input_processor_axis_snap_data_##n,                          \
                            &behavior_input_processor_axis_snap_config_##n, POST_KERNEL,           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_input_processor_axis_snap_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AXIS_SNAP_INST)
