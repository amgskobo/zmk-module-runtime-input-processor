/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 * Real input queues, listeners and downstream scalers; recording HID sink.
 */
#include <assert.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <zmk/hid.h>
#include <zmk/behavior.h>
#include <drivers/input_processor.h>
#include <zmk/pointing/input_processor_runtime.h>

static const struct device *const physical = DEVICE_DT_GET(DT_NODELABEL(mmv));
static const struct device *const processor = DEVICE_DT_GET(DT_NODELABEL(runtime_input_processor));
static const struct device *const virtual_source = DEVICE_DT_GET(DT_NODELABEL(runtime_pipeline));
static const struct device *const second_processor = DEVICE_DT_GET(DT_NODELABEL(second_processor));

static K_SEM_DEFINE(report_ready, 0, 128);
static K_SEM_DEFINE(admission_ready, 0, 128);
static K_SEM_DEFINE(pause_entered, 0, 1);
static K_SEM_DEFINE(pause_release, 0, 1);
static atomic_t pause_output;
static K_SEM_DEFINE(settings_pause_entered, 0, 1);
static K_SEM_DEFINE(settings_pause_release, 0, 1);
static K_SEM_DEFINE(settings_setter_start, 0, 1);
static K_SEM_DEFINE(settings_setter_entered, 0, 1);
static K_SEM_DEFINE(settings_setter_done, 0, 1);
static atomic_t pause_settings_frame;
static int settings_setter_result;
static k_tid_t report_thread;
static K_SEM_DEFINE(input_pause_entered, 0, 1);
static K_SEM_DEFINE(input_pause_release, 0, 1);
static K_SEM_DEFINE(input_barrier, 0, 1);
DEVICE_DEFINE(rip_test_pause_input, "rip-test-pause", NULL, NULL, NULL, NULL, POST_KERNEL,
              CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

static void input_pause_callback(struct input_event *event, void *context) {
    ARG_UNUSED(context);
    if (event->value == 1) {
        k_sem_give(&input_pause_entered);
        assert(k_sem_take(&input_pause_release, K_SECONDS(3)) == 0);
    } else {
        k_sem_give(&input_barrier);
    }
}
INPUT_CALLBACK_DEFINE(DEVICE_GET(rip_test_pause_input), input_pause_callback, NULL);
static struct zmk_hid_mouse_report_body reports[128];
static unsigned int report_count, report_index;
static int admissions[128];
static unsigned int admission_count, admission_index;

int __wrap_zmk_endpoint_send_mouse_report(void) {
    if (report_thread == NULL) {
        report_thread = k_current_get();
    }
    assert(report_thread == k_current_get());
    assert(report_count < ARRAY_SIZE(reports));
    reports[report_count++] = zmk_hid_get_mouse_report()->body;
    k_sem_give(&report_ready);
    return 0;
}

void zmk_input_pipeline_test_after_event(const struct device *pipeline,
                                         const struct device *stage_processor, size_t index) {
    if (pipeline == virtual_source && stage_processor == processor && index == 0 &&
        atomic_cas(&pause_settings_frame, 1, 0)) {
        k_sem_give(&settings_pause_entered);
        assert(k_sem_take(&settings_pause_release, K_SECONDS(3)) == 0);
    }
}

static void settings_setter(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    for (;;) {
        k_sem_take(&settings_setter_start, K_FOREVER);
        k_sem_give(&settings_setter_entered);
        settings_setter_result = zmk_input_processor_runtime_set_scaling(
            processor, 1, 2, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
        k_sem_give(&settings_setter_done);
    }
}
K_THREAD_DEFINE(pipeline_settings_setter, 3072, settings_setter, NULL, NULL, NULL, 6, 0, 0);

void zmk_input_pipeline_test_before_emit(const struct device *dev) {
    if (dev == virtual_source && atomic_cas(&pause_output, 1, 0)) {
        k_sem_give(&pause_entered);
        assert(k_sem_take(&pause_release, K_SECONDS(3)) == 0);
    }
}

void zmk_input_pipeline_test_frame_admitted(const struct device *dev, int status) {
    if (dev == virtual_source) {
        assert(admission_count < ARRAY_SIZE(admissions));
        admissions[admission_count++] = status;
        k_sem_give(&admission_ready);
    }
}

static int admitted(void) {
    assert(k_sem_take(&admission_ready, K_SECONDS(2)) == 0);
    return admissions[admission_index++];
}

static struct zmk_hid_mouse_report_body next_report(void) {
    assert(k_sem_take(&report_ready, K_SECONDS(2)) == 0);
    return reports[report_index++];
}

static void angle(const struct device *dev, int value) {
    assert(zmk_input_processor_runtime_set_rotation(
               dev, value, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY) == 0);
}

static void send_event(const struct device *dev, uint8_t type, uint16_t code, int32_t value,
                       bool sync) {
    assert(input_report(dev, type, code, value, sync, K_FOREVER) == 0);
}

static void xy(int x, int y) {
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, x, false);
    send_event(physical, INPUT_EV_REL, INPUT_REL_Y, y, true);
    assert(admitted() == 0);
}

static void expect_xy(int x, int y) {
    struct zmk_hid_mouse_report_body report = next_report();
    /* The real downstream scaler doubles both axes exactly once. */
    assert(report.d_x == x * 2 && report.d_y == y * 2);
}

static int held(const char *name, uint32_t position, bool pressed, uint32_t mode) {
    struct zmk_behavior_binding binding = {.behavior_dev = name, .param1 = mode, .param2 = 1000};
    struct zmk_behavior_binding_event event = {.position = position, .timestamp = k_uptime_get()};
    return zmk_behavior_invoke_binding(&binding, event, pressed);
}

static void test_held_overrides(void) {
    const char *dpi = DEVICE_DT_NAME(DT_NODELABEL(held_dpi));
    const char *newer = DEVICE_DT_NAME(DT_NODELABEL(newer_dpi));
    const char *snap = DEVICE_DT_NAME(DT_NODELABEL(held_snap));
    const char *unsupported = DEVICE_DT_NAME(DT_NODELABEL(unsupported_rotation));
    const struct device *inline_dev = DEVICE_DT_GET(DT_NODELABEL(inline_processor));
    assert(zmk_input_processor_runtime_set_rotation(
               processor, 0, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    assert(zmk_input_processor_runtime_set_scaling(
               processor, 1, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    angle(second_processor, 0);
    assert(held(dpi, 0, true, 0) == 0);
    assert(held(snap, 1, true, ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_Y) == 0);
    xy(2, 2);
    expect_xy(0, 6);
    assert(held(snap, 1, false, 0) == 0);
    xy(2, 2);
    expect_xy(-6, 6);
    assert(held(dpi, 0, false, 0) == 0);
    xy(2, 2);
    expect_xy(-2, 2);
    /* Release DPI first: the independent snap owner remains held. */
    assert(held(dpi, 0, true, 0) == 0);
    assert(held(snap, 1, true, ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_Y) == 0);
    assert(held(dpi, 0, false, 0) == 0);
    xy(2, 2);
    expect_xy(0, 2);
    assert(held(snap, 1, false, 0) == 0);
    /* Same field, different behavior and position owners, both release orders. */
    for (int reverse = 0; reverse < 2; reverse++) {
        assert(held(dpi, 0, true, 0) == 0);
        assert(held(newer, 1, true, 0) == 0);
        assert(held(reverse ? newer : dpi, reverse ? 1 : 0, false, 0) == 0);
        xy(1, 1);
        expect_xy(reverse ? -3 : -5, reverse ? 3 : 5);
        assert(held(reverse ? dpi : newer, reverse ? 0 : 1, false, 0) == 0);
        xy(1, 1);
        expect_xy(-1, 1);
    }
    assert(held(dpi, 20, true, 0) == 0);
    assert(held(dpi, 21, true, 0) == 0);
    assert(held(dpi, 20, false, 0) == 0);
    xy(1, 1);
    expect_xy(-3, 3);
    assert(held(dpi, 21, false, 0) == 0);
    assert(zmk_input_processor_runtime_set_rotation(
               processor, 15, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    assert(held(dpi, 0, true, 0) == 0);
    xy(1000, 0);
    expect_xy(-2898, 777);
    assert(held(dpi, 0, false, 0) == 0);
    assert(zmk_input_processor_runtime_set_rotation(
               processor, 0, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    assert(held(unsupported, 0, true, 0) == -ENOTSUP);
    struct input_event event = {.type = INPUT_EV_REL, .code = INPUT_REL_X, .value = 2};
    assert(zmk_input_processor_handle_event(inline_dev, &event, 0, 0, NULL) == 0);
    assert(event.value == 2);
    assert(held(unsupported, 0, false, 0) == 0);
    assert(held(snap, 1, true, 256) == -EINVAL);
    for (unsigned i = 0; i < CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_TEMP_OVERRIDES; i++) {
        assert(held(dpi, 100 + i, true, 0) == 0);
    }
    assert(held(newer, 300, true, 0) == -ENOSPC);
    xy(1, 1);
    expect_xy(-3, 3);
    for (unsigned i = 0; i < CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_TEMP_OVERRIDES; i++) {
        assert(held(dpi, 100 + i, false, 0) == 0);
    }
    assert(zmk_input_processor_runtime_set_scaling(
               processor, 1, 1000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    xy(999, 0);
    expect_xy(0, 0);
    assert(zmk_input_processor_runtime_set_scaling(
               processor, 1, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    xy(1, 0);
    expect_xy(-1, 0);
    printk("PASS: held_temporary_overrides\n");
}

static void test_complete_frame_settings(void) {
    angle(processor, 0);
    angle(second_processor, 0);
    assert(zmk_input_processor_runtime_set_scaling(
               processor, 1, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    atomic_set(&pause_settings_frame, 1);
    xy(10, 10);
    assert(k_sem_take(&settings_pause_entered, K_SECONDS(2)) == 0);
    k_sem_give(&settings_setter_start);
    assert(k_sem_take(&settings_setter_entered, K_SECONDS(2)) == 0);
    assert(k_sem_take(&settings_setter_done, K_MSEC(20)) == -EAGAIN);
    k_sem_give(&settings_pause_release);
    assert(k_sem_take(&settings_setter_done, K_SECONDS(2)) == 0);
    assert(settings_setter_result == 0);
    expect_xy(-10, 10);
    xy(10, 10);
    expect_xy(-5, 5);
    assert(zmk_input_processor_runtime_set_scaling(
               processor, 1, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    printk("PASS: rip_complete_frame_settings\n");
}

static void test_inertia_input_thread(void) {
    const uint16_t codes[] = {INPUT_REL_X, INPUT_REL_Y, INPUT_REL_HWHEEL, INPUT_REL_WHEEL};
    for (size_t i = 0; i < ARRAY_SIZE(codes); i++) {
        assert(zmk_input_processor_runtime_test_queue_inertia(processor, codes[i], 7) == 0);
        struct zmk_hid_mouse_report_body report = next_report();
        assert(report.d_x == (codes[i] == INPUT_REL_X ? 7 : 0));
        assert(report.d_y == (codes[i] == INPUT_REL_Y ? 7 : 0));
        assert(report.d_scroll_x == (codes[i] == INPUT_REL_HWHEEL ? 7 : 0));
        assert(report.d_scroll_y == (codes[i] == INPUT_REL_WHEEL ? 7 : 0));
        assert(report.buttons == 0);
    }
    /* The producer runs on this test thread; the recording sink requires the
     * same thread as physical reports. No downstream scaler may touch inertia. */
    xy(3, 4);
    expect_xy(-3, 4);
    assert(zmk_input_processor_runtime_test_queue_inertia(second_processor, INPUT_REL_Y, -9) == 0);
    struct zmk_hid_mouse_report_body second = next_report();
    assert(second.d_x == 0 && second.d_y == -9 && second.d_scroll_x == 0 && second.d_scroll_y == 0);
    assert(k_sem_take(&report_ready, K_NO_WAIT) != 0);

    /* Retire a queued sample before the real input thread can consume it. */
    const struct device *pause_source = DEVICE_GET(rip_test_pause_input);
    assert(input_report(pause_source, INPUT_EV_MSC, INPUT_MSC_SCAN, 1, true, K_FOREVER) == 0);
    assert(k_sem_take(&input_pause_entered, K_SECONDS(2)) == 0);
    assert(zmk_input_processor_runtime_test_queue_inertia(processor, INPUT_REL_X, 11) == 0);
    assert(zmk_input_processor_runtime_set_scaling(
               processor, 2, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    k_sem_give(&input_pause_release);
    assert(input_report(pause_source, INPUT_EV_MSC, INPUT_MSC_SCAN, 2, true, K_FOREVER) == 0);
    assert(k_sem_take(&input_barrier, K_SECONDS(2)) == 0);
    assert(k_sem_take(&report_ready, K_NO_WAIT) != 0);
    assert(zmk_input_processor_runtime_set_scaling(
               processor, 1, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);

    /* A full queue drops bounded output without blocking the producer or
     * converting an accepted header into an extra movement later. */
    assert(input_report(pause_source, INPUT_EV_MSC, INPUT_MSC_SCAN, 1, true, K_FOREVER) == 0);
    assert(k_sem_take(&input_pause_entered, K_SECONDS(2)) == 0);
    unsigned accepted = 0;
    int ret = 0;
    for (unsigned i = 0; i <= CONFIG_INPUT_QUEUE_MAX_MSGS; i++) {
        ret = zmk_input_processor_runtime_test_queue_inertia(processor, INPUT_REL_X, 7);
        if (ret < 0) {
            break;
        }
        ++accepted;
    }
    assert(ret < 0 && accepted > 0);
    k_sem_give(&input_pause_release);
    assert(input_report(pause_source, INPUT_EV_MSC, INPUT_MSC_SCAN, 2, true, K_FOREVER) == 0);
    assert(k_sem_take(&input_barrier, K_SECONDS(2)) == 0);
    for (unsigned i = 0; i < accepted; i++) {
        struct zmk_hid_mouse_report_body report = next_report();
        assert(report.d_x == 7 && report.d_y == 0 && report.d_scroll_x == 0 &&
               report.d_scroll_y == 0);
    }
    assert(k_sem_take(&report_ready, K_NO_WAIT) != 0);
    xy(1, 2);
    expect_xy(-1, 2);
    printk("PASS: rip_inertia_input_thread\n");
}

static void pipeline_test_run(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);
    assert(device_is_ready(virtual_source));
    test_held_overrides();
    angle(processor, 90);
    angle(second_processor, 90);
    xy(10, 20);
    expect_xy(-10, 20);
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 10, true);
    assert(admitted() == 0);
    expect_xy(-10, 0);
    send_event(physical, INPUT_EV_REL, INPUT_REL_Y, 10, true);
    assert(admitted() == 0);
    expect_xy(0, 10);
    send_event(physical, INPUT_EV_REL, INPUT_REL_Y, 8, false);
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 6, true);
    assert(admitted() == 0);
    expect_xy(-6, 8);
    printk("PASS: runtime_pipeline_stage_order\n");

    angle(second_processor, 0);
    angle(processor, 15);
    xy(1000, 0);
    expect_xy(-966, 259);
    angle(processor, 45);
    xy(1000, 0);
    expect_xy(-707, 707);
    angle(processor, -90);
    xy(7, 5);
    expect_xy(-5, -7);
    angle(processor, 360);
    xy(7, 5);
    expect_xy(-7, 5);
    printk("PASS: runtime_pipeline_arbitrary_angles\n");

    angle(processor, 45);
    int total_x = 0, total_y = 0;
    for (int i = 0; i < 10; i++) {
        send_event(physical, INPUT_EV_REL, INPUT_REL_X, 1, true);
        assert(admitted() == 0);
        struct zmk_hid_mouse_report_body report = next_report();
        total_x += report.d_x;
        total_y += report.d_y;
    }
    assert(total_x == -14 && total_y == 14);
    printk("PASS: runtime_pipeline_fractional_frames\n");
    angle(processor, 90);
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 3, false);
    send_event(physical, INPUT_EV_REL, INPUT_REL_WHEEL, 4, false);
    send_event(physical, INPUT_EV_KEY, INPUT_BTN_0, 1, true);
    assert(admitted() == 0);
    struct zmk_hid_mouse_report_body press = next_report();
    assert(press.d_x == 0 && press.d_y == 6 && press.d_scroll_y == 4 && press.buttons == 1);
    send_event(physical, INPUT_EV_KEY, INPUT_BTN_0, 0, true);
    assert(admitted() == 0);
    assert(next_report().buttons == 0);
    printk("PASS: runtime_pipeline_buttons_and_scroll\n");

    for (int i = 0; i < 5; i++) {
        send_event(physical, INPUT_EV_REL, INPUT_REL_X, 100, i == 4);
    }
    assert(admitted() < 0);
    xy(1, 2);
    expect_xy(2, 1);
    assert(k_sem_take(&report_ready, K_NO_WAIT) != 0);
    printk("PASS: runtime_pipeline_oversized_recovery\n");

    atomic_set(&pause_output, 1);
    xy(1, 1);
    assert(k_sem_take(&pause_entered, K_SECONDS(2)) == 0);
    xy(2, 2);
    xy(3, 3);
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 99, false);
    send_event(physical, INPUT_EV_REL, INPUT_REL_Y, 99, true);
    assert(admitted() < 0);
    k_sem_give(&pause_release);
    expect_xy(1, 1);
    expect_xy(2, 2);
    expect_xy(3, 3);
    xy(4, 5);
    expect_xy(5, 4);
    assert(k_sem_take(&report_ready, K_NO_WAIT) != 0);
    printk("PASS: runtime_pipeline_queue_recovery\n");
    test_complete_frame_settings();
    test_inertia_input_thread();
    exit(0);
}
K_THREAD_DEFINE(pipeline_test_runner, 4096, pipeline_test_run, NULL, NULL, NULL, 5, 0, 0);
