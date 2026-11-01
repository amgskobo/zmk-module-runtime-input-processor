/* SPDX-License-Identifier: MIT
 * Real custom-subsystem dispatch and nanopb envelopes, inside the native owner.
 */
#include <pb_encode.h>
#include <pb_decode.h>
#include <cormoran/rip/custom.pb.h>
#include <zmk/studio/custom.h>

#define REVIEW_REQUIRE(condition)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            LOG_ERR("FAIL: rip_review line=%d", __LINE__);                                         \
            return -EINVAL;                                                                        \
        }                                                                                          \
    } while (0)

static bool review_decode_response(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    return pb_decode(stream, cormoran_rip_Response_fields, *arg);
}

static int review_rpc(const cormoran_rip_Request *request, cormoran_rip_Response *response) {
    static zmk_custom_CallRequest raw;
    static uint8_t encoded[512];
    raw = (zmk_custom_CallRequest)zmk_custom_CallRequest_init_zero;
    pb_ostream_t stream = pb_ostream_from_buffer(raw.payload.bytes, sizeof(raw.payload.bytes));
    REVIEW_REQUIRE(pb_encode(&stream, cormoran_rip_Request_fields, request));
    raw.payload.size = stream.bytes_written;
    zmk_custom_CallResponse envelope = zmk_custom_CallResponse_init_zero;
    bool handled = false;
    STRUCT_SECTION_FOREACH(zmk_rpc_custom_subsystem, subsystem) {
        if (!strcmp(subsystem->identifier, "cormoran_rip")) {
            handled = subsystem->handler(&raw, &envelope.payload);
            break;
        }
    }
    REVIEW_REQUIRE(handled);
    stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    REVIEW_REQUIRE(pb_encode(&stream, zmk_custom_CallResponse_fields, &envelope));
    *response = (cormoran_rip_Response)cormoran_rip_Response_init_zero;
    envelope = (zmk_custom_CallResponse)zmk_custom_CallResponse_init_zero;
    envelope.payload.funcs.decode = review_decode_response;
    envelope.payload.arg = response;
    pb_istream_t input = pb_istream_from_buffer(encoded, stream.bytes_written);
    REVIEW_REQUIRE(pb_decode(&input, zmk_custom_CallResponse_fields, &envelope));
    return 0;
}

static int test_review_persist_snapshot(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_name(TEST_PROCESSOR_NAME);
    REVIEW_REQUIRE(dev);
    struct zmk_input_processor_runtime_config original;
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &original) == 0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, 7, 3, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST) >= 0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, 11, 5, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    zmk_input_processor_runtime_test_flush_save(dev);
    REVIEW_REQUIRE(zmk_input_processor_runtime_discard_all() == 0);
    struct zmk_input_processor_runtime_config config;
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &config) == 0);
    REVIEW_REQUIRE(config.scale_multiplier == 7 && config.scale_divisor == 3);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_scaling(
                       dev, original.scale_multiplier, original.scale_divisor,
                       ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    LOG_INF("PASS: rip_persist_snapshot");
    return 0;
}

static int test_review_rpc_widths(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_id(0);
    REVIEW_REQUIRE(dev);
    static cormoran_rip_Request request;
    static cormoran_rip_Response response;
    struct zmk_input_processor_runtime_config before, after;
    REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &before) == 0);
    const uint32_t ids[] = {256, 257, UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
        request = (cormoran_rip_Request)cormoran_rip_Request_init_zero;
        request.which_request_type = cormoran_rip_Request_set_scale_multiplier_tag;
        request.request_type.set_scale_multiplier.id = ids[i];
        request.request_type.set_scale_multiplier.value = 123;
        request.request_type.set_scale_multiplier.write_mode =
            cormoran_rip_WriteMode_WRITE_MODE_MEMORY;
        REVIEW_REQUIRE(review_rpc(&request, &response) == 0);
        REVIEW_REQUIRE(response.which_response_type == cormoran_rip_Response_error_tag);
        REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &after) == 0);
        REVIEW_REQUIRE(after.scale_multiplier == before.scale_multiplier);
    }
    const pb_size_t tags[] = {cormoran_rip_Request_set_temp_layer_layer_tag,
                              cormoran_rip_Request_set_temp_layer_activation_delay_tag,
                              cormoran_rip_Request_set_temp_layer_deactivation_delay_tag,
                              cormoran_rip_Request_set_axis_snap_mode_tag,
                              cormoran_rip_Request_set_axis_snap_threshold_tag,
                              cormoran_rip_Request_set_axis_snap_timeout_tag};
    for (size_t i = 0; i < ARRAY_SIZE(tags); i++) {
        request = (cormoran_rip_Request)cormoran_rip_Request_init_zero;
        request.which_request_type = tags[i];
        switch (tags[i]) {
        case cormoran_rip_Request_set_temp_layer_layer_tag:
            request.request_type.set_temp_layer_layer.layer = 256;
            request.request_type.set_temp_layer_layer.write_mode =
                cormoran_rip_WriteMode_WRITE_MODE_MEMORY;
            break;
        case cormoran_rip_Request_set_temp_layer_activation_delay_tag:
            request.request_type.set_temp_layer_activation_delay.activation_delay_ms = 65536;
            request.request_type.set_temp_layer_activation_delay.write_mode =
                cormoran_rip_WriteMode_WRITE_MODE_MEMORY;
            break;
        case cormoran_rip_Request_set_temp_layer_deactivation_delay_tag:
            request.request_type.set_temp_layer_deactivation_delay.deactivation_delay_ms = 65536;
            request.request_type.set_temp_layer_deactivation_delay.write_mode =
                cormoran_rip_WriteMode_WRITE_MODE_MEMORY;
            break;
        case cormoran_rip_Request_set_axis_snap_mode_tag:
            request.request_type.set_axis_snap_mode.mode = 256;
            request.request_type.set_axis_snap_mode.write_mode =
                cormoran_rip_WriteMode_WRITE_MODE_MEMORY;
            break;
        case cormoran_rip_Request_set_axis_snap_threshold_tag:
            request.request_type.set_axis_snap_threshold.threshold = 65536;
            request.request_type.set_axis_snap_threshold.write_mode =
                cormoran_rip_WriteMode_WRITE_MODE_MEMORY;
            break;
        case cormoran_rip_Request_set_axis_snap_timeout_tag:
            request.request_type.set_axis_snap_timeout.timeout_ms = 65536;
            request.request_type.set_axis_snap_timeout.write_mode =
                cormoran_rip_WriteMode_WRITE_MODE_MEMORY;
            break;
        }
        REVIEW_REQUIRE(review_rpc(&request, &response) == 0);
        REVIEW_REQUIRE(response.which_response_type == cormoran_rip_Response_error_tag);
        REVIEW_REQUIRE(zmk_input_processor_runtime_get_config(dev, NULL, &after) == 0);
        REVIEW_REQUIRE(
            after.temp_layer_layer == before.temp_layer_layer &&
            after.temp_layer_activation_delay_ms == before.temp_layer_activation_delay_ms &&
            after.temp_layer_deactivation_delay_ms == before.temp_layer_deactivation_delay_ms &&
            after.axis_snap_mode == before.axis_snap_mode &&
            after.axis_snap_threshold == before.axis_snap_threshold &&
            after.axis_snap_timeout_ms == before.axis_snap_timeout_ms);
    }
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_xy_to_scroll_enabled(
                       dev, true, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    REVIEW_REQUIRE(zmk_input_processor_runtime_set_xy_swap_enabled(
                       dev, true, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    request = (cormoran_rip_Request)cormoran_rip_Request_init_zero;
    request.which_request_type = cormoran_rip_Request_get_input_processor_tag;
    REVIEW_REQUIRE(review_rpc(&request, &response) == 0);
    REVIEW_REQUIRE(response.which_response_type == cormoran_rip_Response_get_input_processor_tag);
    REVIEW_REQUIRE(response.response_type.get_input_processor.has_processor);
    REVIEW_REQUIRE(response.response_type.get_input_processor.processor.xy_to_scroll_enabled &&
                   response.response_type.get_input_processor.processor.xy_swap_enabled);
    REVIEW_REQUIRE(
        zmk_input_processor_runtime_set_xy_to_scroll_enabled(
            dev, before.xy_to_scroll_enabled, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    REVIEW_REQUIRE(
        zmk_input_processor_runtime_set_xy_swap_enabled(
            dev, before.xy_swap_enabled, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
    LOG_INF("PASS: rip_rpc_widths_and_get_flags");
    return 0;
}
