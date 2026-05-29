# LVGL App Manifest v1

## Purpose

This document defines the first application manifest format for the Intewell
LVGL desktop system.

The goal of v1 is modest:

- describe launchable desktop apps
- let `lvgl_desktop` discover apps from disk
- provide enough metadata for a launcher and task list
- avoid adding a large parser or complex packaging format too early

This is not a package format. It is only an app registry format.

## Scope

v1 is designed for:

- built-in apps
- uploaded native user-space programs
- remote LVGL apps launched by `lvgl_desktop`

v1 is not designed for:

- dependency resolution
- version constraints between apps
- sandboxing
- permissions enforcement
- package signatures
- incremental upgrades

Those belong to a later package manager / installer layer.

## Discovery

Recommended discovery directories:

- `/etc/apps`
- `/usr/share/applications`

For v1, `lvgl_desktop` should scan one directory first, not recurse, and load
all files ending in `.json`.

Recommended initial choice:

- `/etc/apps`

Reason:

- simpler to manage during development
- easy to update by FTP
- avoids mixing desktop registry with upstream documentation or assets

## File Naming

Each app manifest is one JSON file:

```text
<app_id>.json
```

Examples:

```text
terminal.json
remote-demo.json
file-manager.json
```

The filename should match `app_id`, but the runtime should still trust the JSON
field value, not only the filename.

## Encoding

- UTF-8 text
- JSON object at top level
- no comments

Keep fields ASCII unless there is a clear reason to localize display text.

## Required Fields

Every manifest must contain:

- `manifest_version`
- `app_id`
- `name`
- `exec`
- `type`

### `manifest_version`

Type:

- integer

Required value for this spec:

```json
1
```

### `app_id`

Type:

- string

Rules:

- stable identifier
- lowercase recommended
- use `a-z`, `0-9`, `-`, `_`, `.`
- should not change once published

Examples:

```json
"terminal"
"remote-demo"
"files"
```

### `name`

Type:

- string

Purpose:

- launcher display name
- task list / window menu label

Examples:

```json
"Terminal"
"Remote Demo"
"Files"
```

### `exec`

Type:

- string

Purpose:

- exact command line to launch

v1 rule:

- desktop may execute this using a shell-like split on spaces only
- therefore keep it simple
- avoid embedded quoting in v1 unless launcher later adds a proper parser

Recommended:

- executable path first
- optional flat arguments after it

Examples:

```json
"./lvgl_remote_terminal"
"/root/lvgl_remote_demo"
"/root/lvgl_file_demo /"
```

### `type`

Type:

- string

Allowed v1 values:

- `"remote-lvgl"`
- `"native-terminal"`

Current expected default:

- use `"remote-lvgl"` for all GUI apps in this desktop model

`"native-terminal"` is reserved for future use if a non-windowed tool is wrapped
or launched through a terminal host.

## Optional Fields

### `icon`

Type:

- string

Purpose:

- icon asset reference

v1 recommendation:

- symbolic ID, not a binary blob

Examples:

```json
"terminal"
"folder"
"settings"
```

Desktop can map symbolic IDs to built-in icons.

### `cwd`

Type:

- string

Purpose:

- working directory before launch

Examples:

```json
"/"
"/root"
```

If omitted, desktop may use `/`.

### `args`

Type:

- array of strings

Purpose:

- structured alternative to embedding arguments inside `exec`

For v1, choose one of these patterns and do not mix them casually:

- simple mode: use `exec` only
- structured mode: use `exec` as executable path and `args` as argv tail

Recommended long-term direction:

- keep `exec` as executable path
- use `args` for arguments

Example:

```json
{
  "exec": "/root/lvgl_file_demo",
  "args": ["/"]
}
```

### `env`

Type:

- object of string-to-string

Purpose:

- additional environment variables

Example:

```json
{
  "env": {
    "LVGL_SOCKET": "/run/lvgl-desktop.sock"
  }
}
```

### `categories`

Type:

- array of strings

Purpose:

- launcher grouping
- future search/filter support

Examples:

```json
["system", "utility"]
["development", "terminal"]
```

### `startup_notify`

Type:

- boolean

Purpose:

- whether launcher should show a launching state until the app creates a window

Recommended default:

```json
true
```

### `single_instance`

Type:

- boolean

Purpose:

- whether launcher should refuse to start another instance if one already runs

Recommended default:

```json
false
```

### `topmost`

Type:

- boolean

Purpose:

- launcher hint that desktop may pass into window creation policy

Recommended default:

```json
false
```

### `autostart`

Type:

- boolean

Purpose:

- app starts automatically when desktop session comes up

Recommended default:

```json
false
```

### `hidden`

Type:

- boolean

Purpose:

- exclude from launcher UI while keeping it registered

Useful for internal helper apps.

## Reserved Fields

These names should not be repurposed casually:

- `version`
- `vendor`
- `description`
- `permissions`
- `entry`
- `window`
- `package`
- `update`

They are reserved because they are likely to be useful in v2+.

## Recommended v1 Runtime Rules

When `lvgl_desktop` loads manifests:

1. Ignore invalid JSON files.
2. Ignore manifests with unsupported `manifest_version`.
3. Ignore manifests missing required fields.
4. Ignore duplicate `app_id` after first valid entry, and log it.
5. Keep manifest parsing non-fatal to the desktop session.

When launching an app:

1. Use `cwd` if present, else `/`.
2. Use `exec` and optional `args`.
3. Record `app_id`, `pid`, launch time, and manifest path.
4. Wait for the app to create a remote LVGL window if `type == "remote-lvgl"`.

## Recommended v1 In-Memory Structure

This is not required, but is a sensible C-side shape:

```c
struct lvgl_app_manifest {
    int manifest_version;
    char app_id[64];
    char name[64];
    char exec[128];
    char cwd[128];
    char icon[32];
    char type[32];
    bool startup_notify;
    bool single_instance;
    bool topmost;
    bool autostart;
    bool hidden;
};
```

Keep v1 fixed-size and simple.

## Example Manifests

### Terminal

```json
{
  "manifest_version": 1,
  "app_id": "terminal",
  "name": "Terminal",
  "type": "remote-lvgl",
  "exec": "/root/lvgl_remote_terminal",
  "cwd": "/",
  "icon": "terminal",
  "categories": ["system", "development"],
  "startup_notify": true,
  "single_instance": false,
  "topmost": false,
  "autostart": false,
  "hidden": false
}
```

### Remote Demo

```json
{
  "manifest_version": 1,
  "app_id": "remote-demo",
  "name": "Remote Demo",
  "type": "remote-lvgl",
  "exec": "/root/lvgl_remote_demo",
  "cwd": "/root",
  "icon": "demo",
  "categories": ["demo"],
  "startup_notify": true,
  "single_instance": false,
  "topmost": false,
  "autostart": false,
  "hidden": false
}
```

## Suggested Directory Layout

Recommended initial layout on target:

```text
/root/lvgl_desktop
/root/lvgl_remote_terminal
/root/lvgl_remote_demo
/etc/apps/terminal.json
/etc/apps/remote-demo.json
```

This keeps binaries and launcher metadata separate.

## Versioning Strategy

Use `manifest_version` to version the schema, not the app release.

If app release version is needed later, add:

```json
"version": "0.1.0"
```

But do not make launcher behavior depend on that in v1.

## Design Constraints

1. v1 must be trivial to parse in C without adding a heavy dependency.
2. v1 must support direct FTP-based deployment during development.
3. v1 must map cleanly onto the current `lvgl_desktop` remote-app model.
4. v1 should not assume package management exists yet.
5. v1 should avoid shell-parsing complexity whenever possible.

## Next Step After This Spec

The next implementation milestone should be:

1. add manifest loader to `lvgl_desktop`
2. scan `/etc/apps`
3. build a launcher UI from loaded manifests
4. support click-to-launch using `fork/exec`

That is the correct follow-up after manifest v1.
