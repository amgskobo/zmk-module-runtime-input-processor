/* SPDX-License-Identifier: MIT */
/* Actual core/custom dispatch, codec and backend write-mode boundaries. */
static int boundary_reload_scale(const struct device *dev, uint32_t expected) {
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, 97, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY) == 0);
    REVIEW_REQUIRE(settings_load_subtree("custom_settings") == 0);
    zmk_input_processor_runtime_test_apply_persisted_settings();
    struct zmk_input_processor_runtime_config config;
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0);
    REVIEW_REQUIRE(config.scale_multiplier == expected);
    return 0;
}

static int boundary_scale_request(cormoran_rip_WriteMode mode, uint32_t value,
                                  cormoran_rip_Response *response) {
    cormoran_rip_Request request = cormoran_rip_Request_init_zero;
    request.which_request_type = cormoran_rip_Request_set_scale_multiplier_tag;
    request.request_type.set_scale_multiplier.value = value;
    request.request_type.set_scale_multiplier.write_mode = mode;
    return boundary_rip_rpc(&request, response);
}

static int test_boundary_rpc_write_modes(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    static cormoran_rip_Response response;
    REVIEW_REQUIRE(dev);
    zmk_studio_core_unlock();
    REVIEW_REQUIRE(zmk_input_processor_runtime_reset(dev) >= 0);
    zmk_input_processor_runtime_test_flush_save(dev);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_inertia_enabled(
                       dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);

    /* Mode zero is omitted by proto3 encoding: retain older-client PERSIST.
     * MEMORY and TEMPORARY remain live-only after a real backend reload. */
    for (unsigned int mode = 0; mode <= 2; mode++) {
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                           dev, 5, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST) >= 0);
        zmk_input_processor_runtime_test_flush_save(dev);
        REVIEW_REQUIRE(boundary_scale_request((cormoran_rip_WriteMode)mode, 41 + mode, &response) ==
                       0);
        REVIEW_REQUIRE(response.which_response_type ==
                       cormoran_rip_Response_set_scale_multiplier_tag);
        struct zmk_input_processor_runtime_config config;
        REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0);
        REVIEW_REQUIRE(config.scale_multiplier == (mode == 2 ? 5 : 41 + mode));
        REVIEW_REQUIRE(boundary_driver_value(dev, INPUT_REL_X, 1, 41 + mode) == 0);
        zmk_input_processor_runtime_test_flush_save(dev);
        REVIEW_REQUIRE(boundary_reload_scale(dev, mode == 0 ? 41 : 5) == 0);
    }
    LOG_INF("PASS: rip_rpc_known_write_modes");

    unsigned int saves = test_settings_save_calls;
    zmk_studio_core_lock();
    REVIEW_REQUIRE(boundary_scale_request((cormoran_rip_WriteMode)3, 43, &response) == 0);
    REVIEW_REQUIRE(response.which_response_type == cormoran_rip_Response_error_tag);
    REVIEW_REQUIRE(!strcmp(response.response_type.error.message, "Studio unlock required"));
    zmk_input_processor_runtime_test_flush_save(dev);
    REVIEW_REQUIRE(test_settings_save_calls == saves);
    REVIEW_REQUIRE(boundary_reload_scale(dev, 5) == 0);
    zmk_studio_core_unlock();

    const cormoran_rip_WriteMode modes[] = {cormoran_rip_WriteMode_WRITE_MODE_MEMORY,
                                            (cormoran_rip_WriteMode)3, (cormoran_rip_WriteMode)123};
    for (size_t m = 0; m < ARRAY_SIZE(modes); m++) {
        const cormoran_rip_WriteMode mode = modes[m];
#define MODE_REQUEST(member, field, value_)                                                        \
    {                                                                                              \
        .which_request_type = cormoran_rip_Request_##member##_tag,                                 \
        .request_type.member = {.id = 0, .field = value_, .write_mode = mode},                     \
    }
        const cormoran_rip_Request requests[] = {
            MODE_REQUEST(set_scale_multiplier, value, 43),
            MODE_REQUEST(set_scale_divisor, value, 3),
            MODE_REQUEST(set_rotation, value, 0),
            MODE_REQUEST(set_temp_layer_enabled, enabled, true),
            MODE_REQUEST(set_temp_layer_layer, layer, 1),
            MODE_REQUEST(set_temp_layer_activation_delay, activation_delay_ms, 17),
            MODE_REQUEST(set_temp_layer_deactivation_delay, deactivation_delay_ms, 19),
            MODE_REQUEST(set_active_layers, layers, 2),
            MODE_REQUEST(set_axis_snap_mode, mode, cormoran_rip_AxisSnapMode_AXIS_SNAP_MODE_X),
            MODE_REQUEST(set_axis_snap_threshold, threshold, 101),
            MODE_REQUEST(set_axis_snap_timeout, timeout_ms, 100),
            MODE_REQUEST(set_xy_to_scroll_enabled, enabled, true),
            MODE_REQUEST(set_xy_swap_enabled, enabled, true),
            MODE_REQUEST(set_x_invert, invert, true),
            MODE_REQUEST(set_y_invert, invert, true),
            MODE_REQUEST(set_inertia_interval, interval_ms, 4),
            MODE_REQUEST(set_inertia_window, window_ms, 40),
            MODE_REQUEST(set_inertia_threshold, threshold, 2),
            MODE_REQUEST(set_inertia_enabled, enabled, true),
            MODE_REQUEST(set_inertia_decay, decay_percent, 40),
            MODE_REQUEST(set_inertia_normal_max_output, max_output, 5),
            MODE_REQUEST(set_inertia_fast_threshold, threshold, 6),
            MODE_REQUEST(set_inertia_fast_output_percent, percent, 150),
        };
#undef MODE_REQUEST
        for (size_t i = 0; i < ARRAY_SIZE(requests); i++) {
            if (mode == cormoran_rip_WriteMode_WRITE_MODE_MEMORY) {
                /* The identical id/payload must be accepted with a valid mode.
                 * A range refusal cannot stand in for the unknown-mode guard. */
                saves = test_settings_save_calls;
                REVIEW_REQUIRE(boundary_rip_rpc(&requests[i], &response) == 0);
                REVIEW_REQUIRE(response.which_response_type != cormoran_rip_Response_error_tag);
                zmk_input_processor_runtime_test_flush_save(dev);
                REVIEW_REQUIRE(test_settings_save_calls == saves);
                REVIEW_REQUIRE(boundary_reload_scale(dev, 5) == 0);
                continue;
            }
            struct zmk_input_processor_runtime_config before = {0}, after = {0};
            struct zmk_custom_setting_value blob_before, blob_after;
            REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &before) == 0);
            REVIEW_REQUIRE(zmk_custom_setting_read_by_key("cormoran_rip", TEST_PROCESSOR_NAME,
                                                          &blob_before) == 0);
            saves = test_settings_save_calls;
            REVIEW_REQUIRE(boundary_rip_rpc(&requests[i], &response) == 0);
            REVIEW_REQUIRE(response.which_response_type == cormoran_rip_Response_error_tag);
            REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &after) == 0);
            REVIEW_REQUIRE(!memcmp(&before, &after, sizeof(before)));
            REVIEW_REQUIRE(boundary_driver_value(dev, INPUT_REL_X, 1, 5) == 0);
            REVIEW_REQUIRE(boundary_driver_value(dev, INPUT_REL_Y, 1, 5) == 0);
            zmk_input_processor_runtime_test_flush_save(dev);
            REVIEW_REQUIRE(test_settings_save_calls == saves);
            REVIEW_REQUIRE(zmk_custom_setting_read_by_key("cormoran_rip", TEST_PROCESSOR_NAME,
                                                          &blob_after) == 0);
            REVIEW_REQUIRE(
                blob_after.size == blob_before.size &&
                !memcmp(blob_after.bytes_value, blob_before.bytes_value, blob_before.size));
            REVIEW_REQUIRE(boundary_reload_scale(dev, 5) == 0);
        }
        if (mode == cormoran_rip_WriteMode_WRITE_MODE_MEMORY) {
            LOG_INF("PASS: rip_rpc_mode_payload_controls");
        }
    }

    /* Refusal must also preserve an earlier accepted debounced snapshot. */
    saves = test_settings_save_calls;
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, 7, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST) >= 0);
    REVIEW_REQUIRE(boundary_scale_request((cormoran_rip_WriteMode)123, 44, &response) == 0);
    REVIEW_REQUIRE(response.which_response_type == cormoran_rip_Response_error_tag);
    struct zmk_input_processor_runtime_config config;
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0);
    REVIEW_REQUIRE(config.scale_multiplier == 7);
    zmk_input_processor_runtime_test_flush_save(dev);
    REVIEW_REQUIRE(test_settings_save_calls == saves + 1);
    REVIEW_REQUIRE(boundary_reload_scale(dev, 7) == 0);
    LOG_INF("PASS: rip_rpc_write_mode_validation");
    return 0;
}
