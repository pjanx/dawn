#!/bin/sh -xeu
# Dependencies: flatpak, flatpak-builder, binutils/strip, curl, git
appid=name.janouch.Dawn
arch=$(flatpak --default-arch)
src=$(CDPATH= cd "$(dirname "$0")" && pwd)
dst=${1:-$src/build-flatpak}

version=$(awk '$1 == "VERSION" { print $2; exit }' "$src/CMakeLists.txt")
baseyml=$src/$appid.BaseApp.yml

# XXX: Not sure how to version the BaseApp; supposedly newer means better.
baseapp=$dst/Dawn-BaseApp-latest-$arch.flatpak
bundle=$dst/Dawn-${DAWN_VERSION:-$version}-$arch.flatpak

# Flatpak insists on this, and a disposable machine has no login session.
XDG_RUNTIME_DIR=$dst/xdg-runtime
export XDG_RUNTIME_DIR
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

flathub=https://flathub.org/repo/flathub.flatpakrepo
flatpak --user remote-add --if-not-exists flathub "$flathub"

# Don't rebuild dependencies on every push, but refresh automatically.
if [ ! -f "$baseapp" ]
then
	code=$(curl -f#R -z "$(git -C "$src" log -1 --format=%cD -- "$baseyml")" \
		-o "$baseapp" -w '%{http_code}' \
		"https://janouch.name/cd/${baseapp##*/}") || [ "$code" = 404 ]
fi
if [ ! -f "$baseapp" ]
then
	# Qt's build tree is several gigabytes, and of no interest once it is built.
	flatpak-builder --user --install-deps-from=flathub --force-clean \
		--delete-build-dirs --default-branch=stable \
		--state-dir="$dst/state" --repo="$dst/repo-base" \
		"$dst/appdir-base" "$baseyml"
	flatpak build-bundle --arch="$arch" --runtime-repo="$flathub" \
		"$dst/repo-base" "$baseapp" "$appid.BaseApp" stable
fi

flatpak --user install -y --reinstall "$baseapp"

runtime_version=$(sed -n "s/^runtime-version: '\(.*\)'$/\1/p" "$src/$appid.yml")
flatpak --user install --or-update -y flathub \
	"org.freedesktop.Sdk//$runtime_version"

flatpak-builder --user --force-clean \
	--default-branch=stable --state-dir="$dst/state" \
	--repo="$dst/repo" "$dst/appdir" "$src/$appid.yml"
flatpak build-bundle --arch="$arch" --runtime-repo="$flathub" \
	"$dst/repo" "$bundle" "$appid" stable

flatpak --user install -y --reinstall "$bundle"
flatpak run --user --env=QT_QPA_PLATFORM=offscreen "$appid" \
	--list-supported-media-types
