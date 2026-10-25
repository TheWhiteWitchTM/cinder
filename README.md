# Cinder

A black editor for bash and sh. Write a script, run it, trace it, or stop on a breakpoint. Built with Qt 6.

## Open it

```bash
cd ~/Projects/cinder
scripts/build.sh
./cinder
```

`./cinder` runs the build when Qt is installed. `scripts/bundle-dist.sh` writes `dist/`, the same shape as Vesper: `dist/cinder` launches it, and `dist/bin`, `dist/lib`, and `dist/plugins` carry the binary, the Qt libraries, and the plugins. That copy starts on a machine where Qt is not installed.

```bash
scripts/bundle-dist.sh
./dist/cinder
```

Drop a script on the window, or pass it on the command line.

```bash
./cinder ~/bin/backup.sh
./cinder ~/scripts
```

Cinder shows up in the app grid as **Cinder** after the desktop file is installed.

```bash
./cinder --self-test
```

## What it does

- Bash and sh in tabs, with a file tree and an outline of functions.
- Syntax highlighting on a black page: keywords, builtins, strings, variables, and a warning color for bash syntax in an sh script.
- Run with F5. The interpreter follows the shebang, or the Auto / bash / sh choice in the toolbar. Arguments are the box next to it.
- Trace with F6 (`bash -x`). The line that is running is marked in the gutter.
- Debug with F7. Click the gutter to set a breakpoint. F10 steps over, F11 steps in, F8 pauses a running debug, F5 continues. Variables the script creates show up in the Variables tab.
- sh scripts run with `/bin/sh`. Breakpoints need bash, because dash has no debug trap. Cinder says so in the console.
- A problem list for unclosed quotes and bash syntax in sh files. If `shellcheck` is installed, its notes replace that list.
- Find, replace, comment toggle, and indent that follows `then`, `do`, and `{`.

The editor is black. Ember is the accent. Settings live in `~/.config/cinder/`.

Cinder is MIT licensed. See `LICENSE`. The About window is Mira, The White Witch.

## Keys

F5 runs, and continues a paused debug. F6 traces. F7 debugs. Shift+F5 stops. F8 pauses. F9 toggles a breakpoint. F10 steps over. F11 steps in. Ctrl+/ comments the selection. Ctrl+F finds, Ctrl+H replaces, Ctrl+G jumps to a line.

## Build notes

Qt 6 widgets come from the system, or from `~/Qt/6.12.0` when that tree is present. `scripts/build.sh` prefers that qmake. Launch with `./cinder`, not `build/cinder` directly: the wrapper picks the bundled copy when the system Qt is too old. `scripts/bundle-dist.sh` copies Qt into `dist/` so the editor does not need Qt installed.
