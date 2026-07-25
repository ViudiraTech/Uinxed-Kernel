# PS/2 Keyboard and Mouse Driver Design

## Goal

Split the existing combined PS/2 keyboard driver into focused controller, keyboard, and mouse components. Add a production-quality PS/2 mouse driver for standard, IntelliMouse wheel, and IntelliMouse Explorer five-button devices. Expose keyboard and mouse through correctly registered evdev, devtmpfs, and sysfs objects.

## Scope

Included:

- i8042 controller initialisation, command transport, port selection, and IRQ dispatch.
- Existing PS/2 keyboard behaviour, including TTY input, preserved in a dedicated keyboard driver.
- PS/2 mouse reset, command acknowledgement, retries, device ID detection, IntelliMouse and Explorer protocol negotiation, IRQ12 packet parsing, and recovery from malformed packets.
- Generic evdev-backed `/dev/input/eventX` nodes with an independent evdev client for every open file.
- Sysfs registration for input event devices after the device model is ready.
- Tests for protocol parsing, negotiation, event allocation, independent open clients, and device-node registration.

Excluded:

- Synaptics, ALPS, or other vendor-specific touchpad protocols.
- A new general-purpose Linux serio bus subsystem.

## Architecture

### Controller, keyboard, and mouse responsibilities

`drivers/char/ps2.c` becomes the i8042 controller implementation. It owns status polling with bounded timeouts, controller commands, configuration-byte access, second-port selection, controller/port tests, and source-aware IRQ dispatch. It contains no keyboard or mouse event interpretation.

`drivers/char/keyboard.c` owns keyboard reset/configuration, IRQ1 byte handling, its evdev descriptor, and the existing TTY scancode handoff. It emits `EV_MSC/MSC_SCAN`, `EV_KEY`, and `EV_SYN/SYN_REPORT`.

`drivers/char/mouse.c` owns mouse reset/configuration, device ID negotiation, packet framing, and the mouse evdev descriptor. It emits `EV_REL/REL_X`, `EV_REL/REL_Y`, `EV_REL/REL_WHEEL` when available, `EV_KEY` for left/right/middle/side/extra buttons, and one `EV_SYN/SYN_REPORT` per completed packet.

The shared header exposes controller transport and clearly named keyboard/mouse lifecycle functions. Keyboard and mouse never send data directly to the other port.

### Mouse protocols

The driver begins with a PS/2 reset and validates `ACK` followed by self-test completion. It reads the baseline device ID, then attempts the established sampling-rate sequence for IntelliMouse wheel mode and, after success, Explorer five-button mode. A failed command, timeout, unexpected response, or unsupported ID falls back safely to the best confirmed protocol without disabling the keyboard or halting boot.

Packet sizes are selected from the confirmed ID:

- Standard mouse: three bytes, left/right/middle, X and Y.
- IntelliMouse: four bytes, plus signed wheel delta.
- Explorer: four bytes, plus signed wheel delta and side/extra buttons.

The IRQ12 parser requires the first-byte synchronisation bit, keeps incomplete packets across interrupts, drops overflowed movement packets, and resets framing after malformed input. It negates the hardware Y value before reporting it so positive `REL_Y` means upward movement under the project’s evdev convention.

### evdev and devtmpfs

`evdev_register()` remains the source of dynamically assigned event minors. A generic evdev-to-devtmpfs bridge creates `/dev/input/eventX` for each registered device using the Linux input major number and `EVDEV_MINOR_BASE + X` minor number. The PS/2 keyboard and mouse do not hardcode event node paths or allocate their own device nodes.

The devtmpfs tmpfs adapter delegates the device-specific open callback and stores its returned private data. Its release, read, write, poll, and ioctl paths forward that private data to evdev. Thus every open `/dev/input/eventX` file gets a separate `evdev_client_t`; reading one file cannot consume another file’s events. Existing global keyboard queue wrappers and the hardcoded `/dev/input/event0` creation are removed.

### sysfs

PS/2 input devices are registered only after `sysfs_init()` and `device_model_init()` complete. The input registration creates the `input` class once and creates an `eventX` device for each evdev device with the matching device number and pointer to the input descriptor. It exposes read-only identity and capability information sufficient to identify the keyboard and mouse and to associate an event device with its input source.

The controller and input devices initialise early enough to receive hardware interrupts, but their sysfs and devtmpfs publication happen in late registration hooks once those subsystems exist. This keeps boot ordering correct while leaving event numbers determined by registration order.

## Boot Order

1. `init_ps2()` initialises evdev once, probes the controller and ports, installs IRQ handlers, and initialises the keyboard and available mouse.
2. VFS, sysfs, and the device model initialise.
3. The PS/2 input late-registration hook registers the input class and its keyboard/mouse sysfs devices.
4. `devtmpfs_init()` asks evdev to publish every registered event device as `/dev/input/eventX`.

This produces keyboard `event0` and mouse `event1` in the normal boot path, without treating either number as part of the PS/2 driver API.

## Failure Handling

- Controller or port self-test failures are logged; only the failed port is disabled.
- Second-port absence disables mouse initialisation but leaves the keyboard active.
- Every controller and device command has a bounded wait and a limited retry count.
- Mouse reset or negotiation failures retain the last verified mouse protocol when possible, otherwise leave mouse unavailable without blocking the system.
- Duplicate devtmpfs/sysfs registration returns a controlled error and does not corrupt the evdev table.

## Verification

- Unit-test packet decoding for standard, wheel, and Explorer packets, including signed movement, all buttons, wheel direction, overflow, loss of framing, and packet continuation across IRQ bytes.
- Unit-test command/ID negotiation with a scripted PS/2 transport, including retries and fallback.
- Unit-test generic evdev event-number assignment and devtmpfs node metadata.
- Unit-test two independently opened event nodes to prove per-open event queues are not shared.
- Unit-test input sysfs registration and representative identity/capability attributes.
- Build the kernel and boot it in QEMU, confirming both `/dev/input/event0` and `/dev/input/event1`, their sysfs entries, IRQ12 mouse events, wheel events, and side-button events when emulated hardware supports them.
