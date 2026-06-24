# Advanced Wireless MacroPad System (ESP32-S3)

A dual-node wireless macro keyboard system built on the ESP32-S3 microcontroller ecosystem. The project was developed to resolve the latency, memory degradation, and connectivity limitations frequently encountered in open-source human interface devices. The system pairs a primary **transmitter node**, which serves as the physical macro pad and configuration host, with a dedicated **receiver node** that acts as a native USB HID gateway. The two devices communicate over the low-latency ESP-NOW protocol, and the entire ecosystem is designed to be configured without writing a single line of code or editing any files by hand.

---

## Table of Contents

- [System Overview](#system-overview)
- [Key Features](#key-features)
- [Transmitter Node: Architecture and Memory Decisions](#transmitter-node-architecture-and-memory-decisions)
- [Receiver Node and HID Execution Logic](#receiver-node-and-hid-execution-logic)
- [Zero-Configuration Ecosystem](#zero-configuration-ecosystem)
- [Macro Syntax Reference](#macro-syntax-reference)
- [Repository and Layout Structure](#repository-and-layout-structure)
- [Hardware Requirements](#hardware-requirements)
- [Deployment and Compilation Protocols](#deployment-and-compilation-protocols)
- [First-Time Pairing](#first-time-pairing)
- [Author and License](#author-and-license)

---

## System Overview

The system is split into two cooperating firmware images that share a common wireless protocol and a unified message structure:

| Node | Role | Primary Responsibilities |
|------|------|--------------------------|
| Transmitter | Macro pad interface and configuration host | Reads 16 physical switches, hosts the asynchronous web portal, manages layered macros, and broadcasts macro and control packets over ESP-NOW. |
| Receiver | Native USB HID gateway | Listens for ESP-NOW packets, translates them into USB keyboard, mouse, and consumer-control events, and reports its network identity back to the transmitter. |

Both nodes are written as monolithic C++ sketches with no external file dependencies at runtime; all frontend assets are embedded directly in flash.

---

## Key Features

- **Native USB HID output** for keyboard, mouse, and consumer-control (media) events, executed by the receiver with near-zero perceived latency.
- **Heap-fragmentation-free architecture** using static multidimensional character arrays and reentrant parsing (`strtok_r`) instead of dynamic string allocation.
- **Fully asynchronous web portal** built on `ESPAsyncWebServer` and `AsyncTCP`, served entirely from `PROGMEM`, so configuration never blocks the high-priority input loop.
- **Up to 10 independent hardware layers** (160 total macro slots) persisted in non-volatile storage, with instantaneous in-page layer switching that requires no HTTP reload.
- **Autonomous Wi-Fi state machine** that prioritizes the home network, disables its own access point once connected, and instantly restores the access point and captive portal on signal loss.
- **Zero-configuration pairing**: Wi-Fi credentials sync automatically from the transmitter to the receiver, and the receiver reports its acquired IP address back to the transmitter.
- **Dual Over-the-Air (OTA) updates** that allow a user to flash either node from a single browser interface, including a cross-origin path to update the receiver remotely.
- **Strict power management** with automatic CPU frequency scaling during idle periods and an optional wired-mode override.
- **Regional keyboard layout translation** for US, TR, FR, DE, and JP layouts.

---

## Transmitter Node: Architecture and Memory Decisions

The primary focus during the development of the transmitter firmware was long-term memory stability and strict power management. To ensure continuous operation without risk of kernel panics or performance degradation over extended uptimes, dynamic string allocations were entirely avoided in the core codebase. Instead, the firmware handles all data processing through static multidimensional character arrays and reentrant parsing functions such as `strtok_r`. This deliberate architectural choice prevents heap fragmentation and ensures a predictable, bounded memory footprint regardless of uptime.

Configuration management is handled asynchronously through an embedded captive web portal powered by the `ESPAsyncWebServer` library. The underlying frontend assets, including HTML, CSS, and JavaScript, are stored in continuous flash memory using `PROGMEM` attributes. Operating the web server on a separate execution context guarantees that users can modify macro configurations on the fly without introducing any blocking delays or jitter into the high-priority physical hardware interrupt loop. The non-volatile storage partitioning is scaled to securely maintain up to 10 independent hardware layers, representing 160 macro slots, with instantaneous state transitions that do not require an HTTP reload.

Radio frequency state control is managed via an autonomous state machine designed to minimize wireless interference and reduce overall power consumption. The device prioritizes authentication with a local station network and automatically deactivates its internal Soft-AP once a secure connection is established. In the event of a signal drop or credential mismatch, the state machine restores the access point and captive portal instantly, ensuring the configuration interface remains reachable at all times. The node also includes a secure multipart upload handler to support Over-the-Air firmware updates directly through the asynchronous web server.

---

## Receiver Node and HID Execution Logic

The receiver node acts as a dedicated hardware bridge between the proprietary 2.4 GHz ESP-NOW wireless packets and the host operating system. The receiver listens for `struct_message` payloads originating from the transmitter's hardware MAC address. Upon receiving a valid packet, the device leverages the native hardware USB capabilities of the ESP32-S3 to translate the data into standard USB HID events with near-zero latency.

To preserve real-time stability, all decoding and HID emission is deferred out of the wireless interrupt context and processed in the main execution loop, ensuring that no USB or networking calls ever run inside the radio task. This setup allows the system to execute complex keystrokes, precise mouse actions, and consumer-control sequences natively across different operating systems without requiring any specialized software, drivers, or background services on the host computer.

---

## Zero-Configuration Ecosystem

The system is designed so that a user never has to manually configure the receiver. The pairing and provisioning flow is fully automated:

1. **Automatic Wi-Fi synchronization.** When the user saves Wi-Fi credentials in the transmitter's web interface, the transmitter broadcasts a dedicated ESP-NOW control packet to the receiver, which persists the credentials and joins the network automatically.
2. **IP handshake.** Once the receiver establishes a station connection, it transmits its acquired IP address back to the transmitter over ESP-NOW. The transmitter stores this address and exposes it in its status interface.
3. **Remote firmware updates.** Using the reported IP address, the transmitter's web interface can flash the receiver remotely. The receiver's update endpoint returns the appropriate cross-origin headers so the browser permits the request, allowing both nodes to be maintained from one page.
4. **Self-learning peer registration.** The receiver learns the transmitter's MAC address from the first packet it receives and registers the peer automatically, while a manual pairing fallback remains available on the receiver's own minimal web page.

---

## Macro Syntax Reference

Macros are defined as comma-separated command sequences. The parser supports literal characters, named keys, modifier combinations, timed delays, mouse actions, consumer-control events, and runtime layer switching.

| Command form | Example | Result |
|--------------|---------|--------|
| Literal text | `H,e,l,l,o` | Types the characters in order |
| Modifier combination | `CTRL+C` | Copies the current selection |
| Sequence with delay | `WIN,DELAY:500,C,A,L,C,ENTER` | Opens the calculator on Windows |
| Mouse action | `MOUSE_LEFT` | Issues a left mouse click |
| Consumer control | `VOL_UP`, `PLAY_PAUSE`, `NEXT` | Media and volume control |
| Layer switch | `LAYER:2` | Switches the active hardware layer |

---

## Repository and Layout Structure

The repository is organized to separate the individual development domains of the dual-node ecosystem.

```
.
├── firmware/
│   ├── transmitter/      Monolithic macro pad firmware, embedded web UI, ESP-NOW logic
│   └── receiver/         USB HID gateway and packet-translation routines
├── hardware/
│   ├── schematics/       Circuit schematics
│   ├── pcb/              PCB Gerber layouts
│   └── enclosure/        3D-printable enclosure models
├── LICENSE
└── README.md
```

The `firmware/transmitter/` directory contains the C++ source for the macro pad, including the embedded web interface and the wireless transmission logic. The `firmware/receiver/` directory houses the gateway logic and the native USB HID translation routines. All physical design files, including circuit schematics, PCB Gerber layouts, and 3D-printable enclosure models, are located within the `hardware/` directory.

---

## Hardware Requirements

- One ESP32-S3 development board for the transmitter, with native hardware USB support.
- One ESP32-S3 development board for the receiver, with native hardware USB support.
- Up to 16 mechanical switches wired to GPIO pins 1 through 16 and ground, using the internal pull-up resistors (no external resistors required).
- A USB-C connection from the receiver to the host computer that will receive the HID events.

---

## Deployment and Compilation Protocols

Compilation is fully compatible with the Arduino IDE using **ESP32 Core version 3.x.x or higher** (ESP-IDF v5 based). Before compiling the transmitter code, the external `AsyncTCP` and `ESPAsyncWebServer` dependencies must be added to the build environment from source, using the ESP32-compatible forks of those libraries.

Recommended Arduino IDE settings:

| Setting | Value |
|---------|-------|
| Board | ESP32S3 Dev Module |
| USB Mode | USB-OTG (TinyUSB) |
| USB CDC On Boot | Enabled |
| Upload Mode | UART |
| Partition Scheme | 8 MB Flash with Huge APP, or larger |
| Flash Size | 8 MB (or as provided by the board) |

A large partition scheme is required because the non-volatile storage matrix persists 160 independent macro sequences alongside the wireless update structures. Selecting an undersized partition will cause storage writes to fail.

---

## First-Time Pairing

1. Flash the transmitter firmware to the first board and the receiver firmware to the second board.
2. Power on the receiver and connect to its web interface (served on its Soft-AP, or via `macropadreceiver.local` once networked). Copy the hardware MAC address displayed on its page.
3. Open the transmitter's web portal, navigate to the Device Pairing tab, and enter the receiver's MAC address to register the ESP-NOW peer.
4. Enter your home Wi-Fi credentials in the transmitter's System Settings and save. The credentials are synchronized to the receiver automatically, and the receiver reports its IP address back to the transmitter once connected.
5. Configure macros, layers, and device behavior from the transmitter portal. Changes are saved over an asynchronous REST interface without reloading the page.

---

## Author and License

Developed by **Doruk Erel**.

Released under the **MIT License**. See the `LICENSE` file for the full text.
