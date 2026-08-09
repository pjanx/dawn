# lxdrgen-cpp.awk: C++20 LibertyXDR backend for lxdrgen.awk.
#
# Copyright The dawn Authors
# SPDX-License-Identifier: MPL-2.0
#
# Usage: env LC_ALL=C awk -f lxdrgen.awk -f lxdrgen-cpp.awk \
#  -v PrefixCamel=Instance -v Namespace=dn::ipc::instance \
#  common.lxdr instance.lxdr
#
# PrefixCamel is accepted for consistency with other backends and is not
# applied to C++ type names.  IDL names are emitted as-is.

function define_internal(name, ctype, view) {
	Types[name] = "internal"
	CodegenCType[name] = ctype
	CodegenViewType[name] = view == "" ? ctype : view
}

function define_int(shortname, ctype) {
	define_internal(shortname, ctype)
	CodegenSerialize[shortname] = "\tencoder." shortname "(%s);\n"
	CodegenDeserialize[shortname] = \
		"\tif (!decoder." shortname "(%s))\n" \
		"\t\treturn false;\n"
}

function define_sint(size) { define_int("i" size, "int" size "_t") }
function define_uint(size) { define_int("u" size, "uint" size "_t") }

function is_scalar(type) {
	return (Types[type] == "internal" && type != "string") || \
		Types[type] == "enum"
}

function field_decl(ctype, name, scalar) {
	if (scalar)
		return "\t" ctype " " name "{};\n"
	return "\t" ctype " " name ";\n"
}

function owning_array_type(type) {
	if (type == "u8")
		return "std::vector<std::byte>"
	if (type == "i8")
		return "std::vector<int8_t>"
	if (type == "string")
		return "std::vector<std::string>"
	return "std::vector<" CodegenCType[type] ">"
}

function view_array_type(type) {
	if (type == "u8")
		return "std::span<const std::byte>"
	if (type == "i8")
		return "std::span<const int8_t>"
	if (type == "string")
		return "std::vector<std::string_view>"
	return "std::vector<" CodegenViewType[type] ">"
}

function array_encode(f, type,    ser) {
	ser = "\tencoder.u32(static_cast<uint32_t>(" f ".size()));\n"
	if (type == "u8")
		return ser "\tencoder.bytes(" f ");\n"
	if (type == "i8")
		return ser "\tencoder.i8s(" f ");\n"
	return ser \
		"\tfor (size_t i = 0; i < " f ".size(); ++i)\n" \
		indent(sprintf(CodegenSerialize[type], f "[i]"))
}

function array_decode(f, type,    block, vt) {
	block = \
		"\t{\n" \
		"\t\tuint32_t n{};\n" \
		"\t\tif (!decoder.u32(n))\n" \
		"\t\t\treturn false;\n" \
		"\t\tsize_t const count = n;\n" \
		"\t\tif (count > decoder.remaining() ||\n" \
		"\t\t    count > dn::ipc::Decoder::kMaxElements)\n" \
		"\t\t\treturn false;\n"
	if (type == "u8")
		return block \
			"\t\tif (!decoder.bytes(" f ", count))\n" \
			"\t\t\treturn false;\n" \
			"\t}\n"
	if (type == "i8")
		return block \
			"\t\tif (!decoder.i8s(" f ", count))\n" \
			"\t\t\treturn false;\n" \
			"\t}\n"
	vt = CodegenViewType[type]
	return block \
		"\t\t" f ".reserve(std::min(count, size_t{4096}));\n" \
		"\t\tfor (size_t i = 0; i < count; ++i) {\n" \
		"\t\t\t" vt " elem{};\n" \
		indent(indent(sprintf(CodegenDeserialize[type], "elem"))) \
		"\t\t\t" f ".push_back(elem);\n" \
		"\t\t}\n" \
		"\t}\n"
}

function basename(p) {
	while (match(p, /\//))
		p = substr(p, RSTART + 1)
	return p
}

function emit_preamble(    i, src) {
	src = ""
	for (i = 1; i < ARGC; i++) {
		if (src != "")
			src = src " "
		src = src basename(ARGV[i])
	}
	if (src == "")
		src = basename(FILENAME)
	print "// Code generated from " src ". DO NOT EDIT."
	print "#pragma once"
	print ""
	print "#include <algorithm>"
	print "#include <cstddef>"
	print "#include <cstdint>"
	print "#include <span>"
	print "#include <string>"
	print "#include <string_view>"
	print "#include <variant>"
	print "#include <vector>"
	print ""
	print "namespace dn::ipc {"
	print "class Encoder;"
	print "class Decoder;"
	print "}"
	print ""
	print "namespace " Namespace " {"
}

BEGIN {
	if (Namespace == "")
		Namespace = "dn::ipc"
}

function codegen_begin() {
	define_sint("8")
	define_sint("16")
	define_sint("32")
	define_sint("64")
	define_uint("8")
	define_uint("16")
	define_uint("32")
	define_uint("64")

	define_internal("string", "std::string", "std::string_view")
	CodegenSerialize["string"] = "\tencoder.string(%s);\n"
	CodegenDeserialize["string"] = \
		"\tif (!decoder.string(%s))\n" \
		"\t\treturn false;\n"

	define_internal("bool", "bool")
	CodegenSerialize["bool"] = "\tencoder.boolean(%s);\n"
	CodegenDeserialize["bool"] = \
		"\tif (!decoder.boolean(%s))\n" \
		"\t\treturn false;\n"

	if (CodegenStarted)
		return
	CodegenStarted = 1
	emit_preamble()
}

function codegen_constant(name, value) {
	print ""
	print "inline constexpr int32_t k" snaketocamel(name) " = " value ";"
}

function codegen_enum_value(name, subname, value, cg) {
	append(cg, "fields",
		"\t" snaketocamel(subname) " = " value ",\n")
	append(cg, "cases",
		"\tcase static_cast<int8_t>(" name "::" \
			snaketocamel(subname) "):\n" \
		"\t\tvalue = " name "::" snaketocamel(subname) ";\n" \
		"\t\treturn true;\n")
}

function codegen_enum(name, cg,    fields) {
	fields = cg["fields"]
	sub(/,\n$/, "\n", fields)

	print ""
	print "enum class " name " : int8_t {"
	printf "%s", fields
	print "};"

	print ""
	print "inline void encode(const " name \
		" &value, dn::ipc::Encoder &encoder)"
	print "{"
	print "\tencoder.i8(static_cast<int8_t>(value));"
	print "}"

	print ""
	print "inline bool decode(dn::ipc::Decoder &decoder, " name " &value)"
	print "{"
	print "\tint8_t raw{};"
	print "\tif (!decoder.i8(raw) || raw == 0)"
	print "\t\treturn false;"
	print "\tswitch (raw) {"
	printf "%s", cg["cases"]
	print "\tdefault:"
	print "\t\treturn false;"
	print "\t}"
	print "}"

	CodegenSerialize[name] = "\tencode(%s, encoder);\n"
	CodegenDeserialize[name] = \
		"\tif (!decode(decoder, %s))\n" \
		"\t\treturn false;\n"
	CodegenCType[name] = name
	CodegenViewType[name] = name
	for (i in cg)
		delete cg[i]
}

function codegen_struct_tag(d, cg) {
	# C puts the tag on each arm struct.  C++ does not.
}

function codegen_struct_field(d, cg,    f) {
	f = "value." d["name"]
	if (!d["isarray"]) {
		append(cg, "fields",
			field_decl(CodegenCType[d["type"]], d["name"],
				is_scalar(d["type"])))
		append(cg, "vfields",
			field_decl(CodegenViewType[d["type"]], d["name"],
				is_scalar(d["type"])))
		append(cg, "serialize",
			sprintf(CodegenSerialize[d["type"]], f))
		append(cg, "deserialize",
			sprintf(CodegenDeserialize[d["type"]], f))
		return
	}

	append(cg, "fields",
		field_decl(owning_array_type(d["type"]), d["name"], 0))
	append(cg, "vfields",
		field_decl(view_array_type(d["type"]), d["name"], 0))
	append(cg, "serialize", array_encode(f, d["type"]))
	append(cg, "deserialize", array_decode(f, d["type"]))
}

function emit_codec(own, view, ser, des) {
	print ""
	if (ser) {
		print "inline void encode(const " own \
			" &value, dn::ipc::Encoder &encoder)"
		print "{"
		printf "%s", ser
		print "}"
	} else {
		print "inline void encode(const " own \
			" &, dn::ipc::Encoder &)"
		print "{"
		print "}"
	}

	print ""
	if (des) {
		print "inline bool decode(dn::ipc::Decoder &decoder, " \
			view " &value)"
		print "{"
		printf "%s", des
		print "\treturn true;"
		print "}"
	} else {
		print "inline bool decode(dn::ipc::Decoder &, " view " &)"
		print "{"
		print "\treturn true;"
		print "}"
	}
}

function codegen_struct(name, cg,    view) {
	view = name "View"

	print ""
	print "struct " name " {"
	printf "%s", cg["fields"]
	print "};"

	print ""
	print "struct " view " {"
	printf "%s", cg["vfields"]
	print "};"

	emit_codec(name, view, cg["serialize"], cg["deserialize"])

	CodegenSerialize[name] = "\tencode(%s, encoder);\n"
	CodegenDeserialize[name] = \
		"\tif (!decode(decoder, %s))\n" \
		"\t\treturn false;\n"
	CodegenCType[name] = name
	CodegenViewType[name] = view
	for (i in cg)
		delete cg[i]
}

function codegen_union_tag(d, cg) {
	cg["tagtype"] = d["type"]
	cg["tagname"] = d["name"]
}

function codegen_union_struct(name, casename, cg, scg,    n, arm) {
	arm = name snaketocamel(casename)
	codegen_struct(arm, scg)

	n = cg["n"] + 0
	cg["n"] = n + 1
	cg["own", n] = arm
	cg["view", n] = arm "View"
	cg["case", n] = snaketocamel(casename)
}

function codegen_union(name, cg,    view, n, i, own_list, view_list, tag) {
	view = name "View"
	n = cg["n"] + 0
	tag = cg["tagtype"]
	own_list = ""
	view_list = ""
	for (i = 0; i < n; i++) {
		if (i) {
			own_list = own_list ", "
			view_list = view_list ", "
		}
		own_list = own_list cg["own", i]
		view_list = view_list cg["view", i]
	}

	print ""
	print "struct " name " {"
	print "\tstd::variant<" own_list "> value;"
	print "};"

	print ""
	print "struct " view " {"
	print "\tstd::variant<" view_list "> value;"
	print "};"

	print ""
	print "inline void encode(const " name \
		" &value, dn::ipc::Encoder &encoder)"
	print "{"
	print "\tswitch (value.value.index()) {"
	for (i = 0; i < n; i++) {
		print "\tcase " i ":"
		print "\t\tencode(" tag "::" cg["case", i] ", encoder);"
		print "\t\tencode(std::get<" i ">(value.value), encoder);"
		print "\t\tbreak;"
	}
	print "\tdefault:"
	print "\t\tbreak;"
	print "\t}"
	print "}"

	print ""
	print "inline bool decode(dn::ipc::Decoder &decoder, " view " &value)"
	print "{"
	print "\tint8_t tag{};"
	print "\tif (!decoder.i8(tag) || tag == 0)"
	print "\t\treturn false;"
	print "\tswitch (tag) {"
	for (i = 0; i < n; i++) {
		print "\tcase static_cast<int8_t>(" tag "::" \
			cg["case", i] "): {"
		print "\t\t" cg["view", i] " arm{};"
		print "\t\tif (!decode(decoder, arm))"
		print "\t\t\treturn false;"
		print "\t\tvalue.value = arm;"
		print "\t\treturn true;"
		print "\t}"
	}
	print "\tdefault:"
	print "\t\treturn false;"
	print "\t}"
	print "}"

	CodegenSerialize[name] = "\tencode(%s, encoder);\n"
	CodegenDeserialize[name] = \
		"\tif (!decode(decoder, %s))\n" \
		"\t\treturn false;\n"
	CodegenCType[name] = name
	CodegenViewType[name] = view
	for (i in cg)
		delete cg[i]
}

END {
	if (!CodegenStarted)
		exit
	print ""
	print "}  // namespace " Namespace
}
