/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 * Real input queues, listeners and downstream scalers; recording HID sink.
 */
#include <assert.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
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
static struct zmk_hid_mouse_report_body reports[128];
static unsigned int report_count, report_index;
static int admissions[128];
static unsigned int admission_count, admission_index;

int __wrap_zmk_endpoint_send_mouse_report(void) {
    assert(report_count < ARRAY_SIZE(reports));
    reports[report_count++] = zmk_hid_get_mouse_report()->body;
    k_sem_give(&report_ready);
    return 0;
}

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
    exit(0);
}
K_THREAD_DEFINE(pipeline_test_runner, 4096, pipeline_test_run, NULL, NULL, NULL, 5, 0, 0);
