#!/bin/sh -e
# msys2-package-msi.sh: build an MSI from a MinGW/MSYS2 cross-build
export LC_ALL=C
arch=$1 msi=$2 wxs=$3 description=$4
destdir=$PWD/package/${msi%.*}

# CMAKE_SYSTEM_PROCESSOR is x86_64 for a 64-bit MinGW cross-build.
[ "$arch" = "x86" ] || arch=x64

# Wine 11 has removed wine64, so this is only backwards compatibility.
if command -v wine64 >/dev/null
then wine() { command wine64 "$@"; }
fi

rm -rf "$destdir"
cmake --install . --prefix "$destdir"

txt2rtf() {
	LC_ALL=C.UTF-8 iconv -f utf-8 -t ascii//translit "$@" | awk 'BEGIN {
		print "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0 Tahoma;}}"
		print "\\f0\\fs24{\\pard\\sa240"
	} {
		gsub(/\\/, "\\\\"); gsub(/[{]/, "\\{"); gsub(/[}]/, "\\}")
		if (!$0) { print "\\par}{\\pard\\sa240"; prefix = "" }
		else { print prefix $0; prefix = " " }
	} END {
		print "\\par}}"
	}'
}

# msitools have this filename hardcoded in UI files, and it's required.
txt2rtf "$(dirname "$0")/LICENSE" > License.rtf

find "$destdir" -type f \
	| wixl-heat --prefix "$destdir/" --directory-ref INSTALLDIR \
		--component-group CG.dn --var var.SourceDir > package-files.wxs

# Only register extensions in Explorer's "Open with" list, never claim
# HKCR\.ext itself: Windows 8+ silently ignores default-app changes anyway,
# and doing it by hand here would just hijack the user's existing default.
cat <<XML > associations.wxs
<?xml version='1.0' encoding='utf-8'?>
<Wix xmlns='http://schemas.microsoft.com/wix/2006/wi'>
	<Fragment>
		<DirectoryRef Id='INSTALLDIR'>
			<Component Id='FileAssociations' Guid='*'>
				<RegistryKey Root='HKCR' Key='dawn.dn'>
					<RegistryValue Type='string' Value='$description' />
					<RegistryValue Type='string' Key='DefaultIcon'
						Value='[INSTALLDIR]dn.ico' />
					<RegistryValue Type='string' Key='shell\\open\\command'
						Value='"[INSTALLDIR]dn.exe" "%1"' />
				</RegistryKey>

$(wine "$destdir/dn.exe" --list-supported-extensions \
	| sed -n 's/^\*\(\.[[:alnum:]]\{1,\}\)\r\?$/\1/p' \
	| tr A-Z a-z \
	| sort -u \
	| while IFS= read -r ext
do cat <<END
				<RegistryKey Root='HKCR' Key='$ext\\OpenWithProgids'>
					<RegistryValue Type='string' Name='dawn.dn' Value='' />
				</RegistryKey>
END
done)
			</Component>
		</DirectoryRef>
	</Fragment>
</Wix>
XML

wixl --verbose --arch "$arch" -D SourceDir="$destdir" --ext ui \
	--output "$msi" "$wxs" package-files.wxs associations.wxs
