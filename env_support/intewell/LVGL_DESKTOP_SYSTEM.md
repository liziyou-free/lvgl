# LVGL Desktop System

## Purpose

This document describes the current Intewell LVGL desktop system implemented in:

- `env_support/intewell/apps/lvgl_desktop`
- `env_support/intewell/apps/lvgl_remote_terminal`
- `env_support/intewell/apps/lvgl_remote_demo`
- `env_support/intewell/lv_remote.h`

It is intended as a compact architecture reference for future developers and AI agents.

The system is not a generic upstream LVGL feature. It is a local desktop/windowing layer built on top of LVGL for Intewell RTOS.

## High-Level Model

There is exactly one process that owns the real display and input devices:

- `lvgl_desktop`

All GUI applications are remote clients:

- `lvgl_remote_terminal`
- `lvgl_remote_demo`
- future remote LVGL apps

Remote apps do not open `/dev/fb0`, `/dev/input0`, or `/dev/kbd0` directly.
They render into their own shared surface file and communicate with `lvgl_desktop`
over a local `AF_UNIX` socket.

`lvgl_desktop` acts as:

- display owner
- window manager
- compositor
- input router

This is a Wayland-like model, but much simpler.

## Display/Input Ownership

### `lvgl_desktop`

Owns:

- `/dev/fb0`
- `/dev/input0`
- `/dev/kbd0` or `/dev/kbd`
- `/run/lvgl-desktop.sock`

Responsibilities:

- initialize LVGL on the real framebuffer
- create desktop UI and window chrome
- accept remote app connections
- map remote surface files
- composite remote windows into the desktop framebuffer
- route pointer and keyboard events to the focused app

### Remote apps

Own:

- one `AF_UNIX` client socket connected to `/run/lvgl-desktop.sock`
- one surface file under `/run/lvgl-surface-*.fb`
- their own LVGL instance rendering into that surface

Responsibilities:

- create and publish a surface
- send window metadata and commit rectangles
- receive routed pointer/keyboard input from desktop
- implement app-specific logic

## Process Topology

```text
VNC viewer
    ->
kernel VNC framebuffer + keyboard + pointer
    ->
/dev/fb0 /dev/input0 /dev/kbd0
    ->
lvgl_desktop
    <->  /run/lvgl-desktop.sock
         <-> lvgl_remote_terminal
         <-> lvgl_remote_demo
         <-> future remote LVGL apps
```

## Transport

### Control socket

Path:

- `/run/lvgl-desktop.sock`

Type:

- `AF_UNIX`
- `SOCK_STREAM`

Messages use fixed-size `struct lv_remote_msg` defined in `lv_remote.h`.

### Surface file

Each remote app creates a file like:

- `/run/lvgl-surface-<pid>-<token>-0.fb`

Current assumptions:

- RGB565
- stride is app-defined but currently width `* 2`
- desktop reads from the file and copies dirty rectangles into its cached window image

## Protocol

Defined in:

- `env_support/intewell/lv_remote.h`

Current message types:

- `LV_REMOTE_MSG_HELLO`
- `LV_REMOTE_MSG_CREATE_WINDOW`
- `LV_REMOTE_MSG_ATTACH_BUFFER`
- `LV_REMOTE_MSG_COMMIT`
- `LV_REMOTE_MSG_INPUT_POINTER`
- `LV_REMOTE_MSG_INPUT_KEY`
- `LV_REMOTE_MSG_CLOSE_WINDOW`
- `LV_REMOTE_MSG_TERMINATE`

Important fields:

- `pid`
- `window_id`
- `x/y/w/h`
- `width/height/stride/format`
- `key/action`
- `path`
- `title`

Current behavior:

- remote app sends `HELLO`
- remote app sends `CREATE_WINDOW`
- remote app sends `ATTACH_BUFFER`
- remote app renders and sends `COMMIT` rectangles
- desktop sends `INPUT_POINTER` and `INPUT_KEY` to the focused app

## Window Model

Each remote app maps to one desktop window.

Current desktop window features:

- title bar
- drag by title bar
- close button
- focus border
- z-order raising
- topmost flag support in protocol

Current limitations:

- no resize protocol
- no minimize implementation
- no maximize implementation
- no decoration theme abstraction
- one surface per app window

## Focus Rules

Current intended focus rules:

- pointer move over a window does not focus it
- clicking a window focuses it
- keyboard input goes to the currently focused window
- focused window is raised above normal windows

Implementation note:

- focus is managed in `lvgl_desktop`
- input events must not be broadcast to all clients

## Current Input Routing

### Pointer

Desktop reads `/dev/input0`, converts to desktop coordinates, finds the topmost
remote window under the pointer, and forwards pointer position/button state to that app.

Focus is acquired only on press edge.

### Keyboard

Desktop reads `/dev/kbd0` and forwards key events to:

- `g_focused` if non-null
- otherwise the current top window

Remote apps do not read `/dev/kbd0` directly.

## Current Rendering Model

### Desktop side

`lvgl_desktop` uses LVGL directly on `/dev/fb0`.

It also caches each remote app surface into `image_pixels` and composites windows
into the framebuffer.

### Remote side

Each remote app runs its own LVGL instance and flushes into its own surface file.
After each flush it sends a `COMMIT` rectangle to desktop.

This is software composition only.

## Remote Terminal Design

`lvgl_remote_terminal` is not a direct serial terminal.

It uses:

- remote LVGL window surface
- PTY master `/dev/ptmx`
- slave `/dev/pts/N`
- child shell loop attached to the PTY slave

The terminal app:

- receives `INPUT_KEY` from desktop
- writes translated bytes/sequences to the PTY master
- reads PTY output from the master
- appends the output to the LVGL label-based terminal view

Important consequence:

- terminal input problems can come from desktop focus/routing
- local socket backpressure
- PTY write semantics
- PTY child shell behavior

Do not assume the issue is always in LVGL itself.

## Important Kernel Assumptions

This desktop system depends on the RTOS local socket implementation.

The `AF_UNIX SOCK_STREAM` implementation is not equivalent to Linux behavior in all details.
In particular, local stream write/read and backpressure behavior must be treated carefully.

Previously observed kernel issue:

- multiple accepted local socket connections reused the same FIFO instance id
- this caused cross-stream message mixing between multiple remote apps

That issue was fixed in the RTOS tree by giving each accepted stream connection a unique FIFO instance id.

## Current User-Space Mitigations

Several local mitigations have already been added in the LVGL desktop system:

- hot-path key logging was removed
- terminal output logging was reduced
- focus on pointer hover was removed
- remote terminal main loop processes socket/PTY before running `lv_timer_handler()`
- PTY writes use retry logic instead of single-byte fire-and-forget writes
- desktop input send path now uses per-window send queues and sender threads so UI/input polling is decoupled from `AF_UNIX` socket backpressure

These are workarounds around current RTOS behavior and timing sensitivity.

## Known Open Issue

The main unresolved issue at the time of writing is:

- terminal keyboard input can still be discontinuous or require repeated/rapid keypresses in some cases

Current best hypothesis:

- this is not just an LVGL refresh issue
- it is likely related to the interaction of:
  - desktop input routing
  - `AF_UNIX SOCK_STREAM` semantics in the RTOS
  - remote app socket receive timing
  - PTY backend timing

Future debugging should use counters at each stage instead of ad hoc print spam:

- `/dev/kbd` events read by desktop
- input messages enqueued by desktop
- input messages written by desktop sender thread
- input messages received by remote app
- bytes written to PTY master
- bytes consumed by PTY child shell

## Files and Roles

### `env_support/intewell/apps/lvgl_desktop/main.c`

Contains:

- framebuffer setup
- pointer and keyboard input handling
- desktop UI
- remote window lifecycle
- compositor
- local socket server
- per-window RX handling
- per-window TX queue/thread for input dispatch

### `env_support/intewell/apps/lvgl_remote_terminal/main.c`

Contains:

- remote surface creation
- local socket client
- PTY-backed terminal
- remote input handling
- terminal rendering

### `env_support/intewell/apps/lvgl_remote_demo/main.c`

Contains:

- simplest reference remote LVGL client
- useful for isolating desktop/transport issues from PTY terminal issues

### `env_support/intewell/lv_remote.h`

Contains:

- wire message definition
- protocol constants
- common send helper

Any protocol extension should start here.

## Design Rules For Future Work

1. Only `lvgl_desktop` may own `/dev/fb0`, `/dev/input0`, `/dev/kbd0`.
2. Remote apps must not bypass desktop for display or input.
3. Do not assume local socket writes are cheap or reliably blocking.
4. Avoid hot-path `printf` in input, socket, PTY, and flush paths.
5. Keep protocol messages fixed-size unless there is a strong reason to change.
6. When debugging input loss, instrument each hop and compare counts.
7. Use `lvgl_remote_demo` as the baseline to distinguish generic transport issues from terminal-specific issues.

## Suggested Next Steps

1. Add low-noise event counters for keyboard routing and PTY writes.
2. Add optional protocol-level ACK or sequence tracing for input events.
3. Consider replacing stream control transport with a more explicit message channel if local stream semantics remain unreliable.
4. Consider a dedicated terminal widget or more efficient text view if terminal output volume becomes a bottleneck.

## App Registry Direction

The first app registry format is defined in:

- `env_support/intewell/LVGL_APP_MANIFEST_V1.md`

Recommended target-side discovery directory:

- `/etc/apps`

Example manifests already exist in the repository:

- `env_support/intewell/apps/terminal.json`
- `env_support/intewell/apps/remote-demo.json`
