/* SPDX-License-Identifier: MIT
 * Actual core/custom dispatch, codecs, driver and settings lifecycle boundaries.
 */
#include <limits.h>
#include <cormoran/zmk/custom_settings/custom_settings.pb.h>
#include <zmk/studio/core.h>
#include <zmk/studio/rpc.h>

static int boundary_subsystem_index(const char *identifier) {
    int index = 0;
    STRUCT_SECTION_FOREACH(zmk_rpc_custom_subsystem, subsystem) {
        if (!strcmp(subsystem->identifier, identifier)) {
            return index;
        }
        index++;
    }
    return -ENOENT;
}

static int boundary_dispatch(int index, const pb_msgdesc_t *fields, const void *request,
                             zmk_studio_Response *response) {
    static zmk_studio_Request envelope;
    envelope = (zmk_studio_Request)zmk_studio_Request_init_zero;
    envelope.which_subsystem = zmk_studio_Request_custom_tag;
    envelope.subsystem.custom.which_request_type = zmk_custom_Request_call_tag;
    zmk_custom_CallRequest *call = &envelope.subsystem.custom.request_type.call;
    call->subsystem_index = index;
    pb_ostream_t output = pb_ostream_from_buffer(call->payload.bytes, sizeof(call->payload.bytes));
    REVIEW_REQUIRE(pb_encode(&output, fields, request));
    call->payload.size = output.bytes_written;
    STRUCT_SECTION_FOREACH(zmk_rpc_subsystem, subsystem) {
        if (subsystem->subsystem_choice == zmk_studio_Request_custom_tag) {
            *response = subsystem->func(subsystem, &envelope);
            return 0;
        }
    }
    return -ENOENT;
}

struct boundary_decode_target {
    const pb_msgdesc_t *fields;
    void *message;
};

static bool boundary_decode_payload(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    struct boundary_decode_target *target = *arg;
    return pb_decode(stream, target->fields, target->message);
}

static int boundary_decode(zmk_studio_Response *response, const pb_msgdesc_t *fields,
                           void *message) {
    REVIEW_REQUIRE(response->type.request_response.which_subsystem ==
                   zmk_studio_RequestResponse_custom_tag);
    REVIEW_REQUIRE(response->type.request_response.subsystem.custom.which_response_type ==
                   zmk_custom_Response_call_tag);
    static uint8_t bytes[1024];
    pb_ostream_t output = pb_ostream_from_buffer(bytes, sizeof(bytes));
    REVIEW_REQUIRE(pb_encode(&output, zmk_custom_CallResponse_fields,
                             &response->type.request_response.subsystem.custom.response_type.call));
    struct boundary_decode_target target = {fields, message};
    zmk_custom_CallResponse call = zmk_custom_CallResponse_init_zero;
    call.payload.funcs.decode = boundary_decode_payload;
    call.payload.arg = &target;
    pb_istream_t input = pb_istream_from_buffer(bytes, output.bytes_written);
    REVIEW_REQUIRE(pb_decode(&input, zmk_custom_CallResponse_fields, &call));
    return 0;
}

static int boundary_rip_rpc(const cormoran_rip_Request *request, cormoran_rip_Response *response) {
    static zmk_studio_Response envelope;
    int index = boundary_subsystem_index("cormoran_rip");
    REVIEW_REQUIRE(index >= 0);
    REVIEW_REQUIRE(boundary_dispatch(index, cormoran_rip_Request_fields, request, &envelope) == 0);
    *response = (cormoran_rip_Response)cormoran_rip_Response_init_zero;
    return boundary_decode(&envelope, cormoran_rip_Response_fields, response);
}

static int test_boundary_rpc_authorization(void) {
    static cormoran_rip_Request request;
    static cormoran_rip_Response response;
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    REVIEW_REQUIRE(dev && IS_ENABLED(CONFIG_ZMK_STUDIO_LOCKING));
    zmk_input_processor_runtime_reset(dev);
    zmk_studio_core_lock();
    const pb_size_t reads[] = {cormoran_rip_Request_list_input_processors_tag,
                               cormoran_rip_Request_get_input_processor_tag,
                               cormoran_rip_Request_get_layer_info_tag};
    for (size_t i = 0; i < ARRAY_SIZE(reads); i++) {
        request = (cormoran_rip_Request)cormoran_rip_Request_init_zero;
        request.which_request_type = reads[i];
        REVIEW_REQUIRE(boundary_rip_rpc(&request, &response) == 0);
        REVIEW_REQUIRE(response.which_response_type != cormoran_rip_Response_error_tag);
    }
    const pb_size_t mutations[] = {
        cormoran_rip_Request_set_scale_multiplier_tag,
        cormoran_rip_Request_set_scale_divisor_tag,
        cormoran_rip_Request_set_rotation_tag,
        cormoran_rip_Request_reset_input_processor_tag,
        cormoran_rip_Request_set_temp_layer_enabled_tag,
        cormoran_rip_Request_set_temp_layer_layer_tag,
        cormoran_rip_Request_set_temp_layer_activation_delay_tag,
        cormoran_rip_Request_set_temp_layer_deactivation_delay_tag,
        cormoran_rip_Request_set_active_layers_tag,
        cormoran_rip_Request_set_axis_snap_mode_tag,
        cormoran_rip_Request_set_axis_snap_threshold_tag,
        cormoran_rip_Request_set_axis_snap_timeout_tag,
        cormoran_rip_Request_set_xy_to_scroll_enabled_tag,
        cormoran_rip_Request_set_xy_swap_enabled_tag,
        cormoran_rip_Request_set_x_invert_tag,
        cormoran_rip_Request_set_y_invert_tag,
        cormoran_rip_Request_save_all_settings_tag,
        cormoran_rip_Request_discard_all_settings_tag,
        cormoran_rip_Request_reset_all_settings_tag,
        cormoran_rip_Request_set_inertia_interval_tag,
        cormoran_rip_Request_set_inertia_threshold_tag,
        cormoran_rip_Request_set_inertia_enabled_tag,
        cormoran_rip_Request_set_inertia_decay_tag,
        cormoran_rip_Request_set_inertia_normal_max_output_tag,
        cormoran_rip_Request_set_inertia_notifications_tag,
        cormoran_rip_Request_set_inertia_window_tag,
        cormoran_rip_Request_set_inertia_fast_threshold_tag,
        cormoran_rip_Request_set_inertia_fast_output_percent_tag,
        0, /* Unknown/empty requests must not bypass the locked allowlist. */
    };
    for (size_t i = 0; i < ARRAY_SIZE(mutations); i++) {
        request = (cormoran_rip_Request)cormoran_rip_Request_init_zero;
        request.which_request_type = mutations[i];
        REVIEW_REQUIRE(boundary_rip_rpc(&request, &response) == 0);
        REVIEW_REQUIRE(response.which_response_type == cormoran_rip_Response_error_tag);
        REVIEW_REQUIRE(!strcmp(response.response_type.error.message, "Studio unlock required"));
    }
    zmk_studio_core_unlock();
    request = (cormoran_rip_Request)cormoran_rip_Request_init_zero;
    request.which_request_type = cormoran_rip_Request_set_scale_multiplier_tag;
    request.request_type.set_scale_multiplier.value = 23;
    request.request_type.set_scale_multiplier.write_mode = cormoran_rip_WriteMode_WRITE_MODE_MEMORY;
    REVIEW_REQUIRE(boundary_rip_rpc(&request, &response) == 0);
    REVIEW_REQUIRE(response.which_response_type == cormoran_rip_Response_set_scale_multiplier_tag);
    struct zmk_input_processor_runtime_config config;
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0);
    REVIEW_REQUIRE(config.scale_multiplier == 23);
    LOG_INF("PASS: rip_locked_reads_and_mutations");
    return 0;
}

static bool boundary_encode_bytes(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct zmk_custom_setting_value *value = *arg;
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_string(stream, value->bytes_value, value->size);
}

static int boundary_write_blob(const struct zmk_custom_setting_value *value,
                               cormoran_zmk_custom_settings_Response *response) {
    static cormoran_zmk_custom_settings_Request request;
    static zmk_studio_Response envelope;
    int index = boundary_subsystem_index("cormoran_custom_settings");
    REVIEW_REQUIRE(index >= 0);
    request = (cormoran_zmk_custom_settings_Request)cormoran_zmk_custom_settings_Request_init_zero;
    request.which_request_type = cormoran_zmk_custom_settings_Request_write_setting_tag;
    cormoran_zmk_custom_settings_WriteSettingRequest *write = &request.request_type.write_setting;
    write->has_setting = true;
    write->setting.has_custom_subsystem_index = true;
    write->setting.custom_subsystem_index = boundary_subsystem_index("cormoran_rip");
    write->setting.has_key = true;
    strcpy(write->setting.key, TEST_PROCESSOR_NAME);
    write->has_value = true;
    write->value.which_value_type = cormoran_zmk_custom_settings_SettingValue_bytes_value_tag;
    write->value.value_type.bytes_value.funcs.encode = boundary_encode_bytes;
    write->value.value_type.bytes_value.arg = (void *)value;
    write->mode = cormoran_zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST;
    REVIEW_REQUIRE(boundary_dispatch(index, cormoran_zmk_custom_settings_Request_fields, &request,
                                     &envelope) == 0);
    *response =
        (cormoran_zmk_custom_settings_Response)cormoran_zmk_custom_settings_Response_init_zero;
    return boundary_decode(&envelope, cormoran_zmk_custom_settings_Response_fields, response);
}

static int test_boundary_generic_authorization(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    struct zmk_custom_setting_value desired, before, after;
    static cormoran_zmk_custom_settings_Response response;
    REVIEW_REQUIRE(dev);
    zmk_input_processor_runtime_reset(dev);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, 17, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST) >= 0);
    zmk_input_processor_runtime_test_flush_save(dev);
    REVIEW_REQUIRE(zmk_custom_setting_read_by_key("cormoran_rip", TEST_PROCESSOR_NAME, &desired) ==
                   0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, 5, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST) >= 0);
    zmk_input_processor_runtime_test_flush_save(dev);
    REVIEW_REQUIRE(zmk_custom_setting_read_by_key("cormoran_rip", TEST_PROCESSOR_NAME, &before) ==
                   0);
    zmk_studio_core_lock();
    REVIEW_REQUIRE(boundary_write_blob(&desired, &response) == 0);
    REVIEW_REQUIRE(response.which_response_type == cormoran_zmk_custom_settings_Response_error_tag);
    REVIEW_REQUIRE(zmk_custom_setting_read_by_key("cormoran_rip", TEST_PROCESSOR_NAME, &after) ==
                   0);
    REVIEW_REQUIRE(after.size == before.size &&
                   !memcmp(after.bytes_value, before.bytes_value, before.size));
    REVIEW_REQUIRE(settings_load_subtree("custom_settings") == 0);
    zmk_input_processor_runtime_test_apply_persisted_settings();
    struct zmk_input_processor_runtime_config config;
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0 &&
                   config.scale_multiplier == 5);
    zmk_studio_core_unlock();
    REVIEW_REQUIRE(boundary_write_blob(&desired, &response) == 0);
    REVIEW_REQUIRE(response.which_response_type ==
                       cormoran_zmk_custom_settings_Response_status_tag &&
                   response.response_type.status.affected_count == 1);
    /* Design A: generic persistence does not automatically replace live settings. */
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0 &&
                   config.scale_multiplier == 5);
    REVIEW_REQUIRE(settings_load_subtree("custom_settings") == 0);
    zmk_input_processor_runtime_test_apply_persisted_settings();
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0 &&
                   config.scale_multiplier == 17);
    LOG_INF("PASS: rip_generic_blob_authorization");
    return 0;
}

static int boundary_driver_value(const struct device *dev, uint16_t code, int32_t value,
                                 int32_t expected) {
    struct input_event event = {.type = INPUT_EV_REL, .code = code, .value = value, .sync = true};
    REVIEW_REQUIRE(zmk_input_processor_handle_event(dev, &event, 0, 0, NULL) ==
                   ZMK_INPUT_PROC_CONTINUE);
    REVIEW_REQUIRE(event.value == expected);
    return 0;
}

static int test_boundary_driver_values(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    REVIEW_REQUIRE(dev);
    zmk_input_processor_runtime_reset(dev);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_inertia_enabled(
                       dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, 1, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_x_invert(
                       dev, true, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_y_invert(
                       dev, true, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    for (uint16_t axis = INPUT_REL_X; axis <= INPUT_REL_Y; axis++) {
        REVIEW_REQUIRE(boundary_driver_value(dev, axis, INT32_MIN + 1, INT16_MAX) == 0);
        REVIEW_REQUIRE(boundary_driver_value(dev, axis, INT32_MIN, INT16_MAX) == 0);
    }
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_x_invert(
                       dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_y_invert(
                       dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    for (uint32_t mode = 1; mode <= 2; mode++) {
        for (int direction = -1; direction <= 1; direction += 2) {
            uint16_t code = mode == 1 ? INPUT_REL_Y : INPUT_REL_X;
            REVIEW_REQUIRE(
                zmk_input_processor_runtime_set_axis_snap(
                    dev, mode, 100, 1000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
            REVIEW_REQUIRE(boundary_driver_value(dev, code, direction * 100, direction * 100) == 0);
            REVIEW_REQUIRE(
                zmk_input_processor_runtime_set_axis_snap(
                    dev, mode, 100, 1000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
            REVIEW_REQUIRE(boundary_driver_value(dev, code, direction * 65536,
                                                 direction > 0 ? INT16_MAX : INT16_MIN) == 0);
        }
    }
    LOG_INF("PASS: rip_full_width_driver_boundaries");
    return 0;
}

static int test_boundary_discard_snap(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    REVIEW_REQUIRE(dev);
    zmk_input_processor_runtime_reset(dev);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, 1, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    for (uint32_t mode = 1; mode <= 2; mode++) {
        uint16_t code = mode == 1 ? INPUT_REL_Y : INPUT_REL_X;
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap(
                           dev, mode, 100, 60000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST) >=
                       0);
        zmk_input_processor_runtime_test_flush_save(dev);
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap(
                           dev, mode, 1000, 60000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) ==
                       0);
        REVIEW_REQUIRE(boundary_driver_value(dev, code, 90, 0) == 0);
        REVIEW_REQUIRE(zmk_input_processor_runtime_discard_all() == 0);
        struct zmk_input_processor_runtime_config config;
        REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0 &&
                       config.axis_snap_threshold == 100 && config.axis_snap_mode == mode);
        REVIEW_REQUIRE(boundary_driver_value(dev, code, 10, 0) == 0);
        REVIEW_REQUIRE(zmk_input_processor_runtime_set_axis_snap(
                           dev, mode, 100, 60000, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) ==
                       0);
        REVIEW_REQUIRE(boundary_driver_value(dev, code, 10, 0) == 0);
    }
    LOG_INF("PASS: rip_discard_retires_snap_state");
    return 0;
}
