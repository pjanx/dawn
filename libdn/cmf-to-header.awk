# Convert CIE 1931 CMF CSV (wavelength,X,Y,Z) to a C++ header.
# Usage: awk -f cmf-to-header.awk ciexyz31_1.csv cmf-cie1931-2deg-1nm.h

BEGIN {
	if (ARGC < 3) {
		print "usage: awk -f cmf-to-header.awk in.csv out.h" > "/dev/stderr"
		exit 1
	}
	infile = ARGV[1]
	outfile = ARGV[2]
	ARGV[2] = ""
	rows = 0
}

{
	gsub(/\r/, "")
}

/^#/ { next }
/^Wavelength/ { next }
NF == 0 { next }

{
	n = split($0, a, ",")
	if (n < 4)
		next
	if (rows == 0) {
		base = infile
		sub(/^.*\//, "", base)
		print "// Generated from " base > outfile
		print "constexpr double kCmf[][3] = {" > outfile
	}
	printf "\t{%s, %s, %s},\n", a[2], a[3], a[4] > outfile
	++rows
}

END {
	if (rows == 0) {
		print "cmf-to-header.awk: no data rows" > "/dev/stderr"
		exit 1
	}
	print "};" > outfile
	print "constexpr int kCmfN = int(sizeof(kCmf) / sizeof(kCmf[0]));" > outfile
	printf "static_assert(kCmfN == %d);\n", rows > outfile
}
