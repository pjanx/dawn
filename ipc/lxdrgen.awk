# lxdrgen.awk: an XDR-derived code generator for network protocols.
#
# Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# Usage: env LC_ALL=C awk -f lxdrgen.awk -f lxdrgen-{c,go,mjs}.awk \
#  -v PrefixCamel=Foo foo.lxdr > foo.{c,go,mjs} | {clang-format,gofmt,...}

# --- Utilities ----------------------------------------------------------------

function cameltosnake(s) {
	while (match(s, /[[:lower:]][[:upper:]]/)) {
		s = substr(s, 1, RSTART) "_" \
			tolower(substr(s, RSTART + 1, RLENGTH - 1)) \
			substr(s, RSTART + RLENGTH)
	}
	return tolower(s)
}

function snaketocamel(s) {
	s = toupper(substr(s, 1, 1)) tolower(substr(s, 2))
	while (match(s, /_[[:alnum:]]/)) {
		s = substr(s, 1, RSTART - 1) \
			toupper(substr(s, RSTART + 1, RLENGTH - 1)) \
			substr(s, RSTART + RLENGTH)
	}
	return s
}

function decapitalize(s) {
	if (match(s, /[[:upper:]][[:lower:]]/)) {
		return tolower(substr(s, 1, 1)) substr(s, 2)
	}
	return s
}

function indent(s) {
	if (!s)
		return s

	gsub(/\n/, "\n\t", s)
	sub(/\t*$/, "", s)
	return "\t" s
}

function append(a, key, value) {
	a[key] = a[key] value
}

# --- Parsing ------------------------------------------------------------------

function fatal(message) {
	print "// " FILENAME ":" FNR ": fatal error: " message
	print FILENAME ":" FNR ": fatal error: " message > "/dev/stderr"
	exit 1
}

function skipcomment() {
	do {
		if (match($0, /[*]\//)) {
			$0 = substr($0, RSTART + RLENGTH)
			return
		}
	} while (getline > 0)
	fatal("unterminated block comment")
}

function nexttoken() {
	do {
		if (match($0, /^[[:space:]]+/)) {
			$0 = substr($0, RLENGTH + 1)
		} else if (match($0, /^\/\/.*/)) {
			$0 = ""
		} else if (match($0, /^\/[*]/)) {
			$0 = substr($0, RLENGTH + 1)
			skipcomment()
		} else if (match($0, /^[[:alpha:]][[:alnum:]_]*/)) {
			Token = substr($0, 1, RLENGTH)
			$0 = substr($0, RLENGTH + 1)
			return Token
		# AWK implementations rarely support non-decimal notations
		# in their implicit string-to-number conversions.
		} else if (match($0, /^(0|-?[1-9][0-9]*)/)) {
			Token = substr($0, 1, RLENGTH)
			$0 = substr($0, RLENGTH + 1)
			return Token
		} else if ($0) {
			Token = substr($0, 1, 1)
			$0 = substr($0, 2)
			return Token
		}
	} while ($0 || getline > 0)
	Token = ""
	return Token
}

function expect(v) {
	if (!v)
		fatal("broken expectations at `" Token "' before `" $0 "'")
	return v
}

function accept(what) {
	if (Token != what)
		return 0
	nexttoken()
	return 1
}

function identifier(    v) {
	if (Token !~ /^[[:alpha:]]/)
		return 0
	v = Token
	nexttoken()
	return v
}

function number(    v) {
	if (Token !~ /^(0|-?[1-9])/)
		return 0
	v = Token
	nexttoken()
	return v
}

function readnumber(    ident) {
	ident = identifier()
	if (!ident)
		return expect(number())
	if (!(ident in Consts))
		fatal("unknown constant: " ident)
	return Consts[ident]
}

function defconst(    ident, num) {
	if (!accept("const"))
		return 0

	ident = expect(identifier())
	expect(accept("="))
	num = readnumber()
	if (ident in Consts)
		fatal("constant redefined: " ident)

	Consts[ident] = num
	codegen_constant(ident, num)
	return 1
}

function readtype(    ident) {
	ident = deftype()
	if (ident)
		return ident

	ident = identifier()
	if (!ident)
		return 0

	if (!(ident in Types))
		fatal("unknown type: " ident)
	return ident
}

function defenum(    name, ident, value, cg) {
	delete cg[0]

	name = expect(identifier())
	expect(accept("{"))
	while (!accept("}")) {
		ident = expect(identifier())
		value = value + 1
		if (accept("="))
			value = readnumber() + 0
		if (!value)
			fatal("enumeration values cannot be zero")
		if (value < -128 || value > 127)
			fatal("enumeration value out of range")
		expect(accept(","))
		append(EnumValues, name, SUBSEP ident)
		if (EnumValues[name, ident]++)
			fatal("duplicate enum value: " ident)
		codegen_enum_value(name, ident, value, cg)
	}

	Types[name] = "enum"
	codegen_enum(name, cg)
	return name
}

function readfield(out,    nonvoid) {
	nonvoid = !accept("void")
	if (nonvoid) {
		out["type"] = expect(readtype())
		out["name"] = expect(identifier())
		# TODO: Consider supporting XDR's VLA length limits here.
		# TODO: Consider supporting XDR's fixed-length syntax for string limits.
		out["isarray"] = accept("<") && expect(accept(">"))
	}
	expect(accept(";"))
	return nonvoid
}

function defstruct(    name, d, cg) {
	delete d[0]
	delete cg[0]

	name = expect(identifier())
	expect(accept("{"))
	while (!accept("}")) {
		if (readfield(d))
			codegen_struct_field(d, cg)
	}

	Types[name] = "struct"
	codegen_struct(name, cg)
	return name
}

function defunion(    name, tag, tagtype, tagvalue, cg, scg, d, a, i, unseen) {
	delete cg[0]
	delete scg[0]
	delete d[0]

	name = expect(identifier())
	expect(accept("switch"))
	expect(accept("("))
	tag["type"] = tagtype = expect(readtype())
	tag["name"] = expect(identifier())
	expect(accept(")"))

	if (Types[tagtype] != "enum")
		fatal("not an enum type: " tagtype)
	codegen_union_tag(tag, cg)

	split(EnumValues[tagtype], a, SUBSEP)
	for (i in a)
		unseen[a[i]]++

	expect(accept("{"))
	while (!accept("}")) {
		if (accept("case")) {
			if (tagvalue)
				codegen_union_struct(name, tagvalue, cg, scg)

			tagvalue = expect(identifier())
			expect(accept(":"))
			if (!unseen[tagvalue]--)
				fatal("no such value or duplicate case: " tagtype "." tagvalue)
			codegen_struct_tag(tag, scg)
		} else if (tagvalue) {
			if (readfield(d))
				codegen_struct_field(d, scg)
		} else {
			fatal("union fields must fall under a case")
		}
	}
	if (tagvalue)
		codegen_union_struct(name, tagvalue, cg, scg)

	# What remains non-zero in unseen[2..] is simply not recognized/allowed.
	Types[name] = "union"
	codegen_union(name, cg)
	return name
}

function deftype() {
	if (accept("enum"))
		return defenum()
	if (accept("struct"))
		return defstruct()
	if (accept("union"))
		return defunion()
	return 0
}

{
	if (PrefixCamel) {
		PrefixLower = tolower(cameltosnake(PrefixCamel)) "_"
		PrefixUpper = toupper(cameltosnake(PrefixCamel)) "_"
	}

	# This is not in a BEGIN clause (even though it consumes all input),
	# so that the code generator can insert the first FILENAME.
	codegen_begin()

	nexttoken()
	while (Token != "") {
		expect(defconst() || deftype())
		expect(accept(";"))
	}
}
