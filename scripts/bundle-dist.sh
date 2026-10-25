#!/bin/bash
# Build dist/ so Cinder runs on a machine that does not have this Qt installed.
# The folder carries the binary, the Qt libraries it links, and the plugins it loads.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -x "$ROOT/build/cinder" ]]; then
    echo "cinder: build the binary first (scripts/build.sh)" >&2
    exit 1
fi

if [[ -n "${QT_PREFIX:-}" && -e "$QT_PREFIX/lib/libQt6Core.so" ]]; then
    qt_prefix="$(cd "$QT_PREFIX" && pwd)"
    qt_lib="$qt_prefix/lib"
else
    core="$(ldd "$ROOT/build/cinder" | awk '/libQt6Core\.so/{print $3; exit}')"
    if [[ -z "$core" || ! -e "$core" ]]; then
        echo "cinder: could not locate libQt6Core for the binary" >&2
        exit 1
    fi
    qt_lib="$(cd "$(dirname "$core")" && pwd)"
    qt_prefix="$(cd "$qt_lib/.." && pwd)"
fi

rm -rf "$ROOT/dist"
mkdir -p "$ROOT/dist/bin" "$ROOT/dist/lib" "$ROOT/dist/plugins"

cp -a "$ROOT/build/cinder" "$ROOT/dist/bin/cinder"
cat > "$ROOT/dist/bin/qt.conf" << 'EOF'
[Paths]
Prefix = ..
Plugins = plugins
Libraries = lib
EOF

plugins=()
add_plugin() {
    local rel="$1"
    local src="$qt_prefix/plugins/$rel"
    if [[ -f "$src" ]]; then
        mkdir -p "$ROOT/dist/plugins/$(dirname "$rel")"
        cp -a "$src" "$ROOT/dist/plugins/$rel"
        plugins+=("$ROOT/dist/plugins/$rel")
    fi
}
add_plugin_dir() {
    local rel="$1"
    local src="$qt_prefix/plugins/$rel"
    [[ -d "$src" ]] || return 0
    mkdir -p "$ROOT/dist/plugins/$rel"
    cp -a "$src"/. "$ROOT/dist/plugins/$rel/"
    local so
    while IFS= read -r so; do
        plugins+=("$so")
    done < <(find "$ROOT/dist/plugins/$rel" -name '*.so' -o -name '*.so.*')
}

add_plugin platforms/libqoffscreen.so
add_plugin platforms/libqminimal.so
add_plugin platforms/libqxcb.so
add_plugin platforms/libqwayland.so
add_plugin_dir xcbglintegrations
add_plugin_dir wayland-shell-integration
add_plugin_dir wayland-decoration-client
add_plugin_dir wayland-graphics-integration-client
add_plugin_dir platforminputcontexts
add_plugin_dir imageformats
add_plugin_dir iconengines

copy_lib() {
    local src="$1"
    local real base soname
    real="$(readlink -f "$src")"
    base="$(basename "$real")"
    if [[ ! -e "$ROOT/dist/lib/$base" ]]; then
        cp -a "$real" "$ROOT/dist/lib/$base"
    fi
    soname="$(readelf -d "$real" | awk -F'[][]' '/SONAME/{print $2; exit}')"
    if [[ -n "$soname" && ! -e "$ROOT/dist/lib/$soname" ]]; then
        ln -sfn "$base" "$ROOT/dist/lib/$soname"
    fi
}

declare -A seen=()
queue=("$ROOT/dist/bin/cinder" "${plugins[@]}")
while [[ ${#queue[@]} -gt 0 ]]; do
    bin="${queue[0]}"
    queue=("${queue[@]:1}")
    [[ -n "${seen[$bin]:-}" ]] && continue
    seen["$bin"]=1
    # Prefer the Qt tree we linked against. ldd can hand back a system copy of the
    # same SONAME when a plugin's $ORIGIN path is not filled in yet.
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        soname="${line%% *}"
        resolved=""
        if [[ "$line" == *" => /"* ]]; then
            resolved="$(awk '{print $3}' <<< "$line")"
        fi
        if [[ -e "$qt_lib/$soname" ]]; then
            dep="$qt_lib/$soname"
        elif [[ -n "$resolved" && "$resolved" == "$qt_lib"/* ]]; then
            dep="$resolved"
        else
            continue
        fi
        copy_lib "$dep"
        queue+=("$(readlink -f "$dep")")
    done < <(ldd "$bin" || true)
done

cat > "$ROOT/dist/cinder" << 'EOF'
#!/bin/sh
# Launch the bundled editor. Qt libraries and plugins live next to this script.
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# COSMIC Bar is offscreen and used to pass that platform to everything it
# opened. With a real display, drop it so this editor shows a window.
if [ "${QT_QPA_PLATFORM:-}" = "offscreen" ] && [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    unset QT_QPA_PLATFORM
    unset QT_ENABLE_HIGHDPI_SCALING
fi
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/plugins/platforms"
exec "$HERE/bin/cinder" "$@"
EOF
chmod 0755 "$ROOT/dist/cinder" "$ROOT/dist/bin/cinder"

missing=0
for need in \
    lib/libQt6Core.so.6 \
    lib/libQt6Gui.so.6 \
    lib/libQt6Widgets.so.6 \
    lib/libQt6Network.so.6 \
    plugins/platforms/libqoffscreen.so \
    plugins/platforms/libqxcb.so \
    plugins/platforms/libqwayland.so
do
    if [[ ! -e "$ROOT/dist/$need" ]]; then
        echo "cinder: dist is missing $need" >&2
        missing=1
    fi
done
if [[ "$missing" -ne 0 ]]; then
    exit 1
fi

echo "cinder: wrote $ROOT/dist ($(du -sh "$ROOT/dist" | awk '{print $1}'))"
echo "cinder: launch with $ROOT/dist/cinder"
