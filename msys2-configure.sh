#!/bin/sh -e
# msys2-configure.sh: set up an MSYS2-based build
#
# Dependencies: AWK, cURL, sha256sum (coreutils), bsdtar (libarchive), jq,
# cabextract, wine, rustc, cargo, rust-src, x86_64-w64-mingw32-gcc
repository=https://repo.msys2.org/mingw/ucrt64/
pkg=mingw-w64-ucrt-x86_64

# Wine 11 has removed wine64, so this is only backwards compatibility.
if command -v wine64 >/dev/null
then wine() { command wine64 "$@"; }
fi

status() {
	echo "$(tput bold)-- $*$(tput sgr0)" >&2
}

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

	# SwiftShader
	url=$(curl -L# https://github.com/jakoch/rasterizers/releases/latest/download/versions.json \
		| jq -r ".latest[\"swiftshader-win64\"].url")
	name=$(basename "$url")
	status Fetching "$name"
	[ -f "$name" ] || curl -#Lo "$name" "$url"
	ln -sf "$name" swiftshader.zip

	# Visual Studio runtime for SwiftShader
	url=https://aka.ms/vs/17/release/vc_redist.x64.exe
	sha256=cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b
	name=$(basename "$url")
	echo "$sha256 *packages/$name" >> db.sums
	status Fetching "$name"
	[ -f "$name" ] || curl -#Lo "$name" "$url"

	# ExifTool
	version=$(curl -# https://exiftool.org/ver.txt)
	name=exiftool-$version.tar.gz remotename=Image-ExifTool-$version.tar.gz
	status Fetching "$remotename"
	[ -f "$name" ] || curl -#Lo "$name" \
		"https://sourceforge.net/projects/exiftool/files/$remotename/download"
	ln -sf "$name" exiftool.tar.gz

	# resvg (unstable API, so pinning isn't a bad idea)
	url=https://github.com/linebender/resvg/releases/download/v0.48.1/resvg-0.48.1.tar.xz
	sha256=13ed5a2bae7a01156288ecae5bf944cf7d1c572742c19fc68027947a4d87294c
	name=$(basename "$url")
	echo "$sha256 *packages/$name" >> db.sums
	status Fetching "$name"
	[ -f "$name" ] || curl -#Lo "$name" "$url"
	ln -sf "$name" resvg.tar.xz
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

	# SwiftShader
	bsdtar -C bin -xf swiftshader.zip vk_swiftshader.dll vk_swiftshader_icd.json

	# Visual Studio runtime for SwiftShader
	mkdir -p tmp
	cabextract -d tmp -F a12 vc_redist.x64.exe
	cabextract -d tmp tmp/a12
	for n in msvcp140 vcruntime140 vcruntime140_1
	do mv tmp/$n.dll_amd64 bin/$n.dll
	done
	rm -rf tmp

	# ExifTool
	# ExifTool is quite massive (mainly in file count), not sure what to do
	#bsdtar -xf exiftool.tar.gz
	#mv Image-ExifTool-*/exiftool bin
	#mv Image-ExifTool-*/lib/* lib/perl5/site_perl
	#rm -rf Image-ExifTool-*
}

# Native rustc cannot load upstream rust-std; compile std from rust-src.
# TODO(p): Try to push this bullshit directly to MSYS2.
resvg() {
	status Building resvg
	mkdir -p tmp include/resvg bin lib/pkgconfig
	src=$PWD/tmp
	bsdtar -C $src -xf resvg.tar.xz --strip-components 1
	# Vendor config replaces crates-io; -Zbuild-std needs hashbrown etc.
	rm -f $src/.cargo/config

	unset RUSTC
	(cd "$src/crates/c-api" &&
		CARGO_TARGET_DIR=$src \
		CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc \
		CC_x86_64_pc_windows_gnu=x86_64-w64-mingw32-gcc \
		AR_x86_64_pc_windows_gnu=x86_64-w64-mingw32-gcc-ar \
		RUSTC_BOOTSTRAP=1 \
		cargo build --release --target x86_64-pc-windows-gnu -Zbuild-std)
	cp $src/crates/c-api/resvg.h include/resvg/resvg.h
	mv $src/x86_64-pc-windows-gnu/release/resvg.dll bin/resvg.dll
	mv $src/x86_64-pc-windows-gnu/release/libresvg.dll.a lib/libresvg.dll.a
	cat >lib/pkgconfig/resvg.pc <<-'EOF'
		prefix=/ucrt64
		exec_prefix=${prefix}
		libdir=${prefix}/lib
		includedir=${prefix}/include

		Name: resvg
		Description: SVG rendering library (C API)
		Version: 0
		Libs: -L${libdir} -lresvg
		Cflags: -I${includedir}/resvg
	EOF
	rm -rf tmp
}

forward() {
	echo "#!/bin/sh" >$2
	echo "WINEPATH=$PWD/bin wine $PWD/share/qt6/bin/$1.exe"' "$@"' >>$2
	chmod +x $2
}

configure() {
	status Configuring packages
	wine bin/update-mime-database.exe share/mime
	forward rcc autorcc
	forward moc automoc
}

# This directory name matches the prefix in .pc files, so we don't need to
# modify them (pkgconf has --prefix-variable, but CMake can't pass that option).
mkdir -p ucrt64
cd ucrt64

dbsync
fetch $pkg-qt6-base $pkg-vulkan-loader $pkg-vulkan-headers $pkg-libwebp \
	$pkg-libjpeg-turbo $pkg-libheif $pkg-libraw $pkg-zlib \
	$pkg-shared-mime-info $pkg-gcc-libs \
	#$pkg-perl $pkg-perl-win32-api
verify
extract
resvg
configure "$@"

cd ..
toolchain=submodules/liberty/cmake/toolchains/MinGW-w64-x64.cmake
cmake -DCMAKE_TOOLCHAIN_FILE=$toolchain \
	-DCMAKE_AUTOMOC_EXECUTABLE=$PWD/ucrt64/automoc \
	-DCMAKE_AUTORCC_EXECUTABLE=$PWD/ucrt64/autorcc "$@"
