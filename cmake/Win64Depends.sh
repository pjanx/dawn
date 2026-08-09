#!/bin/sh -e
# Win64Depends.sh: MSYS2 ucrt64 prefix + resvg, SwiftShader, MSVC CRT,
# shared-mime-info for MinGW-w64 cross builds.
#
# Usage: sh cmake/Win64Depends.sh <build-dir>
# Example: sh dawn/cmake/Win64Depends.sh build-mingw
#
# Host: awk, curl, bsdtar, sha256sum, x86_64-w64-mingw32-gcc, 7z,
# update-mime-database (shared-mime-info).
# resvg uses a rustup toolchain in the build tree (Arch rustc cannot load
# upstream rust-std).
set -e

repository=https://repo.msys2.org/mingw/ucrt64/
pkg=mingw-w64-ucrt-x86_64
resvg_ver=0.48.1
resvg_url="https://github.com/linebender/resvg/archive/refs/tags/v${resvg_ver}.tar.gz"
resvg_sha256=40dafea6b4b9d01e9d28b6d49f1e912daf3e9055676ad9179a5a2db6e7386945
swiftshader_url=https://github.com/jakoch/rasterizers/releases/download/20260731/swiftshader-win64-5.0.0.1.zip
swiftshader_sha256=63e97af8b88c2c8cbc61495fc5ef4fad1ce60a874f2c19207cd072c46944065f
vcredist_url=https://aka.ms/vs/17/release/vc_redist.x64.exe
vcredist_sha256=cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b
exiftool_ver=13.59
exiftool_url="https://sourceforge.net/projects/exiftool/files/Image-ExifTool-${exiftool_ver}.tar.gz/download"
exiftool_sha256=668ea3acececb7235fbd0f4900e72d5f12c9b07e5c778fd36cb1e9b5828fd65a

status() {
	echo "$(tput bold)-- $*$(tput sgr0)" >&2
}

if [ -z "$1" ]
then
	echo "usage: $0 <build-dir>" >&2
	exit 1
fi

builddir=$(realpath "$1")
msys2_root=$builddir/ucrt64
mkdir -p "$msys2_root"
cd "$msys2_root"

dbsync() {
	status Fetching repository DB
	[ -f db.tsv ] || curl -# "$repository/ucrt64.db" | bsdtar -xOf- | awk '
		function flush() { print f["%NAME%"] f["%FILENAME%"] f["%DEPENDS%"] }
		NR > 1 && $0 == "%FILENAME%" { flush(); for (i in f) delete f[i] }
		!/^[^%]/ { field = $0; next } { f[field] = f[field] $0 "\t" }
		field == "%SHA256SUM%" { path = "*packages/" f["%FILENAME%"]
			sub(/\t$/, "", path); print $0, path > "db.sums" } END { flush() }
	' > db.tsv
}

fetch() {
	status Resolving "$@"
	mkdir -p packages
	awk -F'\t' 'function get(name,    i, a) {
		if (visited[name]++ || !(name in filenames)) return
		print filenames[name]; split(deps[name], a); for (i in a) get(a[i])
	} BEGIN { while ((getline < "db.tsv") > 0) {
		filenames[$1] = $2; deps[$1] = ""; for (i = 3; i <= NF; i++) {
			gsub(/[<=>].*/, "", $i); deps[$1] = deps[$1] $i FS }
	} for (i = 0; i < ARGC; i++) get(ARGV[i]) }' "$@" | tee db.want | \
	while IFS= read -r name
	do
		status Fetching "$name"
		[ -f "packages/$name" ] || curl -#o "packages/$name" "$repository/$name"
	done
}

verify() {
	status Verifying checksums
	sha256sum --ignore-missing --quiet -c db.sums
}

extract() {
	status Extracting packages
	for subdir in *
	do [ -d "$subdir" -a "$subdir" != packages ] && rm -rf -- "$subdir"
	done
	while IFS= read -r name
	do bsdtar -xf "packages/$name" --strip-components 1 \
		--exclude '*/share/man' --exclude '*/share/doc'
	done < db.want
}

setup_rustup() {
	export RUSTUP_HOME=$builddir/rustup
	export CARGO_HOME=$builddir/cargo
	export PATH="$CARGO_HOME/bin:$PATH"
	if [ -x "$CARGO_HOME/bin/rustc" ] && \
		"$CARGO_HOME/bin/rustc" --print target-list >/dev/null 2>&1 && \
		"$CARGO_HOME/bin/rustc" --print target-libdir \
			--target x86_64-pc-windows-gnu >/dev/null 2>&1
	then
		return
	fi
	status rustup + x86_64-pc-windows-gnu
	export RUSTUP_INIT_SKIP_PATH_CHECK=yes
	curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
		sh -s -- -y --no-modify-path --profile minimal --default-toolchain stable
	rustup target add x86_64-pc-windows-gnu
}

install_resvg() {
	prefix=$msys2_root
	if [ -f "$prefix/bin/resvg.dll" ]
	then
		status "resvg already in prefix"
		return
	fi
	src=$builddir/resvg-$resvg_ver
	tarball=$builddir/resvg-$resvg_ver.tar.gz
	[ -f "$tarball" ] || curl -#L -o "$tarball" "$resvg_url"
	echo "$resvg_sha256  $tarball" | sha256sum -c
	[ -d "$src" ] || bsdtar -xf "$tarball" -C "$builddir"
	setup_rustup
	status Building resvg $resvg_ver C API
	export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc
	export CC_x86_64_pc_windows_gnu=x86_64-w64-mingw32-gcc
	export AR_x86_64_pc_windows_gnu=x86_64-w64-mingw32-gcc-ar
	unset RUSTC
	export CARGO_TARGET_DIR=$src/target
	( cd "$src/crates/c-api" && cargo build --release --target x86_64-pc-windows-gnu )
	rel=$src/target/x86_64-pc-windows-gnu/release
	mkdir -p "$prefix/include/resvg" "$prefix/lib/pkgconfig" "$prefix/bin"
	cp "$src/crates/c-api/resvg.h" "$prefix/include/resvg/resvg.h"
	cp "$rel/resvg.dll" "$prefix/bin/resvg.dll"
	cp "$rel/libresvg.dll.a" "$prefix/lib/libresvg.dll.a"
	cat >"$prefix/lib/pkgconfig/resvg.pc" <<-EOF
		prefix=/ucrt64
		exec_prefix=\${prefix}
		libdir=\${prefix}/lib
		includedir=\${prefix}/include

		Name: resvg
		Description: SVG rendering library (C API)
		Version: $resvg_ver
		Libs: -L\${libdir} -lresvg
		Cflags: -I\${includedir}/resvg
	EOF
}

install_swiftshader() {
	mkdir -p "$msys2_root/bin"
	if [ -f "$msys2_root/bin/vk_swiftshader.dll" ]
	then
		status "SwiftShader already in prefix"
	else
		zip=$builddir/swiftshader-win64-5.0.0.1.zip
		[ -f "$zip" ] || curl -#L -o "$zip" "$swiftshader_url"
		echo "$swiftshader_sha256  $zip" | sha256sum -c
		status SwiftShader ICD
		bsdtar -xf "$zip" -C "$msys2_root/bin" vk_swiftshader.dll
	fi
	cat >"$msys2_root/bin/vk_swiftshader_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": ".\\vk_swiftshader.dll",
        "api_version": "1.1.0"
    }
}
EOF
}

run7z() {
	cmd=$1
	shift
	"$cmd" x "$@" || {
		r=$?
		[ "$r" -le 1 ] || exit "$r"
	}
}

install_msvc_crt() {
	if [ -f "$msys2_root/bin/msvcp140.dll" ]
	then
		status "MSVC CRT already in prefix"
		return
	fi
	z=
	for c in 7z 7za 7zz
	do
		if command -v "$c" >/dev/null
		then
			z=$c
			break
		fi
	done
	if [ -z "$z" ]
	then
		echo "7z required to peel vc_redist.x64.exe" >&2
		exit 1
	fi
	exe=$builddir/vc_redist.x64.exe
	[ -f "$exe" ] || curl -#L -o "$exe" "$vcredist_url"
	echo "$vcredist_sha256  $exe" | sha256sum -c
	work=$builddir/msvc-crt-work
	rm -rf "$work"
	mkdir -p "$work/s" "$work/c" "$work/a"
	status "Peeling VC++ CRT"
	run7z "$z" -t# -y "-o$work/s" "$exe"
	run7z "$z" -y "-o$work/c" "$work/s/4.cab"
	run7z "$z" -y "-o$work/a" "$work/c/a12"
	mkdir -p "$msys2_root/bin"
	for n in msvcp140 vcruntime140 vcruntime140_1
	do cp "$work/a/${n}.dll_amd64" "$msys2_root/bin/${n}.dll"
	done
	rm -rf "$work"
}

install_exiftool() {
	version_file=$msys2_root/.exiftool-version
	installed_ver=
	[ ! -f "$version_file" ] || installed_ver=$(cat "$version_file")
	if [ -f "$msys2_root/bin/exiftool" ] && \
		[ "$installed_ver" = "$exiftool_ver" ]
	then
		status "ExifTool already in prefix"
		return
	fi
	tarball=$builddir/exiftool-$exiftool_ver.tar.gz
	[ -f "$tarball" ] || curl -#L -o "$tarball" "$exiftool_url"
	echo "$exiftool_sha256  $tarball" | sha256sum -c
	work=$builddir/exiftool-$exiftool_ver
	rm -rf "$work"
	mkdir -p "$work" "$msys2_root/bin" "$msys2_root/lib/perl5/site_perl"
	bsdtar -xf "$tarball" -C "$work" --strip-components 1
	cp "$work/exiftool" "$msys2_root/bin/exiftool"
	cp -R "$work/lib/." "$msys2_root/lib/perl5/site_perl/"
	echo "$exiftool_ver" > "$version_file"
	rm -rf "$work"
}

if [ -f "$msys2_root/bin/Qt6Gui.dll" ] && \
	[ -f "$msys2_root/bin/wperl.exe" ] && \
	[ -d "$msys2_root/lib/perl5" ]
then
	status "MSYS2 prefix already extracted"
else
	dbsync
	fetch $pkg-qt6-base $pkg-vulkan-loader $pkg-vulkan-headers \
		$pkg-libjpeg-turbo $pkg-libwebp $pkg-zlib \
		$pkg-libheif $pkg-libraw \
		$pkg-gcc-libs $pkg-perl $pkg-perl-win32-api
	# XML only; do not follow MSYS2 glib/python deps. Host u-m-d compiles it.
	mime=$(awk -F'\t' -v n="$pkg-shared-mime-info" \
		'$1 == n { print $2; exit }' db.tsv)
	echo "$mime" >> db.want
	status Fetching "$mime"
	[ -f "packages/$mime" ] || curl -#o "packages/$mime" "$repository/$mime"
	verify
	extract
fi
install_resvg
install_swiftshader
install_msvc_crt
install_exiftool
status Compiling MIME database
update-mime-database "$msys2_root/share/mime"

status Success
