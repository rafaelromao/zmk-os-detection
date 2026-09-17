# zmk-os-detection

Works out which operating system is on the other end of the cable -- or the
Bluetooth link -- and turns one layer on or off to match.

The layer is a *mode relative to the firmware*, not an absolute OS. A keymap
built for macOS uses the layer to mean "this host is a Linux box"; a keymap
built for Linux uses the same layer to mean the opposite. So the node does not
name a base OS: it names, for each OS that can be detected, whether the layer
should be on or off.

```dts
/ {
    os_detection {
        compatible = "zmk,os-detection";
        layer = <ALT_OS>;
#ifdef LINUX
        activate-for = "macos";
        deactivate-for = "linux";
#else
        activate-for = "linux";
        deactivate-for = "macos";
#endif
    };
};
```

An OS in neither list -- including a host that could not be identified --
leaves the layer exactly as it was. That is the whole failure mode: a guess is
never invented, so whatever set the layer last still stands.

## How it detects

Nothing here reads keystrokes or anything the host sends as HID data. It
watches how the host *reads the keyboard's own descriptors*, which differs
between operating systems in ways that are stable and cheap to count.

**Over USB**, during enumeration:

| Host | Shape of the `GET_DESCRIPTOR(STRING)` requests |
| --- | --- |
| macOS | a 2-byte probe for the length, then a re-read at that length |
| Linux | every string read straight into a 255-byte buffer, no probe |

**Over BLE**, during service discovery:

| Host | Reads |
| --- | --- |
| Linux (BlueZ) | GAP Appearance and the DIS PnP ID, as well as the HID report map |
| macOS | the HID report map; neither of the other two |

Each transport keeps its own counts and waits for them to go quiet before
committing to an answer.

## What it will get wrong

- **An Android phone reads as macOS over BLE.** It skips GAP Appearance the
  same way macOS does. Telling them apart needs a signal this module does not
  collect. If you pair a phone, teach that profile by hand.
- **Windows is not detected at all.** It is absent from `enum zmk_os`, so it
  can never be named in a list. Over USB it looks like Linux here; over BLE it
  looks like Linux too. Both are arguably the right answer for a ctrl-based
  host, but neither is a deliberate decision.
- **The BLE rule is much weaker than the USB one.** Confirm it against your
  own machines before trusting it -- see below.

## Confirming the fingerprints

Both classifiers log their raw counts at debug level every time they settle:

```
os detection: usb probe=3 full=0 other=3 bos=1/5 -> os=1
os detection: ble profile=0 report_map=1 hids_info=0 pnp=0 appearance=0 mtu=527 -> os=1
```

Turn on `CONFIG_ZMK_USB_LOGGING=y` (or your usual log backend) and plug into
each host in turn. If a count does not match the table above, the rule in
`src/os_detection_internal.h` is what to change -- it is deliberately kept
free of Zephyr headers so it can be read and edited on its own.

## Configuration

| Symbol | Default | |
| --- | --- | --- |
| `ZMK_OS_DETECTION` | auto | On when a `zmk,os-detection` node exists. Central/unibody only. |
| `ZMK_OS_DETECTION_USB` | `y` | Needs `ZMK_USB` and Zephyr's legacy USB stack. |
| `ZMK_OS_DETECTION_BLE` | `y` | Needs `ZMK_BLE`. |
| `ZMK_OS_DETECTION_USB_SETTLE_MS` | 200 | Quiet time before the USB guess is read. |
| `ZMK_OS_DETECTION_BLE_SETTLE_MS` | 1000 | Same, for BLE. Longer because discovery is slower. |

### A note on `CONFIG_USB_DEVICE_BOS`

`ZMK_OS_DETECTION_USB` selects it, because the function this module hooks only
exists when it is set. It also changes the device descriptor's `bcdUSB` from
2.00 to 2.01, so hosts start asking for a BOS descriptor -- and Zephyr's is
malformed until something registers a capability, which nothing in ZMK does.
This module registers one (see the comment in `src/os_detection_usb.c`). If
you turn USB detection off, that registration goes with it.

## Alongside zmk-persistent-layers

They compose, and are better together than either alone.
`zmk,persistent-layers` replays the layer the instant an endpoint is selected;
this module corrects it a fraction of a second later once the host has
identified itself, and the correction is recorded in turn. So a host you have
never seen is right on the first connect instead of the second, and a host
whose OS was reinstalled fixes itself.

Keep the layer listed in both nodes. Writes here are deferred past the replay
by `settle-ms` for that reason -- the two modules subscribe to the same event,
and their order is otherwise link order.

## Manual override

The layer's own toggle keys keep working. Detection writes only when a new
result arrives or the endpoint changes, so setting the layer by hand sticks
for the rest of the session. On the next reconnect detection re-asserts what
it sees.

## Credit

The descriptor-read signatures come from
[cormoran/zmk-feature-os-detection](https://github.com/cormoran/zmk-feature-os-detection)
(MIT), which verified them against real hardware. That module is more
ambitious than this one -- it names Windows, iOS and Android too, and carries
the capture notes behind every rule. Read it if you need more than two
operating systems.
