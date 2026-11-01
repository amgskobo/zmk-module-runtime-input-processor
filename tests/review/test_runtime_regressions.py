"""Focused review regressions against unchanged production function bodies.

Run inside the ZMK SDK container:
    python3 -m unittest discover -s tests/review -p test_runtime_regressions.py -v

These are host arithmetic/contact-state checks, not a Zephyr, persistence,
Studio RPC or physical transport test. Production structs/enums/functions are
read from the owner sources; only kernel, layer and notification effects are
stand-ins. Inertia is disabled in these scenarios and is not exercised.
"""

import hashlib
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/pointing/input_processor_runtime.c"


def declaration(text, kind, name):
    match = re.search(
        r"^" + kind + r" " + re.escape(name) + r" \{.*?^\};", text, re.M | re.S
    )
    if match is None:
        raise ValueError("missing declaration: " + name)
    return match.group()


def function(text, name):
    match = re.search(
        r"^(?:static )?[^\s;{}=][^\n;{}=]*\b" + re.escape(name) + r"\(", text, re.M
    )
    if match is None:
        raise ValueError("missing function: " + name)
    opening = text.index("{", match.end())
    # Skip comments and literals so their braces cannot change body boundaries.
    tokens = re.finditer(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        text[opening:],
        re.S,
    )
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                line = text.count("\n", 0, match.start()) + 1
                return (
                    f'#line {line} "{SOURCE}"\n'
                    + text[match.start() : opening + token.end()]
                )
    raise ValueError("unterminated function: " + name)


PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#define IS_ENABLED(x) 0
#define INERTIA_BUCKETS 66
#define RUNTIME_REMAINDER_SOURCES 2
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ARG_UNUSED(x) ((void)(x))
#define CONTAINER_OF(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define ZMK_INPUT_PROC_CONTINUE 0
#define ZMK_INPUT_PROC_STOP 1
#define ZMK_KEYMAP_LAYERS_LEN 8
#define ZMK_KEYMAP_LAYER_ID_INVAL 255
#define INPUT_EV_REL 2
#define INPUT_REL_X 0
#define INPUT_REL_Y 1
#define INPUT_REL_HWHEEL 6
#define INPUT_REL_WHEEL 8
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
typedef uint8_t zmk_keymap_layer_id_t;
typedef int32_t atomic_t;
static void atomic_set(atomic_t *p, atomic_t v) { *p=v; }
static void atomic_inc(atomic_t *p) { ++*p; }
static atomic_t atomic_get(const atomic_t *p) { return *p; }
struct k_work { int reserved; };
struct k_work_delayable { struct k_work work; };
struct device { const void *config; void *data; };
struct input_event { uint8_t type; uint16_t code; int32_t value; bool sync; const struct device *dev; };
struct zmk_input_processor_state { unsigned input_device_index; int16_t *remainder; };
static bool zmk_runtime_input_pipeline_matches(const struct device *p, const struct device *s) { (void)p; (void)s; return false; }
static bool zmk_runtime_input_rotation_matches(const struct device *p, const struct device *s) { (void)p; (void)s; return false; }
static bool zmk_runtime_input_rotation_has_processor(const struct device *p) { (void)p; return false; }
static int64_t now_ms;
static uint32_t active_layers;
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) { return CONTAINER_OF(work, struct k_work_delayable, work); }
static int64_t k_uptime_get(void) { return now_ms; }
static int k_work_reschedule(struct k_work_delayable *w, int64_t t) { (void)w; (void)t; return 0; }
static zmk_keymap_layer_id_t zmk_keymap_layer_index_to_id(int i) { return (uint8_t)i; }
static bool zmk_keymap_layer_active(uint8_t i) { return (active_layers & (1U << i)) != 0; }
static int zmk_keymap_layer_activate(uint8_t i, bool force) { (void)force; active_layers |= 1U << i; return 0; }
static int zmk_keymap_layer_deactivate(uint8_t i, bool force) { (void)force; active_layers &= ~(1U << i); return 0; }
"""

EFFECT_STUBS = r"""
static const char *write_mode_label(enum zmk_input_processor_runtime_write_mode mode) { (void)mode; return ""; }
static void inertia_stop(struct runtime_processor_data *data, enum zmk_input_processor_inertia_stop_reason reason) { (void)data; (void)reason; }
static void inertia_handle_physical_event(struct runtime_processor_data *data, uint8_t axis, struct input_event *event, int16_t raw) { (void)data; (void)axis; (void)event; (void)raw; }
"""

SCENARIOS = r"""
static struct runtime_processor_data data;
static const uint16_t xs[] = {INPUT_REL_X}, ys[] = {INPUT_REL_Y};
static const struct runtime_processor_config config = {
    .type=INPUT_EV_REL, .x_codes_len=1, .y_codes_len=1, .x_codes=xs, .y_codes=ys};
static const struct device device = {.config=&config, .data=&data};
static int32_t send(uint16_t code, int32_t value, bool sync) {
    struct input_event event = {INPUT_EV_REL, code, value, sync, NULL};
    assert(runtime_processor_handle_event(&device, &event, 0, 0, NULL) == 0);
    return event.value;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    setbuf(stdout, NULL);
    data.scale_multiplier = 1;
    data.scale_divisor = 1;
    if (strcmp(argv[1], "scale-control") == 0) {
        assert(send(INPUT_REL_X, 37, true) == 37);
    } else if (strcmp(argv[1], "scale-product") == 0) {
        assert(zmk_input_processor_runtime_set_scaling(&device, 889, 500,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
        int32_t output = send(INPUT_REL_X, 37, true);
        printf("37 * 889 / 500: actual=%d expected=65\n", output);
        assert(output == 65);
    } else if (strcmp(argv[1], "scale-divisor") == 0) {
        int ret = zmk_input_processor_runtime_set_scaling(&device, 1, 65536,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
        printf("set_scaling(1,65536): ret=%d\n", ret);
        if (ret == 0) { assert(send(INPUT_REL_X, 1, true) == 0); }
        else { assert(ret == -EINVAL); }
    } else if (strcmp(argv[1], "scale-boundary") == 0) {
        int16_t remainder = 0;
        struct zmk_input_processor_state state = {.remainder=&remainder};
        for (int direction=-1; direction<=1; direction+=2) {
            int sum=0;
            remainder=0;
            for (unsigned i=0; i<32768; i++) {
                struct input_event event = {INPUT_EV_REL, INPUT_REL_X, direction, true, NULL};
                assert(scale_val(&event, 1, 32768, &state) == 0);
                sum+=event.value;
            }
            assert(sum==direction && remainder==0);
        }
        struct input_event event = {INPUT_EV_REL, INPUT_REL_X, INT32_MAX, true, NULL};
        assert(scale_val(&event, UINT32_MAX, 1, &state) == 0 && event.value == INT16_MAX);
        event.value=INT32_MIN;
        assert(scale_val(&event, UINT32_MAX, 1, &state) == 0 && event.value == INT16_MIN);
    } else if (strcmp(argv[1], "snap-control") == 0 || strcmp(argv[1], "snap-short") == 0) {
        uint16_t timeout = strcmp(argv[1], "snap-short") == 0 ? 1 : 1000;
        assert(zmk_input_processor_runtime_set_axis_snap(&device,
            ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_X, 100, timeout,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
        now_ms = 100;
        assert(send(INPUT_REL_Y, 1, true) == 0);
        now_ms = 200;
        printf("second cross-axis input after 100ms, timeout=%u\n", timeout);
        assert(send(INPUT_REL_Y, 1, true) == 0);
    } else if (strcmp(argv[1], "snap-threshold-boundary") == 0) {
        assert(zmk_input_processor_runtime_set_axis_snap(&device,
            ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_X, UINT16_MAX, 1,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
        now_ms=100;
        for (int i=0; i<6; i++) { assert(send(INPUT_REL_Y, 10000, true) == 0); }
        assert(send(INPUT_REL_Y, 10000, true) == 10000);
        now_ms=INT64_MAX;
        assert(send(INPUT_REL_Y, 1, true) == 0);
    } else if (strcmp(argv[1], "rotation") == 0) {
        struct zmk_runtime_rotation_state first={0}, second={0};
        int16_t x, y;
        data.rotation_degrees=90;
        update_rotation_values(&data);
        assert(zmk_input_processor_runtime_rotate_xy(&device,10,20,&first,&x,&y)==0);
        printf("90-degree frame (10,20): (%d,%d)\n",x,y);
        assert(x==-20 && y==10);
        assert(zmk_input_processor_runtime_rotate_xy(&device,3,4,&first,&x,&y)==0 && x==-4 && y==3);
        assert(zmk_input_processor_runtime_rotate_xy(&device,10,0,&first,&x,&y)==0 && x==0 && y==10);
        assert(zmk_input_processor_runtime_rotate_xy(&device,0,10,&second,&x,&y)==0 && x==-10 && y==0);
        data.rotation_degrees=15;update_rotation_values(&data);
        int sum_x=0,sum_y=0;
        for(int i=0;i<1000;i++) {
            assert(zmk_input_processor_runtime_rotate_xy(&device,1,0,&first,&x,&y)==0);
            sum_x+=x;sum_y+=y;
        }
        assert(sum_x==966 && sum_y==259);
        assert(zmk_input_processor_runtime_rotate_xy(&device,1000,0,&second,&x,&y)==0 && x==966 && y==259);
        data.rotation_degrees=-90;update_rotation_values(&data);
        assert(zmk_input_processor_runtime_rotate_xy(&device,7,5,&first,&x,&y)==0 && x==5 && y==-7);
        data.rotation_degrees=45;update_rotation_values(&data);
        assert(zmk_input_processor_runtime_rotate_xy(&device,INT32_MAX,INT32_MAX,&first,&x,&y)==0 && x==0 && y==INT16_MAX);
        data.active_layers=2;
        assert(zmk_input_processor_runtime_rotate_xy(&device,7,5,&first,&x,&y)==0 && x==7 && y==5);
        data.active_layers=0;
        struct input_event unsupported={INPUT_EV_REL,INPUT_REL_X,10,true,NULL};
        assert(runtime_processor_handle_event(&device,&unsupported,0,0,NULL)==ZMK_INPUT_PROC_CONTINUE);
        assert(unsupported.value==10);
        assert(zmk_input_processor_runtime_set_rotation(&device,15,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY)==-ENOTSUP);
        assert(data.rotation_degrees==45);
        assert(zmk_input_processor_runtime_set_rotation(&device,0,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY)==0);
        assert(data.rotation_degrees==0);
    } else if (strcmp(argv[1], "retarget-layer") == 0) {
        data.temp_layer_enabled = true;
        data.temp_layer_layer = 1;
        data.temp_layer_activation_pending = true;
        data.temp_layer_activation_started_at = now_ms;
        data.last_input_timestamp = now_ms;
        temp_layer_activation_work_handler(&data.temp_layer_activation_work.work);
        assert(zmk_keymap_layer_active(1));
        assert(zmk_input_processor_runtime_set_temp_layer_layer(&device, 2,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY) == 0);
        temp_layer_deactivation_work_handler(&data.temp_layer_deactivation_work.work);
        printf("after retarget 1->2 and timeout: active layers=0x%x\n", active_layers);
        assert(!zmk_keymap_layer_active(1));
        assert(!zmk_keymap_layer_active(2));
        data.temp_layer_activation_pending = true;
        data.temp_layer_activation_started_at = now_ms;
        data.last_input_timestamp = now_ms;
        temp_layer_activation_work_handler(&data.temp_layer_activation_work.work);
        assert(zmk_keymap_layer_active(2));
        temp_layer_deactivation_work_handler(&data.temp_layer_deactivation_work.work);
        assert(!zmk_keymap_layer_active(2));
    } else if (strcmp(argv[1], "settings-remainders") == 0) {
        int16_t xrem=0, yrem=0;
        struct zmk_input_processor_state state={.remainder=&xrem};
        assert(zmk_input_processor_runtime_set_scaling(&device,1,1000,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY)==0);
        struct input_event event={INPUT_EV_REL,INPUT_REL_X,999,true,NULL};
        assert(runtime_processor_handle_event(&device,&event,0,0,&state)==0);
        assert(event.value==0 && xrem==999);
        assert(zmk_input_processor_runtime_set_scaling(&device,1,1,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY)==0);
        event.value=1;
        assert(runtime_processor_handle_event(&device,&event,0,0,&state)==0);
        assert(event.value==1 && xrem==0);
        assert(zmk_input_processor_runtime_set_scaling(&device,1,3,
            ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY)==0);
        event.value=1;
        assert(runtime_processor_handle_event(&device,&event,0,0,&state)==0 && xrem==1);
        event.code=INPUT_REL_Y; event.value=2; state.remainder=&yrem;
        assert(runtime_processor_handle_event(&device,&event,0,0,&state)==0 && yrem==2);
        event.code=INPUT_REL_X; event.value=2; state.remainder=&xrem;
        assert(runtime_processor_handle_event(&device,&event,0,0,&state)==0 && event.value==1 && xrem==0);
    } else if (strcmp(argv[1], "activation-delay") == 0) {
        data.temp_layer_enabled=true; data.temp_layer_layer=1;
        data.temp_layer_activation_delay_ms=100; data.temp_layer_deactivation_delay_ms=500;
        now_ms=1000; assert(send(INPUT_REL_X,1,true)==1);
        temp_layer_activation_work_handler(&data.temp_layer_activation_work.work);
        assert(!zmk_keymap_layer_active(1));
        now_ms=1099; temp_layer_activation_work_handler(&data.temp_layer_activation_work.work);
        assert(!zmk_keymap_layer_active(1));
        now_ms=1100; temp_layer_activation_work_handler(&data.temp_layer_activation_work.work);
        assert(zmk_keymap_layer_active(1));
        temp_layer_deactivation_work_handler(&data.temp_layer_deactivation_work.work);
        now_ms=2000; assert(send(INPUT_REL_X,1,true)==1);
        data.last_keypress_timestamp=2050;
        now_ms=2100; temp_layer_activation_work_handler(&data.temp_layer_activation_work.work);
        assert(!zmk_keymap_layer_active(1));
        now_ms=3000; assert(send(INPUT_REL_X,1,true)==1);
        now_ms=3600; temp_layer_activation_work_handler(&data.temp_layer_activation_work.work);
        assert(!zmk_keymap_layer_active(1));
    } else { assert(!"unknown scenario"); }
    return 0;
}
"""


class RuntimeReviewRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = SOURCE.read_text()
        api = (ROOT / "include/zmk/pointing/input_processor_runtime.h").read_text()
        events = (
            ROOT / "include/zmk/events/input_processor_inertia_state_changed.h"
        ).read_text()
        enums = "\n".join(
            [
                declaration(api, "enum", "zmk_input_processor_axis_snap_mode"),
                declaration(api, "enum", "zmk_input_processor_runtime_write_mode"),
                declaration(events, "enum", "zmk_input_processor_inertia_stop_reason"),
            ]
        )
        types = "\n".join(
            declaration(source, "struct", name)
            for name in (
                "runtime_processor_config",
                "inertia_trigger_state",
                "runtime_processor_data",
            )
        )
        names = (
            "commit_write",
            "update_rotation_values",
            "zmk_input_processor_runtime_settings_generation",
            "retire_inline_remainder",
            "temp_layer_activation_work_handler",
            "temp_layer_deactivation_work_handler",
            "code_idx",
            "is_processor_active_for_current_layers",
            "scale_val",
            "rotation_component",
            "zmk_input_processor_runtime_rotate_xy",
            "runtime_processor_handle_event",
            "zmk_input_processor_runtime_set_rotation",
            "zmk_input_processor_runtime_set_scaling",
            "zmk_input_processor_runtime_set_temp_layer_layer",
            "zmk_input_processor_runtime_set_axis_snap",
        )
        types += "\n" + declaration(
            (ROOT / "include/zmk/pointing/input_rotation.h").read_text(),
            "struct",
            "zmk_runtime_rotation_state",
        )
        limits = (
            re.search(
                r"^#define ZMK_INPUT_PROCESSOR_RUNTIME_MAX_SCALE_DIVISOR .*$", api, re.M
            ).group()
            + "\n"
        )
        unit = (
            PREFIX
            + limits
            + enums
            + types
            + EFFECT_STUBS
            + "\n".join(function(source, name) for name in names)
            + '\n#line 1 "review-scenarios.c"\n'
            + SCENARIOS
        )
        cls.temp = tempfile.TemporaryDirectory(prefix="rip-review-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        cfile = work / "review.c"
        cfile.write_text(unit)
        cls.binaries = []
        print(
            "Production source SHA256:",
            hashlib.sha256(source.encode()).hexdigest(),
            flush=True,
        )
        for name, flags in (
            ("optimized", ["-O2"]),
            (
                "ubsan",
                ["-O1", "-g", "-fsanitize=undefined", "-fno-sanitize-recover=all"],
            ),
        ):
            binary = work / name
            command = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Wno-unused-parameter",
                "-Wno-unused-variable",
                "-Wno-unused-function",
                *flags,
                str(cfile),
                "-lm",
                "-o",
                str(binary),
            ]
            subprocess.run(command, check=True)
            cls.binaries.append((name, binary))

    def run_scenario(self, scenario):
        for name, binary in self.binaries:
            with self.subTest(variant=name):
                result = subprocess.run(
                    [str(binary), scenario],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                print(
                    f"{name}/{scenario}: exit={result.returncode}\n{result.stdout}",
                    flush=True,
                )
                self.assertEqual(result.returncode, 0, result.stdout)

    def test_gain_changes_retire_axis_remainders(self):
        self.run_scenario("settings-remainders")

    def test_activation_waits_and_rejects_stale_input(self):
        self.run_scenario("activation-delay")

    def test_scale_control(self):
        self.run_scenario("scale-control")

    def test_scale_product_keeps_direction(self):
        self.run_scenario("scale-product")

    def test_large_positive_divisor_does_not_crash(self):
        self.run_scenario("scale-divisor")

    def test_scale_remainders_and_saturation_boundaries(self):
        self.run_scenario("scale-boundary")

    def test_snap_control(self):
        self.run_scenario("snap-control")

    def test_short_snap_timeout_does_not_crash(self):
        self.run_scenario("snap-short")

    def test_full_snap_threshold_and_long_idle(self):
        self.run_scenario("snap-threshold-boundary")

    def test_rotation_preserves_a_complete_frame(self):
        self.run_scenario("rotation")

    def test_retarget_releases_previously_raised_layer(self):
        self.run_scenario("retarget-layer")


if __name__ == "__main__":
    unittest.main()
