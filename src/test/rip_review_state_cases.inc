/* SPDX-License-Identifier: MIT
 * Production driver, typed registry, loader and actual layer work boundaries.
 */
static int test_review_snap_setting_retirement(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    REVIEW_REQUIRE(dev);
    zmk_input_processor_runtime_reset(dev);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_inertia_enabled(
                       dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    for (uint32_t mode = 1; mode <= 2; mode++) {
        uint16_t code = mode == 1 ? INPUT_REL_Y : INPUT_REL_X;
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap(
                           dev, mode, 100, 60000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) ==
                       0);
        REVIEW_REQUIRE(boundary_driver_value(dev, code, 90, 0) == 0);
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap_threshold(
                           dev, 150, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
        REVIEW_REQUIRE(boundary_driver_value(dev, code, 60, 0) == 0);
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap(
                           dev, mode, 100, 60000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) ==
                       0);
        REVIEW_REQUIRE(boundary_driver_value(dev, code, 90, 0) == 0);
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap_timeout(
                           dev, 59000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
        REVIEW_REQUIRE(boundary_driver_value(dev, code, 60, 0) == 0);
    }
    LOG_INF("PASS: rip_snap_setting_retirement");
    return 0;
}

static int test_review_invalid_snap_blob(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    REVIEW_REQUIRE(dev);
    zmk_input_processor_runtime_reset(dev);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_inertia_enabled(
                       dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap(
                       dev, 2, 100, 60000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST) >= 0);
    zmk_input_processor_runtime_test_flush_save(dev);
    struct zmk_custom_setting_value snapped;
    REVIEW_REQUIRE(zmk_custom_setting_read_by_key("cormoran_rip", TEST_PROCESSOR_NAME, &snapped) ==
                   0);
    size_t mode_offset = zmk_input_processor_runtime_test_snap_mode_offset();
    REVIEW_REQUIRE(snapped.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
                   mode_offset < snapped.size && snapped.bytes_value[mode_offset] == 2);
    struct zmk_input_processor_runtime_config before, after;
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &before) == 0);
    const uint8_t invalid_modes[] = {3, UINT8_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(invalid_modes); i++) {
        snapped.bytes_value[mode_offset] = invalid_modes[i];
        REVIEW_REQUIRE(zmk_custom_setting_write_by_key("cormoran_rip", TEST_PROCESSOR_NAME,
                                                       &snapped,
                                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0);
        zmk_input_processor_runtime_test_apply_persisted_settings();
        REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &after) == 0);
        REVIEW_REQUIRE(after.axis_snap_mode == 2 &&
                       after.axis_snap_threshold == before.axis_snap_threshold &&
                       after.scale_multiplier == before.scale_multiplier &&
                       after.scale_divisor == before.scale_divisor &&
                       after.rotation_degrees == before.rotation_degrees);
        REVIEW_REQUIRE(boundary_driver_value(dev, INPUT_REL_Y, 10, 10) == 0);
    }
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap(
                       dev, 0, 100, 60000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST) >= 0);
    zmk_input_processor_runtime_test_flush_save(dev);
    LOG_INF("PASS: rip_invalid_snap_blob");
    return 0;
}

static int review_wait_layer(bool active) {
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        if (zmk_keymap_layer_active(1) == active) {
            return 0;
        }
        k_sleep(K_MSEC(1));
    }
    return -ETIMEDOUT;
}

static int test_review_disabled_temp_layer(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    REVIEW_REQUIRE(dev);
    zmk_input_processor_runtime_reset(dev);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_inertia_enabled(
                       dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    for (unsigned combined = 0; combined < 2; combined++) {
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_temp_layer(
                           dev, true, 1, 0, 20, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) ==
                       0);
        zmk_input_processor_runtime_temp_layer_keep_active(dev, true);
        REVIEW_REQUIRE(boundary_driver_value(dev, INPUT_REL_X, 1, 1) == 0);
        REVIEW_REQUIRE(review_wait_layer(true) == 0);
        k_sleep(K_MSEC(30));
        REVIEW_REQUIRE(zmk_keymap_layer_active(1));
        if (combined) {
            REVIEW_REQUIRE(
                zmk_input_processor_runtime_set_temp_layer(
                    dev, false, 1, 0, 20, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
        } else {
            REVIEW_REQUIRE(zmk_input_processor_runtime_set_temp_layer_enabled(
                               dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
        }
        REVIEW_REQUIRE(review_wait_layer(false) == 0);
        zmk_input_processor_runtime_temp_layer_keep_active(dev, false);
        REVIEW_REQUIRE(!zmk_keymap_layer_active(1));
    }
    LOG_INF("PASS: rip_disabled_temp_layer");
    return 0;
}
