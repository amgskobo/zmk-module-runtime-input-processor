/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT zmk_runtime_input_pipeline

#include <errno.h>
#include <limits.h>
#include <drivers/input_processor.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing/input_rotation.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_TEST)
void zmk_input_pipeline_test_after_event(const struct device *pipeline,
                                         const struct device *processor, size_t index);
#endif

#define INPUTS CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_FRAME_EVENTS
#define OUTPUTS CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_OUTPUT_EVENTS

struct pipeline_frame {
    uint32_t generation;
    uint8_t count;
    struct input_event events[INPUTS];
};

struct pipeline_buffer {
    uint8_t count;
    struct input_event events[OUTPUTS];
};

struct pipeline_stage {
    struct zmk_input_processor_entry entry;
    bool runtime;
};

struct pipeline_stage_data {
    struct zmk_runtime_rotation_state rotation;
    uint32_t settings_generation;
    int16_t remainders[4];
    uint16_t x_code, y_code;
};

struct pipeline_config {
    const struct device *input;
    const struct pipeline_stage *stages;
    struct pipeline_stage_data *state;
    size_t stage_count;
    struct k_msgq *queue;
    uint8_t listener_index;
};

struct pipeline_data {
    struct k_spinlock lock;
    struct pipeline_frame pending;
    struct pipeline_buffer buffers[2];
    uint32_t generation;
    bool dropping;
};

static const uint8_t pipeline_api;

static bool contains_processor(const struct device *source, const struct device *processor) {
    const struct pipeline_config *cfg = source->config;
    for (size_t i = 0; i < cfg->stage_count; i++) {
        if (cfg->stages[i].runtime && cfg->stages[i].entry.dev == processor) {
            return true;
        }
    }
    return false;
}

#define PIPELINE_DEVICE(n) DEVICE_DT_GET(DT_DRV_INST(n)),
bool zmk_runtime_input_pipeline_has_processor(const struct device *processor) {
    const struct device *sources[] = {DT_INST_FOREACH_STATUS_OKAY(PIPELINE_DEVICE)};
    for (size_t i = 0; i < ARRAY_SIZE(sources); i++) {
        if (contains_processor(sources[i], processor)) {
            return true;
        }
    }
    return false;
}

bool zmk_runtime_input_pipeline_matches(const struct device *processor,
                                        const struct device *source) {
    return source && source->api == &pipeline_api && contains_processor(source, processor);
}

static void retire_fractions(const struct pipeline_config *cfg) {
    for (size_t i = 0; i < cfg->stage_count; i++) {
        cfg->state[i].rotation = (struct zmk_runtime_rotation_state){0};
        memset(cfg->state[i].remainders, 0, sizeof(cfg->state[i].remainders));
    }
}

static bool is_axis(const struct pipeline_stage_data *state, const struct input_event *event) {
    return event->type == INPUT_EV_REL &&
           (event->code == state->x_code || event->code == state->y_code);
}

static int append(struct pipeline_buffer *buffer, struct input_event event) {
    if (buffer->count == OUTPUTS) {
        return -EMSGSIZE;
    }
    event.sync = false;
    buffer->events[buffer->count++] = event;
    return 0;
}

static int rotate_stage(const struct pipeline_stage *stage, struct pipeline_stage_data *state,
                        const struct pipeline_buffer *input, struct pipeline_buffer *output) {
    uint32_t generation = zmk_input_processor_runtime_settings_generation(stage->entry.dev);
    if (!state->rotation.initialized || state->settings_generation != generation) {
        memset(state->remainders, 0, sizeof(state->remainders));
        state->rotation = (struct zmk_runtime_rotation_state){0};
        state->settings_generation = generation;
    }
    int64_t x = 0, y = 0;
    bool have_axes = false;
    for (size_t i = 0; i < input->count; i++) {
        const struct input_event *event = &input->events[i];
        if (!is_axis(state, event)) {
            continue;
        }
        have_axes = true;
        if (event->code == state->x_code) {
            x += event->value;
        } else {
            y += event->value;
        }
    }
    int16_t rotated_x = 0, rotated_y = 0;
    if (have_axes) {
        int ret = zmk_input_processor_runtime_rotate_xy(
            stage->entry.dev, (int32_t)CLAMP(x, INT32_MIN, INT32_MAX),
            (int32_t)CLAMP(y, INT32_MIN, INT32_MAX), &state->rotation, &rotated_x, &rotated_y);
        if (ret < 0) {
            return ret;
        }
    }
    bool inserted = false;
    output->count = 0;
    for (size_t i = 0; i < input->count; i++) {
        struct input_event event = input->events[i];
        if (is_axis(state, &event)) {
            if (inserted) {
                continue;
            }
            inserted = true;
            event.code = state->x_code;
            event.value = rotated_x;
            if (append(output, event) < 0) {
                return -EMSGSIZE;
            }
            event.code = state->y_code;
            event.value = rotated_y;
        }
        if (append(output, event) < 0) {
            return -EMSGSIZE;
        }
    }
    return 0;
}

static int16_t *remainder_for(struct pipeline_stage_data *state, const struct input_event *event) {
    if (event->type != INPUT_EV_REL) {
        return NULL;
    }
    switch (event->code) {
    case INPUT_REL_X:
        return &state->remainders[0];
    case INPUT_REL_Y:
        return &state->remainders[1];
    case INPUT_REL_HWHEEL:
        return &state->remainders[2];
    case INPUT_REL_WHEEL:
        return &state->remainders[3];
    default:
        return NULL;
    }
}

static int process_frame_locked(const struct device *dev, const struct pipeline_frame *frame,
                                struct pipeline_buffer **result) {
    const struct pipeline_config *cfg = dev->config;
    struct pipeline_data *data = dev->data;
    struct pipeline_buffer *input = &data->buffers[0], *output = &data->buffers[1];
    input->count = frame->count;
    memcpy(input->events, frame->events, frame->count * sizeof(struct input_event));
    for (size_t i = 0; i < input->count; i++) {
        input->events[i].dev = dev;
    }
    for (size_t i = 0; i < cfg->stage_count; i++) {
        const struct pipeline_stage *stage = &cfg->stages[i];
        struct pipeline_stage_data *state = &cfg->state[i];
        if (stage->runtime) {
            int ret = rotate_stage(stage, state, input, output);
            if (ret < 0) {
                return ret;
            }
            struct pipeline_buffer *swap = input;
            input = output;
            output = swap;
        }
        output->count = 0;
        for (size_t e = 0; e < input->count; e++) {
            struct input_event event = input->events[e];
            event.sync = e == input->count - 1;
            struct zmk_input_processor_state context = {
                .input_device_index = cfg->listener_index,
                .remainder = stage->entry.track_remainders ? remainder_for(state, &event) : NULL,
            };
            int ret = zmk_input_processor_handle_event(
                stage->entry.dev, &event, stage->entry.param1, stage->entry.param2, &context);
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_TEST)
            if (stage->runtime) {
                zmk_input_pipeline_test_after_event(dev, stage->entry.dev, e);
            }
#endif
            if (ret < 0) {
                return ret;
            }
            if (ret == ZMK_INPUT_PROC_CONTINUE && append(output, event) < 0) {
                return -EMSGSIZE;
            }
        }
        struct pipeline_buffer *swap = input;
        input = output;
        output = swap;
    }
    if (input->count) {
        input->events[input->count - 1].sync = true;
    }
    *result = input;
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_TEST)
void zmk_input_pipeline_test_frame_admitted(const struct device *dev, int status);
void zmk_input_pipeline_test_before_emit(const struct device *dev);
#endif

static int process_frame(const struct device *dev, const struct pipeline_frame *frame,
                         struct pipeline_buffer **result) {
    zmk_input_processor_runtime_frame_lock();
    int ret = process_frame_locked(dev, frame, result);
    zmk_input_processor_runtime_frame_unlock();
    return ret;
}

static void pipeline_input(struct input_event *event, void *context) {
    const struct device *dev = context;
    if (!device_is_ready(dev)) {
        return;
    }
    const struct pipeline_config *cfg = dev->config;
    struct pipeline_data *data = dev->data;
    int ret = 0;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (!data->dropping) {
        if (data->pending.count == INPUTS) {
            data->dropping = true;
        } else {
            data->pending.events[data->pending.count++] = *event;
        }
    }
    if (event->sync) {
        data->pending.generation = data->generation;
        ret = data->dropping ? -EMSGSIZE : k_msgq_put(cfg->queue, &data->pending, K_NO_WAIT);
        if (ret < 0) {
            data->generation++;
        }
        data->pending.count = 0;
        data->dropping = false;
    }
    k_spin_unlock(&data->lock, key);
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_TEST)
    if (event->sync) {
        zmk_input_pipeline_test_frame_admitted(dev, ret);
    }
#endif
    if (ret < 0) {
        LOG_WRN("Runtime pipeline dropped a complete input frame (%d)", ret);
    }
}

static void pipeline_thread(void *context, void *unused1, void *unused2) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    const struct device *dev = context;
    const struct pipeline_config *cfg = dev->config;
    struct pipeline_frame frame;
    uint32_t generation = 0;
    while (true) {
        k_msgq_get(cfg->queue, &frame, K_FOREVER);
        if (generation != frame.generation) {
            retire_fractions(cfg);
            generation = frame.generation;
        }
        struct pipeline_buffer *output;
        int ret = process_frame(dev, &frame, &output);
        if (ret < 0) {
            retire_fractions(cfg);
            LOG_WRN("Runtime pipeline dropped a complete processed frame (%d)", ret);
            continue;
        }
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_TEST)
        zmk_input_pipeline_test_before_emit(dev);
#endif
        for (size_t i = 0; i < output->count; i++) {
            const struct input_event *event = &output->events[i];
            ret = input_report(dev, event->type, event->code, event->value, event->sync, K_FOREVER);
            if (ret < 0) {
                LOG_ERR("Runtime pipeline output failed (%d)", ret);
            }
        }
    }
}

static int pipeline_init(const struct device *dev) {
    const struct pipeline_config *cfg = dev->config;
    if (!device_is_ready(cfg->input)) {
        return -ENODEV;
    }
    for (size_t i = 0; i < cfg->stage_count; i++) {
        if (!device_is_ready(cfg->stages[i].entry.dev)) {
            return -ENODEV;
        }
        if (cfg->stages[i].runtime &&
            !zmk_input_processor_runtime_rotation_codes(
                cfg->stages[i].entry.dev, &cfg->state[i].x_code, &cfg->state[i].y_code)) {
            return -ENOTSUP;
        }
    }
    return 0;
}

#define PIPELINE_STAGE(node, prop, idx)                                                            \
    {.entry = ZMK_INPUT_PROCESSOR_ENTRY_AT_IDX(idx, node),                                         \
     .runtime =                                                                                    \
         DT_NODE_HAS_COMPAT(DT_PHANDLE_BY_IDX(node, prop, idx), zmk_input_processor_runtime)},
#define LISTENER_MATCH(i, node)                                                                    \
    DT_SAME_NODE(DT_PHANDLE(DT_INST(i, zmk_input_listener), device), node)
#define LISTENER_INDEX(i, node) ((i) * LISTENER_MATCH(i, node))
#define PIPELINE_DEFINE(n)                                                                         \
    BUILD_ASSERT(OUTPUTS >= INPUTS, "pipeline output capacity must cover an input frame");         \
    BUILD_ASSERT(!DT_SAME_NODE(DT_DRV_INST(n), DT_INST_PHANDLE(n, input_device)),                  \
                 "pipeline cannot receive itself");                                                \
    BUILD_ASSERT((LISTIFY(DT_NUM_INST_STATUS_OKAY(zmk_input_listener), LISTENER_MATCH, (+),        \
                          DT_DRV_INST(n))) == 1,                                                   \
                 "pipeline needs exactly one final listener");                                     \
    static const struct pipeline_stage stages_##n[] = {                                            \
        DT_INST_FOREACH_PROP_ELEM(n, input_processors, PIPELINE_STAGE)};                           \
    static struct pipeline_stage_data state_##n[ARRAY_SIZE(stages_##n)];                           \
    K_MSGQ_DEFINE(pipeline_queue_##n, sizeof(struct pipeline_frame),                               \
                  CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_QUEUE_FRAMES, 4);                              \
    static const struct pipeline_config pipeline_config_##n = {                                    \
        .input = DEVICE_DT_GET(DT_INST_PHANDLE(n, input_device)),                                  \
        .stages = stages_##n,                                                                      \
        .state = state_##n,                                                                        \
        .stage_count = ARRAY_SIZE(stages_##n),                                                     \
        .queue = &pipeline_queue_##n,                                                              \
        .listener_index = LISTIFY(DT_NUM_INST_STATUS_OKAY(zmk_input_listener), LISTENER_INDEX,     \
                                  (+), DT_DRV_INST(n)),                                            \
    };                                                                                             \
    static struct pipeline_data pipeline_data_##n;                                                 \
    DEVICE_DT_INST_DEFINE(n, pipeline_init, NULL, &pipeline_data_##n, &pipeline_config_##n,        \
                          POST_KERNEL, CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_INIT_PRIORITY,            \
                          &pipeline_api);                                                          \
    INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_INST_PHANDLE(n, input_device)), pipeline_input,         \
                          (void *)DEVICE_DT_GET(DT_DRV_INST(n)));                                  \
    K_THREAD_DEFINE(pipeline_thread_##n, CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_STACK_SIZE,             \
                    pipeline_thread, (void *)DEVICE_DT_GET(DT_DRV_INST(n)), NULL, NULL,            \
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

DT_INST_FOREACH_STATUS_OKAY(PIPELINE_DEFINE)
