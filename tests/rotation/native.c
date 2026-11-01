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
#include <zmk/pointing/input_processor_runtime.h>

static const struct device *const physical = DEVICE_DT_GET(DT_NODELABEL(mmv));
static const struct device *const processor = DEVICE_DT_GET(DT_NODELABEL(runtime_input_processor));
static const struct device *const virtual_source = DEVICE_DT_GET(DT_NODELABEL(runtime_rotation));
static const struct device *const second_physical = DEVICE_DT_GET(DT_NODELABEL(msc));
static const struct device *const second_processor = DEVICE_DT_GET(DT_NODELABEL(second_processor));

static K_SEM_DEFINE(report_ready, 0, 64);
static K_SEM_DEFINE(admission_ready, 0, 64);
static K_SEM_DEFINE(pause_entered, 0, 1);
static K_SEM_DEFINE(pause_release, 0, 1);
static atomic_t pause_output;
static struct zmk_hid_mouse_report_body reports[64];
static unsigned int report_count, report_index;
static int admissions[64];
static unsigned int admission_count, admission_index;

int __wrap_zmk_endpoint_send_mouse_report(void) {
    assert(report_count < ARRAY_SIZE(reports));
    reports[report_count++] = zmk_hid_get_mouse_report()->body;
    k_sem_give(&report_ready);
    return 0;
}

void zmk_input_rotation_test_before_emit(const struct device *dev) {
    if (dev == virtual_source && atomic_cas(&pause_output, 1, 0)) {
        k_sem_give(&pause_entered);
        assert(k_sem_take(&pause_release, K_SECONDS(3)) == 0);
    }
}

void zmk_input_rotation_test_frame_admitted(const struct device *dev, int status) {
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

static void rotation_test_run(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);
    assert(device_is_ready(virtual_source));
    angle(processor, 90);
    xy(10, 20);
    expect_xy(-20, 10);
    xy(3, 4);
    expect_xy(-4, 3);
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 10, true);
    assert(admitted() == 0);
    expect_xy(0, 10);
    send_event(physical, INPUT_EV_REL, INPUT_REL_Y, 10, true);
    assert(admitted() == 0);
    expect_xy(-10, 0);
    /* A reversed source order still belongs to one current frame. */
    send_event(physical, INPUT_EV_REL, INPUT_REL_Y, 8, false);
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 6, true);
    assert(admitted() == 0);
    expect_xy(-8, 6);
    printk("PASS: runtime_rotation_frames\n");

    angle(processor, 15);
    xy(1000, 0);
    expect_xy(966, 259);
    angle(processor, 45);
    xy(1000, 0);
    expect_xy(707, 707);
    angle(processor, -90);
    xy(7, 5);
    expect_xy(5, -7);
    angle(processor, 360);
    xy(7, 5);
    expect_xy(7, 5);
    printk("PASS: runtime_rotation_arbitrary_angles\n");

    angle(processor, 90);
    angle(second_processor, -90);
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 10, false);
    send_event(second_physical, INPUT_EV_REL, INPUT_REL_HWHEEL, 3, true);
    struct zmk_hid_mouse_report_body second = next_report();
    assert(second.d_scroll_x == 0 && second.d_scroll_y == -3);
    send_event(physical, INPUT_EV_REL, INPUT_REL_Y, 20, true);
    assert(admitted() == 0);
    expect_xy(-20, 10);
    printk("PASS: runtime_rotation_independent_sources\n");

    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 3, false);
    send_event(physical, INPUT_EV_REL, INPUT_REL_WHEEL, 4, false);
    send_event(physical, INPUT_EV_KEY, INPUT_BTN_0, 1, true);
    assert(admitted() == 0);
    struct zmk_hid_mouse_report_body press = next_report();
    assert(press.d_x == 0 && press.d_y == 6 && press.d_scroll_y == 4 && press.buttons == 1);
    send_event(physical, INPUT_EV_KEY, INPUT_BTN_0, 0, true);
    assert(admitted() == 0);
    assert(next_report().buttons == 0);
    printk("PASS: runtime_rotation_buttons_and_scroll\n");

    for (int i = 0; i < 5; i++) {
        send_event(physical, INPUT_EV_REL, INPUT_REL_X, 100, i == 4);
    }
    assert(admitted() < 0);
    xy(1, 2);
    expect_xy(-2, 1);
    assert(k_sem_take(&report_ready, K_NO_WAIT) != 0);
    printk("PASS: runtime_rotation_oversized_recovery\n");

    atomic_set(&pause_output, 1);
    xy(1, 1);
    assert(k_sem_take(&pause_entered, K_SECONDS(2)) == 0);
    xy(2, 2);
    xy(3, 3);
    send_event(physical, INPUT_EV_REL, INPUT_REL_X, 99, false);
    send_event(physical, INPUT_EV_REL, INPUT_REL_Y, 99, true);
    assert(admitted() < 0);
    k_sem_give(&pause_release);
    expect_xy(-1, 1);
    expect_xy(-2, 2);
    expect_xy(-3, 3);
    xy(4, 5);
    expect_xy(-5, 4);
    assert(k_sem_take(&report_ready, K_NO_WAIT) != 0);
    printk("PASS: runtime_rotation_queue_recovery\n");
    exit(0);
}
K_THREAD_DEFINE(rotation_test_runner, 4096, rotation_test_run, NULL, NULL, NULL, 5, 0, 0);
