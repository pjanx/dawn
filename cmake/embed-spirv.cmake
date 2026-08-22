#
# embed-spirv.cmake: embed a SPIR-V binary as a C++ uint32_t array header
#
# Copyright The dawn Authors
# SPDX-License-Identifier: MPL-2.0
#
# Usage: cmake -DINPUT=foo.spv -DOUTPUT=foo-spv.h -DSYMBOL=foo_spv -P embed-spirv.cmake
#

if(NOT INPUT OR NOT OUTPUT OR NOT SYMBOL)
	message(FATAL_ERROR "INPUT, OUTPUT, and SYMBOL required")
endif()

file(READ "${INPUT}" spirv_hex HEX)
string(LENGTH "${spirv_hex}" hex_len)
math(EXPR word_count "${hex_len} / 8")

set(out "// Code generated from ${INPUT}. DO NOT EDIT.\n")
string(APPEND out [[
#pragma once

#include <cstdint>

]])
string(APPEND out "static constexpr uint32_t ${SYMBOL}[] = {\n")

set(i 0)
while(i LESS hex_len)
	string(SUBSTRING "${spirv_hex}" ${i} 8 word_hex)
	# SPIR-V is little-endian in the file; HEX read is byte order as stored.
	string(SUBSTRING "${word_hex}" 0 2 b0)
	string(SUBSTRING "${word_hex}" 2 2 b1)
	string(SUBSTRING "${word_hex}" 4 2 b2)
	string(SUBSTRING "${word_hex}" 6 2 b3)
	string(APPEND out "\t0x${b3}${b2}${b1}${b0}u,\n")
	math(EXPR i "${i} + 8")
endwhile()

string(APPEND out "};\n")
string(APPEND out "static constexpr uint32_t ${SYMBOL}_words = ${word_count}u;\n")

file(WRITE "${OUTPUT}" "${out}")
