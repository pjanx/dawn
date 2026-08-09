# Embed a SPIR-V binary as a C++ uint32_t array header.
# Usage: cmake -DINPUT=foo.spv -DOUTPUT=foo-spv.h -DSYMBOL=foo_spv -P embed-spirv.cmake

if(NOT INPUT OR NOT OUTPUT OR NOT SYMBOL)
	message(FATAL_ERROR "INPUT, OUTPUT, and SYMBOL required")
endif()

file(READ "${INPUT}" SPIRV_HEX HEX)
string(LENGTH "${SPIRV_HEX}" HEX_LEN)
math(EXPR WORD_COUNT "${HEX_LEN} / 8")

set(OUT "/* Auto-generated from ${INPUT} — do not edit. */\n")
string(APPEND OUT "#pragma once\n\n")
string(APPEND OUT "#include <cstdint>\n\n")
string(APPEND OUT "static constexpr uint32_t ${SYMBOL}[] = {\n")

set(I 0)
while(I LESS HEX_LEN)
	string(SUBSTRING "${SPIRV_HEX}" ${I} 8 WORD_HEX)
	# SPIR-V is little-endian in the file; HEX read is byte order as stored.
	string(SUBSTRING "${WORD_HEX}" 0 2 B0)
	string(SUBSTRING "${WORD_HEX}" 2 2 B1)
	string(SUBSTRING "${WORD_HEX}" 4 2 B2)
	string(SUBSTRING "${WORD_HEX}" 6 2 B3)
	string(APPEND OUT "\t0x${B3}${B2}${B1}${B0}u,\n")
	math(EXPR I "${I} + 8")
endwhile()

string(APPEND OUT "};\n")
string(APPEND OUT "static constexpr uint32_t ${SYMBOL}_words = ${WORD_COUNT}u;\n")

file(WRITE "${OUTPUT}" "${OUT}")
