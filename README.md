# ZMK Runtime Input Processor Module

This ZMK module provides runtime configurable input processors for pointing devices. You can adjust scaling and rotation parameters dynamically through a web interface without rebuilding firmware.

## Features

- **Runtime Configuration**: Adjust input processor parameters without rebuilding firmware
- **Web Interface**: Configure settings through a browser-based UI
- **Scaling Support**: Configure speed multipliers (e.g., x2 faster, x0.5 slower)
- **Rotation Support**: Arbitrary integer angles through a module-owned virtual input source, with complete X/Y frames and fractional movement retention
- **Axis Reversing**: Invert X and/or Y axis independently to reverse input direction
- **Inertia**: Continue input after a rapid, configurable directional movement
- **Axis Snapping**: Lock scrolling to X or Y axis with threshold-based unlock
- **Temp-Layer Layer**: Automatically activate a layer when using pointing device, deactivate on key press or timeout
- **Active Layers**: Specify which layers the processor should be active on using a bitmask
- **Temporary Changes**: Hold a key to temporarily change settings (perfect for DPI toggle)
- **Persistent Settings**: Settings saved to non-volatile storage
- **Multiple Processors**: Support for multiple input processors with individual configuration

## Setup

### 1. Add dependency to your `config/west.yml`

```yaml
manifest:
  remotes:
    - name: amgskobo
      url-base: https://github.com/amgskobo
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-module-runtime-input-processor
      remote: amgskobo
      revision: codex/runtime-input-processor-review
    # Below setting required to use unofficial studio custom RPC feature
    - name: zmk
      remote: cormoran
      revision: e5c9b6915b56801193e359dd9bad4a167ce0d1b8
      import:
        file: app/west.yml
    # Persistent settings storage backend used by this module
    - name: zmk-feature-custom-settings
      remote: amgskobo
      revision: b91a8ceb0a57a0cf41b34270752d79463e2848fa
```

### 2. Enable the feature in your `config/<shield>.conf`

```conf
CONFIG_ZMK_POINTING=y

# Enable runtime input processor
CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR=y

# Enable studio custom RPC features for web UI
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC=y

# Required when the Studio RPC is enabled: "Refresh List" paces its
# per-processor notifications on ZMK's shared low-priority work queue, and
# encoding a notification through the Studio RPC core needs more than that
# thread's 768-byte default stack. A build without this raise will fault
# (MPU stack guard) the first time the web UI lists processors.
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
```

> A module cannot raise this shared thread's stack for you (a ZMK Kconfig
> `default` in your config wins over the module's), so it must live in your
> keyboard config as shown above.

`CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR` automatically selects `CONFIG_ZMK_CUSTOM_SETTINGS`
(from [zmk-feature-custom-settings](https://github.com/amgskobo/zmk-feature-custom-settings)),
which this module uses purely as a typed persistence backend for the settings described
below - `zmk-feature-custom-settings`'s own Studio RPC surface is not required and is not
enabled by this module (the RPC/web UI in this repo is unaffected and unofficial-Studio-RPC
compatible as before).

### 3. Add runtime input processor to your keymap

```dts
#include <dt-bindings/zmk/input.h>
#include <input/processors.dtsi>
#include <input/processors/runtime-input-processor.dtsi>
// The .dtsi provides default device definitions
// - mouse_runtime_input_processor
// - scroll_runtime_input_processor

/ {
    // Then use it in your input device configuration
    my_input_listener {
        // ... other config ...
        input-processors = <&mouse_runtime_input_processor>;

        scroller {
			// layers = <9>;
			input-processors = <&zip_xy_to_scroll_mapper &scroll_runtime_input_processor>;
		};
    };

    // For split keyboard, you can configure input processor in central
    split_input: split_input {
        compatible = "zmk,input-split";
    };
    split_listener: split_listener {
        compatible = "zmk,input-listener";
        status = "disabled";
        device = <&split_input>;
    };

    my_rip: my_rip {
		compatible = "zmk,input-processor-runtime";
		processor-label = "custom";
		type = <INPUT_EV_REL>;
		x-codes = <INPUT_REL_X>;
		y-codes = <INPUT_REL_Y>;
		scale-multiplier = <1>;
		scale-divisor = <1>;
		rotation-degrees = <0>;
		track-remainders;

		// Optional: Temp-layer layer default settings
		temp-layer-enabled;  // Enable temp-layer by default
		temp-layer = <1>;  // Default to layer 1
		temp-layer-activation-delay-ms = <100>;  // 100ms activation delay
		temp-layer-deactivation-delay-ms = <500>;  // 500ms deactivation delay

		// Optional: Active layers configuration
		// Bitmask of layers where processor should be active (0 = all layers)
		// Each bit represents a layer: bit 0 = layer 0, bit 1 = layer 1, etc.
		// Example: active-layers = <0x00000003> activates on layers 0 and 1 only
		active-layers = <0>;  // Default: active on all layers

		// Optional: Performance optimization
		temp-layer-transparent-behavior = <&trans>;
		temp-layer-kp-behavior = <&kp>;
		temp-layer-keep-keycodes = <LEFT_CONTROL LEFT_SHIFT LEFT_ALT LEFT_GUI RIGHT_CONTROL RIGHT_SHIFT RIGHT_ALT RIGHT_GUI>;

		#input-processor-cells = <0>;
	};
};

// <central>.overlay
&split_listener {
    status = "okay";
    input-processors = <&my_rip>;
};
```

### 4. Route arbitrary-angle rotation through a virtual input source

Rotation can turn a pure X input into both X and Y outputs. The ordinary ZMK
input-processor interface returns one event, so rotation uses a virtual source
provided entirely by this module. ZMK itself needs no patch. Scaling-only inline
configurations continue to work; setting a nonzero angle without a virtual source returns `-ENOTSUP` before
changing settings. An old persisted nonzero angle on an inline source is left
unapplied, with one diagnostic, so input continues until the DTS routing is
migrated.

For the split input and processor above, add a virtual source and change the
listener's device. Keep the runtime processor in its processor list:

```dts
/ {
    rotated_input: rotated_input {
        compatible = "zmk,runtime-input-rotation";
        device = <&split_input>;
        processor = <&my_rip>;
    };
};

&split_listener {
    device = <&rotated_input>;
    input-processors = <&my_rip>;
};
```

`device` can also reference a local pointing device. Attach its listener only
to the virtual source to avoid processing the physical input a second time.
Other listener processors run after the rotation; retain their order after
`my_rip`. Any processor that previously preceded `my_rip` now receives rotated
coordinates, so review that ordering when migrating a longer chain.

The virtual source supports one relative X/Y code pair per runtime processor.
It collects events through the source's `sync` boundary, emits both rotated axes,
and preserves other events and one final sync. Pure X, pure Y, reversed axis
order and independent devices are supported. Small fractional movements carry
into later frames. Changes in the effective rotation retire the old fraction;
output saturates at the signed 16-bit pointing boundary. Inversion, snapping,
scaling and the existing remaining runtime processing follow rotation.

Complete frames wait on a dedicated output worker, allowing it to wait for
Zephyr input-queue capacity without blocking the input callback. The default
limits are 16 source events per frame and 8 queued frames, configurable with
`CONFIG_ZMK_RUNTIME_INPUT_ROTATION_FRAME_EVENTS` and
`CONFIG_ZMK_RUNTIME_INPUT_ROTATION_QUEUE_FRAMES`. A frame that exceeds either
bound is dropped as a whole and logged; the next valid frame starts cleanly.
These finite buffers do not guarantee delivery under sustained overload.
Initialization must follow the source device; use
`CONFIG_ZMK_RUNTIME_INPUT_ROTATION_INIT_PRIORITY` if its default 95 is too early
for a custom driver.

## Usage

### Ordered complete-frame pipelines

For a chain with more than one runtime processor, use
`compatible = "zmk,runtime-input-pipeline"`, with `input-device` pointing to
the sensor and `input-processors` listing the ordered stages. Each runtime
stage rotates the current X/Y pair before its remaining processing. Later
stages therefore see mapped scroll codes and transformed coordinates, rather
than the original sensor frame. Fractional rotation and scaling state belong
to each stage and source. Attach exactly one final `zmk,input-listener` to the
pipeline; shared Inertia belongs in that final listener so HID emission stays
on Zephyr's input thread. The final listener's index is passed to processors.

The pipeline has a bounded frame queue and a dedicated output worker. An
oversized source frame or a full queue drops that complete frame. Dropped
frames retire fractional state before the next accepted generation. Invalid
processor results or output expansion beyond the configured limit also drop
the complete output frame. `CONFIG_ZMK_RUNTIME_INPUT_PIPELINE_*` controls frame
and output capacity, queue length, worker stack and initialization priority.
Finite queues do not guarantee lossless delivery during sustained overload.

The `tests/pipeline` native case covers two runtime rotations separated by a
core transform, a downstream scaler, fractional frames, buttons, scroll,
oversized frames and queue-full recovery. Repeat the built binary with
`bash tests/pipeline/repeat.sh NATIVE_EXECUTABLE FRESH_OUTPUT` for three
independent process starts, retaining each ordered log and the binary hash.

### Web Interface

1. Build and flash your firmware with the runtime input processor enabled
2. Connect to your keyboard via USB (Web Serial) or Bluetooth (Web
   Bluetooth) using a Chromium-based browser (Chrome, Edge, ...) over HTTPS
   or localhost
3. The web interface will automatically detect available input processors
4. Adjust scaling and rotation parameters
5. Changes are applied immediately without restarting

### Storage modes (memory vs. persistent)

Each setting write chooses where the value is stored, mirroring
`zmk-feature-custom-settings`:

- **Persist to flash** (default): the value is applied and saved to
  non-volatile storage, so it survives a reboot. This is the historical
  behavior — clients that do not select a mode always persist.
- **Memory only**: the value is applied and kept as the current baseline in
  RAM, but is _not_ written to flash. It is lost on reboot unless you save it.

The web UI exposes this as the **Storage** selector next to "Apply Settings".
Three whole-keyboard operations sit next to "Refresh List":

- **Save All** — flush every processor's current settings to flash (persist any
  "memory only" changes).
- **Discard All** — drop unsaved (memory-only) changes and reload the last saved
  values from flash; processors with nothing saved return to their devicetree
  defaults.
- **Reset All** — reset every processor to its devicetree defaults and persist
  them.

The equivalent firmware API is
`zmk_input_processor_runtime_save_all()` / `_discard_all()` / `_reset_all()`,
and the setters take a `zmk_input_processor_runtime_write_mode`
(`PERSIST` / `MEMORY` / `TEMPORARY`).

### Configuration Parameters

- **Scaling Multiplier/Divisor**: Controls pointer speed
  - Example: `2/1` = 2x faster, `1/2` = 0.5x slower
  - Values are applied as: `output = input * multiplier / divisor`
  - Remainders are tracked for precise scaling
  - The divisor is limited to `1..32768` so fractional remainders fit the
    listener's signed 16-bit storage. Larger divisors are rejected before
    changing the live or saved configuration. Scaled output saturates to the
    signed 16-bit range.

- **Inertia**: Starts continuous input after rapid same-direction movement
  - `inertia-enabled` enables the feature per processor (off by default;
    enable it on the scroll processor or in Studio)
  - `inertia-threshold` (default `12`) is the accumulated amount required
    after scaling within `inertia-window-ms`
  - `inertia-window-ms` (default `200`) controls input measurement, while
    `inertia-interval-ms` (default `20`) controls output cadence
  - `inertia-normal-max-output` (default `0`, unlimited) caps each generated
    output after scaling in the normal stage. Fast mode ignores this cap.
  - `inertia-fast-threshold` (default `20`) is a second threshold in
    scaled input counts over the same window. Crossing it enables fast output
    for the rest of that inertia run.
  - `inertia-fast-output-percent` (default `200`) multiplies the retained
    target speed in fast mode before subtracting physical input. `200` targets
    twice the retained speed for the combined physical and inertia output.
  - The movement that triggers entry sets the initial speed. Each interval
    emits only the gap between the retained peak and the physical input rate
    in the latest measurement bucket. In window
    units this is `max(retained_raw_amount - latest_bucket_raw_amount *
    window_ms / bucket_ms, 0) * interval_ms / window_ms`, then applies normal scale
    (in fast mode, multiply `retained_raw_amount` by the fast percentage before
    subtracting physical input). Fractional output carries into later
    intervals so slow speeds still move. A latest bucket older than its
    duration counts as zero physical input
  - Reverse movement totaling `clamp(threshold / 2, reverse_min, reverse_max)`
    within one measurement window exits inertia; isolated reverse counts are
    ignored. `reverse_min` and `reverse_max` default to `3` and `12` in Kconfig
  - `inertia-decay-percent` (default `8`) multiplies the retained speed by
    `(100 - decay) / 100` after each output interval without new input. This
    gives an exponential tail; `0` holds speed and `100` stops after one more
    interval
  - Reports are distributed over elapsed time in Q16 rolling buckets. Inertia
    ends when its remaining tail cannot produce another scaled output count.
  - The Studio Web tuning area repeats in both directions and can lock either
    axis. Its optional activity display enables memory-only Studio RPC
    notifications for this processor, defaults to off, and changes the area
    background while inertia is active.
  - Kconfig controls rolling bucket count, window slices, and reverse exit
    bounds. See
    `Kconfig` for the complete `ZMK_RUNTIME_INPUT_PROCESSOR_INERTIA_*` options.
  - Speed is stored with fractional precision, so the exponential tail keeps
    its shape at low speeds

### Example Configurations

**2x Speed:**

```
scale-multiplier = 2
scale-divisor = 1
```

**Half Speed:**

```
scale-multiplier = 1
scale-divisor = 2
```

### Temporary Configuration via Behavior

You can temporarily change input processor settings while holding a key, useful for temporary DPI changes:

```dts
#include <behaviors/runtime-input-processor.dtsi>
/ {


    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <
                // Use &temp_fast in your keymap
                &hdpi  // Hold this key for 1.5x mouse speed
                &ldpi  // Hold this key for 0.5x mouse speed
                &hscr  // High scroll speed
                &lscr  // Low scroll speed
                // ... other keys
            >;
        };
    };
};
// Customization
&hdpi {
    scale-multiplier = <3>;
	scale-divisor = <2>;
}
```

When you press and hold a key with the temporary config behavior:

1. Each held behavior/position owns only its requested scaling and nonzero rotation.
2. On a behavior press, the latest held owner wins independently for each field. A zero rotation leaves the current angle unchanged.
3. Releasing a key restores the next held owner for those fields, or the persistent baseline. Other held settings stay active.

Temporary DPI and axis snap share a bounded pool, configured with
`CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_TEMP_OVERRIDES` (default 8). An unsupported
angle or a full pool refuses the press before any setting changes. Releasing a
refused press is harmless. Direct runtime setting updates still apply immediately;
release uses the current persistent baseline when no other behavior owns a field.

### Temp-Layer Layer

The temp-layer layer feature automatically activates a specified layer when you use your pointing device (trackpad, trackball, etc.) and deactivates it after a period of inactivity or when you press a key.

**Configuration via Device Tree (Optional):**

You can configure default temp-layer settings in your device tree:

```dts
my_pointer_processor: my_pointer_processor {
    compatible = "zmk,input-processor-runtime";
    processor-label = "trackpad";
    // ... basic config ...

    // Enable temp-layer with default settings
    temp-layer-enabled;  // Boolean property - presence enables it
    temp-layer = <1>;  // Target layer (default: 0)
    temp-layer-activation-delay-ms = <100>;  // Activation delay (default: 100)
    temp-layer-deactivation-delay-ms = <500>;  // Deactivation delay (default: 500)
};
```

**Configuration via Web UI:**

Temp-layer layer settings can also be configured through the web interface:

- **Enable/Disable**: Toggle the temp-layer layer feature
- **Target Layer**: The layer number to activate (e.g., layer 1, 2, etc.)
- **Activation Delay**: Time to wait after input starts before activating the layer (milliseconds)
- **Deactivation Delay**: Time to wait after input stops before deactivating the layer (milliseconds)

**Behavior:**

- When you move the pointing device, the layer activates after the activation delay
- The layer stays active while you continue using the pointing device
- When you press any keyboard key, the layer deactivates immediately (unless it's a modifier or the key is on the temp-layer layer)
- If you stop moving the pointing device, the layer deactivates after the deactivation delay
- If a key press occurs before the activation delay expires, the layer won't activate

**Keep Temp-Layer Layer Active:**

You can create a behavior to prevent the temp-layer layer from deactivating while holding a key:

```dts
#include <behaviors/runtime-input-processor.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <
                // temp-layer keep active
                &amka  // Hold this key to keep temp-layer layer active
                // ... other keys
            >;
        };
    };
};
```

When holding the keep-active behavior key, the temp-layer layer will not deactivate when you press other keys or after the timeout period.

### Active Layers

The active layers feature allows you to specify which layers the input processor should be active on. This is useful when you want the processor to only apply transformations (scaling, rotation) on specific layers. Active inertia stops when none of the selected layers remains active.

**Configuration via Device Tree:**

```dts
my_pointer_processor: my_pointer_processor {
    compatible = "zmk,input-processor-runtime";
    processor-label = "trackpad";
    // ... basic config ...

    // Active on layers 0 and 1 only (bitmask: 0x00000003)
    active-layers = <0x00000003>;
};
```

**Configuration via Web UI:**

The web interface provides two ways to configure active layers:

1. **Hex Input**: Enter the layer bitmask directly as a hexadecimal value (e.g., `0x00000003` for layers 0 and 1)
2. **Layer Checkboxes**: Click individual layer checkboxes to build the bitmask visually

**Bitmask Format:**

- Each bit represents a layer: bit 0 = layer 0, bit 1 = layer 1, etc.
- `0x00000000` (default): Processor is active on all layers
- `0x00000001`: Active only on layer 0
- `0x00000003`: Active on layers 0 and 1
- `0x0000000F`: Active on layers 0-3
- `0xFFFFFFFF`: Active on all 32 layers

**Behavior:**

- If at least one of the specified layers is active, the processor works normally
- If none of the specified layers are active, the processor skips processing (no transformation applied)
- This allows you to have different pointer speeds or behaviors on different layers

### Axis Snapping

The axis snapping feature locks scrolling to a specific axis (X or Y), preventing unwanted diagonal scrolling. Movement on the locked axis is suppressed unless it exceeds a configurable threshold within a timeout window.

**Configuration via Device Tree:**

```dts
#include <dt-bindings/zmk/runtime_input_processor.h>

scroll_runtime_input_processor: scroll_runtime_input_processor {
    compatible = "zmk,input-processor-runtime";
    processor-label = "scroll";
    // ... basic config ...

    // Lock to Y axis for vertical scrolling only
    axis-snap-mode = <AXIS_SNAP_MODE_Y>;
    axis-snap-threshold = <100>;  // Unlock if cross-axis movement > 100
    axis-snap-timeout-ms = <1000>;  // Decay period
};
```

Available axis snap mode constants:

- `AXIS_SNAP_MODE_NONE` (0): No snapping
- `AXIS_SNAP_MODE_X` (1): Snap to X axis (horizontal only)
- `AXIS_SNAP_MODE_Y` (2): Snap to Y axis (vertical only)

**Configuration via Web UI:**

The web interface provides controls for axis snapping:

1. **Snap Mode**: Select no-snap, snap to X axis, or snap to Y axis
2. **Unlock Threshold**: Set how much cross-axis movement is needed to unlock the snap
3. **Timeout Window**: Set the time window for checking the threshold

**Snap Modes:**

- **No Snap (0)**: Normal operation, no axis locking
- **Snap to X Axis (1)**: Only horizontal movement, vertical suppressed unless threshold exceeded
- **Snap to Y Axis (2)**: Only vertical movement, horizontal suppressed unless threshold exceeded

**Behavior:**

- When snap is enabled, movement on the locked axis proceeds normally
- Movement on the cross-axis is accumulated but suppressed (value set to 0)
- The accumulator decays over time (threshold amount over the timeout period)
- If accumulated cross-axis movement exceeds the threshold, the snap lock is released
- If no cross-axis movement occurs, the accumulator decays to zero after the timeout period

**Example Use Cases:**

- **Wheel Scroll**: Set `axis-snap-mode = <2>` on scroll processor to ensure wheel only scrolls vertically
- **Text Selection**: Use the temporary snap behavior to lock Y-axis while selecting text with mouse

**Temporary Snap Behavior:**

You can temporarily enable axis snapping while holding a key using binding parameters:

```dts
#include <behaviors/runtime-input-processor.dtsi>
#include <dt-bindings/zmk/runtime_input_processor.h>

/ {
    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <
                &ysnap AXIS_SNAP_MODE_Y 100   // Hold for Y-axis snap (threshold=100)
                &xsnap AXIS_SNAP_MODE_X 50    // Hold for X-axis snap (threshold=50)
                // ... other keys
            >;
        };
    };
};
```

The behavior takes two parameters:

- **param1**: Snap mode (use constants: `AXIS_SNAP_MODE_NONE`, `AXIS_SNAP_MODE_X`, `AXIS_SNAP_MODE_Y`)
- **param2**: Threshold for unlocking snap

You can also configure the timeout in the behavior definition:

```dts
&ysnap {
    timeout-ms = <500>;  // Custom timeout (default: 1000ms)
};
```

When you press and hold the snap behavior key:

1. The pressed behavior/position owns the requested mode, threshold and timeout.
2. It remains independent of a held DPI key; overlapping snap owners use latest-press order.
3. Release restores the next held snap owner or the persistent baseline without changing DPI.

## Development Guide

### Setup

Run dependency preparation, tests and builds inside Docker. The CI image is
pinned by digest in `.github/workflows/zmk-module.yml`; use the same image for
local verification. Test dependencies select the exact DYA ZMK, Zephyr and
`zmk-west-commands` revisions in `west/west-dependency/west-test-dependency.yml`.
The Custom Settings fork commit is shared with the production manifest.
Prepare those revisions explicitly before an ordinary test run. Local dirty
sources are a separate snapshot from the published review-branch commit.

There are two west workspace layout options.

#### Option1: Download dependencies in parent directory

This option is west's standard way. Choose this option if you want to re-use dependent projects in other zephyr module development.

```bash
mkdir west-workspace
cd west-workspace # this directory becomes west workspace root (topdir)
git clone <this repository>
cd <cloned directory>
west init -l . --mf west/west-test.yml
west update --narrow
west zephyr-export
```

The directory structure becomes like below:

```
west-workspace
  - .west/config
  - build : build output directory
  - <this repository>
  # other dependencies
  - zmk
  - zephyr
  - ...
  # You can develop other zephyr modules in this workspace
  - your-other-repo
```

Select the owner manifest explicitly in the existing workspace; do not update shared dependency checkouts implicitly.

#### Option2: Download dependencies in ./dependencies (Enabled in dev-container)

Choose this option if you want to download dependencies under this directory (like node_modules in npm). This option is useful for specifying cache target in CI. The layout is relatively easy to recognize if you want to isolate dependencies.

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-standalone.yml
# If you use dev container, start from below commands. Above commands are executed
# automatically.
west update --narrow
west zephyr-export
```

The directory structure becomes like below:

```
<this repository>
  - .west/config
  - build : build output directory
  - dependencies
    - zmk
    - zephyr
    - ...
```

### Dev container

Dev container is configured for setup option2. The container creates below volumes to re-use resources among containers.

- zmk-dependencies: dependencies dir for setup option2
- zmk-build: build output directory
- zmk-root-user: /root, the same to ZMK's official dev container

### Web UI

Please refer [./web/README.md](./web/README.md).

## Test

The `rotation` native case runs the actual virtual source,
input queues, listener and a downstream scaler with a recording HID sink. They
cover arbitrary angles, current-frame pairing, two sources, button/scroll order,
one final sync, oversized frames and queue-full recovery with Zephyr's threaded input mode, which the selected ZMK core requires. GPIO/radio/physical mouse delivery remains a separate hardware check. Reuse a built
native binary for three identical process starts with
`bash tests/rotation/repeat.sh /path/to/zmk.exe /absolute/new/output`; the entry
retains its binary hash, ordered logs and all required snapshot markers.

**ZMK firmware test**

`./tests` directory contains test config for posix to confirm module functionality and config for xiao board to confirm build works.

Tests can be executed by below command:

```bash
# Run all test case and verify results
OWNER_TEST_OUTPUT="$PWD/build/owner-tests" python3 -m unittest -v
```

Choose a fresh output directory for every run. The canonical entry runs the
numerical/UBSan controls, all three firmware variants, every native fixture,
and the owner pipeline/rotation repeat scripts. Native binaries are built once
and run three times in separate initial-state directories; logs and hashes are
retained. The Studio fixture uses real nanopb/custom-subsystem dispatch to
check full-width IDs and field bounds, Get response presence/XY flags, and
PERSIST followed by MEMORY before a debounce flush. A memory-only edit must
not change the already captured durable snapshot. Gain/settings changes also
retire fractional remainders independently for each source/axis.

The Web fixture retains edited gain/rotation when activity notifications arrive
before or after an activity-toggle response. Review-branch pushes run Web
checks/builds; Pages deployment is limited to `main`.

If you want to execute west command manually, run below. (for zmk-build, the result is not verified.)

```
# Build test firmware for xiao
# `-m tests/zmk-config .` means tests/zmk-config and this repo are added as additional zephyr module
west zmk-build tests/zmk-config/config -m tests/zmk-config .

# Run zmk test cases
# -m . is required to add this module to build
west zmk-test tests -m .
```

**Web UI test**

The `./web` directory includes Jest tests. See [./web/README.md](./web/README.md#testing) for more details.

```bash
cd web
npm test
```

### Hardware-free Renode testing

CI also boots this module's firmware in the [Renode](https://renode.io/)
emulator (no physical board needed) and exercises it functionally: the real
ZMK boot banner, a core Studio RPC `GetDeviceInfo` round trip, and this
module's own custom Studio RPC subsystem (`cormoran_rip`). This is a *step*
in the `Build` job in `.github/workflows/zmk-module.yml` (not a separate
job) -- it tests the `renode_smoke_test` artifact `python3 -m unittest -v`
already built (see `tests/zmk-config/build.yaml`) using the pinned
[`zmk-west-commands` action](https://github.com/cormoran/zmk-west-commands/tree/536260056f362edb2092905f2a75317bc8cb9bc2/.github/actions/zmk-renode-test).
The action boots an existing ELF and runs the module cases; it does not build.
The module supplies its `renode-studio-uart` snippet/transport for this fixture.
The helper's generic BLE smoke is boot liveness without a host ELF; the owner
suite separately exercises its UART custom RPC transport. Neither establishes
physical USB/BLE delivery.

Run the owner build first in the prepared container, then select its exact ELF:

```bash
west zmk-renode-test tests/renode \
  --elf "$PWD/build/owner-tests/firmware/renode_smoke_test/zephyr/zmk.elf"
```

The pinned helper supplies `renode_harness` on `PYTHONPATH`. Direct execution
resolves that same selected helper through `west list`, without a sibling
workspace assumption.

`tests/renode/renode_test.py`'s own module docstring documents a known
Renode-only limitation (custom-subsystem responses carrying real data time
out past a few tens of bytes) and how each test in that file relates to it.

## Publishing Web UI

### GitHub Pages (Production)

Github actions are pre-configured to publish web UI to github pages.

1. Visit Settings>Pages
1. Set source as "Github Actions"
1. Visit Actions>"Test and Build Web UI"
1. Click "Run workflow"

Then, the Web UI will be available in
`https://<your github account>.github.io/<repository name>/` like https://cormoran.github.io/zmk-module-template-with-custom-studio-rpc.

### Cloudflare Workers (Pull Request Preview)

For previewing web UI changes in pull requests:

1. Create a Cloudflare Workers project and configure secrets:
   - `CLOUDFLARE_API_TOKEN`: API token with Cloudflare Pages edit permission
   - `CLOUDFLARE_ACCOUNT_ID`: Your Cloudflare account ID
   - (Optional) `CLOUDFLARE_PROJECT_NAME`: Project name (defaults to `zmk-module-web-ui`)
   - Enable "Preview URLs" feature in cloudflare the project

2. Optionally set up an `approval-required` environment in github repository settings requiring approval from repository owners

3. Create a pull request with web UI changes - the preview deployment will trigger automatically and wait for approval

## Sync changes in template

By running `Actions > Sync Changes in Template > Run workflow`, pull request is created to your repository to reflect changes in template repository.

If the template contains changes in `.github/workflows/*`, registering your github personal access token as `GH_TOKEN` to repository secret is required.
The fine-grained token requires write to contents, pull-requests and workflows.
Please see detail in [actions-template-sync](https://github.com/AndreasAugustin/actions-template-sync).

## More Info

For more info on modules, you can read through through the
[Zephyr modules page](https://docs.zephyrproject.org/3.5.0/develop/modules.html)
and [ZMK's page on using modules](https://zmk.dev/docs/features/modules).
[Zephyr's west manifest page](https://docs.zephyrproject.org/3.5.0/develop/west/manifest.html#west-manifests)
may also be of use.

## Acknowledgements

Special thanks to the authors and maintainers of these projects for ideas that
informed the input and inertia implementations in this module:

- [ShiniNet/zmk-driver-iqs9151](https://github.com/ShiniNet/zmk-driver-iqs9151)
- [sekigon-gonnoc/zmk-driver-iqs7211e](https://github.com/sekigon-gonnoc/zmk-driver-iqs7211e)
- [razilyis/zmk-pmw3610-driver](https://github.com/razilyis/zmk-pmw3610-driver/tree/Dev-v0.4_inertial-scroll)
- [amgskobo/zmk-input-inertia](https://github.com/amgskobo/zmk-input-inertia)
