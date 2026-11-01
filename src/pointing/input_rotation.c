/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT zmk_runtime_input_rotation

#include <errno.h>
#include <limits.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing/input_rotation.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_ROTATION_TEST)
void zmk_input_rotation_test_frame_admitted(const struct device *dev, int status);
#endif

#define FRAME_INPUTS CONFIG_ZMK_RUNTIME_INPUT_ROTATION_FRAME_EVENTS

struct rotation_frame {
    uint8_t count;
    struct input_event events[FRAME_INPUTS + 1];
};

struct rotation_config {
    const struct device *input;
    const struct device *processor;
    uint16_t x_code;
    uint16_t y_code;
    struct k_msgq *queue;
};

struct rotation_data {
    struct k_spinlock lock;
    struct rotation_frame pending;
    struct rotation_frame output;
    struct zmk_runtime_rotation_state rotation;
    bool dropping;
};

static const uint8_t rotation_api;

#define ROTATION_OWNS_PROCESSOR(n)                                                                 \
    if (processor == DEVICE_DT_GET(DT_INST_PHANDLE(n, processor))) {                               \
        return true;                                                                               \
    }

bool zmk_runtime_input_rotation_has_processor(const struct device *processor) {
    DT_INST_FOREACH_STATUS_OKAY(ROTATION_OWNS_PROCESSOR)
    return zmk_runtime_input_pipeline_has_processor(processor);
}

bool zmk_runtime_input_rotation_matches(const struct device *processor,
                                        const struct device *source) {
    return (source && source->api == &rotation_api &&
            ((const struct rotation_config *)source->config)->processor == processor) ||
           zmk_runtime_input_pipeline_matches(processor, source);
}

static bool rotation_axis(const struct rotation_config *cfg, const struct input_event *event) {
    return event->type == INPUT_EV_REL &&
           (event->code == cfg->x_code || event->code == cfg->y_code);
}

/* Build one immutable output frame. Other events retain their order, and
 * exactly its last output carries sync, including a button/scroll terminator. */
static int rotation_finish(const struct device *dev) {
    const struct rotation_config *cfg = dev->config;
    struct rotation_data *data = dev->data;
    int64_t x = 0, y = 0;
    bool have_axes = false;
    for (size_t i = 0; i < data->pending.count; i++) {
        const struct input_event *event = &data->pending.events[i];
        if (!rotation_axis(cfg, event)) {
            continue;
        }
        have_axes = true;
        if (event->code == cfg->x_code) {
            x += event->value;
        } else {
            y += event->value;
        }
    }
    int16_t rotated_x = 0, rotated_y = 0;
    if (have_axes) {
        int ret = zmk_input_processor_runtime_rotate_xy(
            cfg->processor, (int32_t)CLAMP(x, INT32_MIN, INT32_MAX),
            (int32_t)CLAMP(y, INT32_MIN, INT32_MAX), &data->rotation, &rotated_x, &rotated_y);
        if (ret < 0) {
            return ret;
        }
    }
    data->output.count = 0;
    bool inserted = false;
    for (size_t i = 0; i < data->pending.count; i++) {
        struct input_event event = data->pending.events[i];
        event.dev = dev;
        event.sync = false;
        if (rotation_axis(cfg, &event)) {
            if (inserted) {
                continue;
            }
            inserted = true;
            event.code = cfg->x_code;
            event.value = rotated_x;
            data->output.events[data->output.count++] = event;
            event.code = cfg->y_code;
            event.value = rotated_y;
        }
        data->output.events[data->output.count++] = event;
    }
    data->output.events[data->output.count - 1].sync = true;
    return k_msgq_put(cfg->queue, &data->output, K_NO_WAIT);
}

static void rotation_input_callback(struct input_event *event, void *context) {
    const struct device *dev = context;
    if (!device_is_ready(dev)) {
        return;
    }
    struct rotation_data *data = dev->data;
    int ret = 0;
    const k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (!data->dropping) {
        if (data->pending.count == FRAME_INPUTS) {
            data->dropping = true;
            ret = -EMSGSIZE;
        } else {
            data->pending.events[data->pending.count++] = *event;
            if (event->sync) {
                ret = rotation_finish(dev);
            }
        }
    }
    if (ret < 0) {
        data->rotation = (struct zmk_runtime_rotation_state){0};
    }
    if (event->sync) {
        if (data->dropping) {
            ret = -EMSGSIZE;
        }
        data->pending.count = 0;
        data->dropping = false;
    }
    k_spin_unlock(&data->lock, key);
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_ROTATION_TEST)
    if (event->sync) {
        zmk_input_rotation_test_frame_admitted(dev, ret);
    }
#endif
    if (ret < 0) {
        LOG_WRN("Rotation source dropped a complete frame (%d)", ret);
    }
}

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_ROTATION_TEST)
void zmk_input_rotation_test_before_emit(const struct device *dev);
#endif

static void rotation_thread(void *context, void *unused1, void *unused2) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    const struct device *dev = context;
    const struct rotation_config *cfg = dev->config;
    struct rotation_frame frame;
    while (true) {
        k_msgq_get(cfg->queue, &frame, K_FOREVER);
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_ROTATION_TEST)
        zmk_input_rotation_test_before_emit(dev);
#endif
        for (size_t i = 0; i < frame.count; i++) {
            const struct input_event *event = &frame.events[i];
            /* This dedicated worker can wait for space. Reporting from the
             * input callback itself could deadlock the input thread or lose
             * half of a rotated pair when its queue is full. */
            int ret =
                input_report(dev, event->type, event->code, event->value, event->sync, K_FOREVER);
            if (ret < 0) {
                LOG_ERR("Rotation source output failed (%d)", ret);
            }
        }
    }
}

static int rotation_init(const struct device *dev) {
    const struct rotation_config *cfg = dev->config;
    return device_is_ready(cfg->input) && device_is_ready(cfg->processor) ? 0 : -ENODEV;
}

#define ROTATION_PROCESSOR(n) DT_INST_PHANDLE(n, processor)
#define ROTATION_DEFINE(n)                                                                         \
    BUILD_ASSERT(!DT_SAME_NODE(DT_DRV_INST(n), DT_INST_PHANDLE(n, device)),                        \
                 "rotation source cannot listen to itself");                                       \
    BUILD_ASSERT(DT_NODE_HAS_COMPAT(ROTATION_PROCESSOR(n), zmk_input_processor_runtime),           \
                 "rotation source needs a runtime input processor");                               \
    BUILD_ASSERT(DT_PROP(ROTATION_PROCESSOR(n), type) == INPUT_EV_REL,                             \
                 "rotation source requires relative input");                                       \
    BUILD_ASSERT(DT_PROP_LEN(ROTATION_PROCESSOR(n), x_codes) == 1 &&                               \
                     DT_PROP_LEN(ROTATION_PROCESSOR(n), y_codes) == 1,                             \
                 "rotation source requires exactly one X/Y code pair");                            \
    BUILD_ASSERT(DT_PROP_BY_IDX(ROTATION_PROCESSOR(n), x_codes, 0) !=                              \
                     DT_PROP_BY_IDX(ROTATION_PROCESSOR(n), y_codes, 0),                            \
                 "rotation X/Y codes must differ");                                                \
    K_MSGQ_DEFINE(rotation_queue_##n, sizeof(struct rotation_frame),                               \
                  CONFIG_ZMK_RUNTIME_INPUT_ROTATION_QUEUE_FRAMES, 4);                              \
    static const struct rotation_config rotation_config_##n = {                                    \
        .input = DEVICE_DT_GET(DT_INST_PHANDLE(n, device)),                                        \
        .processor = DEVICE_DT_GET(ROTATION_PROCESSOR(n)),                                         \
        .x_code = DT_PROP_BY_IDX(ROTATION_PROCESSOR(n), x_codes, 0),                               \
        .y_code = DT_PROP_BY_IDX(ROTATION_PROCESSOR(n), y_codes, 0),                               \
        .queue = &rotation_queue_##n,                                                              \
    };                                                                                             \
    static struct rotation_data rotation_data_##n;                                                 \
    DEVICE_DT_INST_DEFINE(n, rotation_init, NULL, &rotation_data_##n, &rotation_config_##n,        \
                          POST_KERNEL, CONFIG_ZMK_RUNTIME_INPUT_ROTATION_INIT_PRIORITY,            \
                          &rotation_api);                                                          \
    static void rotation_callback_##n(struct input_event *event, void *context) {                  \
        rotation_input_callback(event, context);                                                   \
    }                                                                                              \
    INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_INST_PHANDLE(n, device)), rotation_callback_##n,        \
                          (void *)DEVICE_DT_GET(DT_DRV_INST(n)));                                  \
    K_THREAD_DEFINE(rotation_thread_##n, CONFIG_ZMK_RUNTIME_INPUT_ROTATION_STACK_SIZE,             \
                    rotation_thread, (void *)DEVICE_DT_GET(DT_DRV_INST(n)), NULL, NULL,            \
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

DT_INST_FOREACH_STATUS_OKAY(ROTATION_DEFINE)
