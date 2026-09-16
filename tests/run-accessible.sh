#!/bin/sh
# Run the external accessibility test in a session of its very own.
#
# Usage: run-accessible.sh TEST-BINARY DN-BINARY DIRECTORY
#
# Everything the test touches is private: its own D-Bus session, which is
# also what starts the accessibility bus through ordinary service
# activation; its own headless Weston; and its own XDG directories, which is
# what keeps dn's single-instance socket away from the developer's session.
# The locale is pinned, because nodes are found by their translated names.
#
# Env:
#   DN_A11Y_STRICT   fail instead of skipping when something is missing,
#                    which is what a job configured for accessibility wants.
#   WESTON_RENDERER  vulkan (default) | gl | pixman.  docs/functional-
#                    specification.adoc: gl has been seen rendering upside-
#                    down while headless.  An offscreen Qt platform is not an
#                    option here: it exercises neither client-side
#                    decorations nor compositor geometry.
#   WESTON_TIMEOUT   180 (default), seconds before the session is killed.
set -eu

if [ ! $# -eq 3 ]
then
	echo >&2 "usage: run-accessible.sh TEST DN DIRECTORY"
	exit 2
fi

test_bin=$1
dn_bin=$2
dir=$3

missing() {
	echo >&2 "run-accessible: $1"
	# Asked for explicitly, so its absence is a failure rather than a skip.
	[ -z "${DN_A11Y_STRICT:-}" ] || exit 1
	exit 77
}

command -v dbus-run-session >/dev/null 2>&1 || missing "no dbus-run-session"
command -v dbus-daemon >/dev/null 2>&1 || missing "no dbus-daemon"
command -v weston >/dev/null 2>&1 || missing "no weston"

# The bus comes up through D-Bus activation, so all that has to exist is the
# service file -- wherever the spec says to look for it, and not in any one
# distribution's libexec.
found=
IFS=:
for d in ${XDG_DATA_DIRS:-/usr/local/share:/usr/share}
do [ -f "$d/dbus-1/services/org.a11y.Bus.service" ] && found=$d
done
unset IFS
if [ -z "$found" ]
then missing "no org.a11y.Bus service (install at-spi2-core)"
fi

renderer=${WESTON_RENDERER:-vulkan}
timeout_s=${WESTON_TIMEOUT:-180}

rt=$(mktemp -d)
chmod 700 "$rt"
cleanup() {
	rm -rf "$rt"
}
trap cleanup EXIT INT TERM

log=$rt/weston.log
mkdir -p "$rt/run" "$rt/config" "$rt/cache" "$rt/data" "$rt/state"
chmod 700 "$rt/run"

# Qt must find neither the session's compositor nor an X server, and the
# bridge must not be pointed at the session's own accessibility bus.
unset DISPLAY QT_QPA_PLATFORM WAYLAND_DISPLAY AT_SPI_BUS_ADDRESS
# dbus-broker on the a11y bus activates Registry via systemd, which a
# dbus-run-session does not have.  dbus-daemon runs the service Exec=.
export ATSPI_DBUS_IMPLEMENTATION=dbus-daemon
export XDG_RUNTIME_DIR=$rt/run
export XDG_CONFIG_HOME=$rt/config
export XDG_CACHE_HOME=$rt/cache
export XDG_DATA_HOME=$rt/data
export XDG_STATE_HOME=$rt/state

# Otherwise there's autodetection, for performance reasons.
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1

# Names are translated strings, so the test only knows the untranslated ones.
export LANGUAGE=
if locale -a 2>/dev/null | grep -qi '^C\.utf-\?8$'
then export LC_ALL=C.UTF-8
else export LC_ALL=C
fi

# Weston exits with the compositor's status, never with its startup client's,
# so the client writes its own down on the way out.
result=$rt/status

session=0
# Overflow coverage asks the compositor to configure a new toplevel size.
# The helper lives next to the test binary; an absolute path is what
# weston_load_module accepts besides names in /usr/lib/weston.  The test
# asserts that resize, and the build only produces it together with the
# plugin, so a missing one here is a broken build rather than a thin host.
test_dir=$(CDPATH= cd -- "$(dirname -- "$test_bin")" && pwd)
plugin=$test_dir/weston-a11y-resize.so
if [ ! -f "$plugin" ]
then
	echo >&2 "run-accessible: $plugin was not built"
	exit 1
fi

# dbus-broker activates SystemdService= units, which a dbus-run-session
# has no user systemd for.  dbus-daemon runs the service file's Exec=,
# which is how the accessibility bus is meant to come up here.
timeout "$timeout_s" dbus-run-session --dbus-daemon="$(command -v dbus-daemon)" -- \
	weston \
		--backend=headless \
		--renderer="$renderer" \
		--shell=desktop \
		--width=1920 \
		--height=1080 \
		--socket=weston-a11y-$$ \
		--idle-time=0 \
		--no-config \
		--fake-seat \
		--log="$log" \
		--modules="$plugin" \
		-- /bin/sh -c '"$1" "$2" "$3"; echo $? >"$4"' \
			run-accessible "$test_bin" "$dn_bin" "$dir" "$result" \
	|| session=$?

status=1
if [ -s "$result" ]
then
	status=$(cat "$result")
	# Whatever is in there came out of the session; a file that is not a
	# number is one more way for this to have gone wrong.
	case $status in
	(''|*[!0-9]*) status=1 ;;
	esac
else
	# Killed with the session, or never started: either way, not a pass.
	echo >&2 "run-accessible: the test left no exit status behind"
fi

if [ "$status" -eq 0 ] && [ "$session" -ne 0 ]
then
	echo >&2 "run-accessible: the session exited $session"
	status=$session
fi
if [ "$status" -ne 0 ]
then
	echo >&2 "run-accessible: exit $status"
	[ -f "$log" ] && sed -n '1,60p' "$log" >&2
fi
exit "$status"
