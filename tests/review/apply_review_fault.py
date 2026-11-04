#!/usr/bin/env python3
"""Prepare one identified fault in an isolated container owner staging copy."""

import argparse
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def function_span(source, name):
    found = re.search(
        r"^(?:static\s+)?(?:int|void)\s+" + re.escape(name) + r"\(", source, re.M
    )
    if not found:
        raise ValueError("Missing production function: " + name)
    opening = source.index("{", found.end())
    depth = 0
    tokens = re.finditer(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        source[opening:],
        re.S,
    )
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return found.start(), opening + token.end()
    raise ValueError("Unterminated production function")


def replace_function(source, name, change):
    first, last = function_span(source, name)
    before = source[first:last]
    after = change(before)
    if after == before:
        raise ValueError("Fault did not change its selected function")
    return source[:first] + after + source[last:]


def remove_residual_reset(body):
    for field in ("axis_snap_cross_axis_accum", "axis_snap_last_decay_timestamp"):
        body, count = re.subn(r"\s*data->" + field + r"\s*=\s*0;", "", body)
        if count != 1:
            raise ValueError("Expected exactly one residual reset")
    return body


def direct_inertia(_body):
    return r"""static int inertia_emit(struct runtime_processor_data *data, uint16_t code,
                        int16_t value) {
#if IS_ENABLED(CONFIG_ZMK_POINTING) && IS_ENABLED(CONFIG_INPUT_MODE_THREAD)
    const struct runtime_processor_config *cfg = data->dev->config;
    struct input_event event = {
        .dev = cfg->inertia_input, .type = INPUT_EV_MSC, .code = INPUT_MSC_SCAN,
        .value = (int32_t)atomic_get(&data->inertia_input_epoch),
    };
    inertia_input_callback(&event, NULL);
    event.type = INPUT_EV_REL;
    event.code = code;
    event.value = value;
    event.sync = true;
    inertia_input_callback(&event, NULL);
    return 0;
#else
    ARG_UNUSED(data);
    ARG_UNUSED(code);
    ARG_UNUSED(value);
    return -ENOTSUP;
#endif
}"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "fault",
        choices=(
            "frame-guard",
            "inertia-thread",
            "snap-threshold",
            "snap-timeout",
            "snap-blob",
            "keep-active",
            "activity-selection",
        ),
    )
    parser.add_argument("isolated_owner", type=Path)
    args = parser.parse_args()
    target = args.isolated_owner.resolve()
    if target == ROOT.resolve() or ROOT.resolve().is_relative_to(target):
        raise SystemExit("Refusing to mutate the operator's owner checkout")
    relative = {
        "frame-guard": "src/pointing/runtime_input_pipeline.c",
        "activity-selection": "web/src/App.tsx",
    }.get(args.fault, "src/pointing/input_processor_runtime.c")
    path = target / relative
    if not path.resolve().is_relative_to(target):
        raise SystemExit("Refusing a source link outside the isolated owner")
    source = path.read_text()
    if args.fault == "frame-guard":
        for method in ("lock", "unlock"):
            source, count = re.subn(
                r"\s*zmk_input_processor_runtime_frame_" + method + r"\(\);",
                "",
                source,
            )
            if count != 1:
                raise ValueError("Expected one complete-frame guard operation")
    elif args.fault == "inertia-thread":
        source = replace_function(source, "inertia_emit", direct_inertia)
    elif args.fault in ("snap-threshold", "snap-timeout"):
        setter = "threshold" if args.fault == "snap-threshold" else "timeout"
        source = replace_function(
            source,
            "zmk_input_processor_runtime_set_axis_snap_" + setter + "_locked",
            remove_residual_reset,
        )
    elif args.fault == "snap-blob":
        source, count = re.subn(
            r"\s*\|\|\s*v1->axis_snap_mode\s*>\s*ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_Y",
            "",
            source,
        )
        if count != 1:
            raise ValueError("Expected one saved snap-mode guard")
    elif args.fault == "keep-active":
        source, count = re.subn(
            r"\(data->temp_layer_enabled\s*&&\s*data->temp_layer_keep_active\)",
            "data->temp_layer_keep_active",
            source,
        )
        if count != 1:
            raise ValueError("Expected one disabled-layer keep-active guard")
    else:
        source, count = re.subn(
            r"\s*if\s*\(generation\s*!==\s*inertiaToggleGeneration.current\)\s*return;",
            "",
            source,
        )
        if count != 2:
            raise ValueError("Expected success and rejection retirement guards")
        source, count = re.subn(
            r"const generation\s*=\s*\+\+inertiaToggleGeneration.current;",
            "++inertiaToggleGeneration.current;",
            source,
        )
        if count != 1:
            raise ValueError("Expected one activity request ticket")
    path.write_text(source)
    print("Applied review fault: " + args.fault)


if __name__ == "__main__":
    main()
