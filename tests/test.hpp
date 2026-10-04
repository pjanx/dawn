//
// test.hpp: generic test harness
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <dawn-config.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <vector>

#ifdef DAWN_TEST_FIXTURES_DIR
#include <libdn/libdn.hpp>

#include <string>
#endif

namespace test
{

inline int failures;
inline const char *current = "setup";

DAWN_FORMAT(1, 2) inline void
fail(const char *format, ...)
{
	fprintf(stderr, "%s: ", current);
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
	failures++;
}

inline bool
check(bool passed, const char *expression, const char *file, int line)
{
	if (!passed)
		fail("CHECK failed: %s (%s:%d)", expression, file, line);
	return passed;
}

struct Case {
	const char *name;
	std::function<void()> run;
};

inline int
run(std::initializer_list<Case> cases)
{
	for (const Case &entry : cases) {
		current = entry.name;
		entry.run();
	}
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}

inline void
append_be16(std::vector<uint8_t> &o, uint16_t v)
{
	o.push_back(uint8_t(v >> 8));
	o.push_back(uint8_t(v));
}

inline void
append_be32(std::vector<uint8_t> &o, uint32_t v)
{
	append_be16(o, uint16_t(v >> 16));
	append_be16(o, uint16_t(v));
}

#ifdef DAWN_TEST_FIXTURES_DIR
inline std::vector<uint8_t>
fixture(const std::string &name)
{
	std::vector<uint8_t> data;
	dawn::Error error;
	if (!dawn::read_file(
			std::string(DAWN_TEST_FIXTURES_DIR) + "/" + name, &data, &error))
		fail("%s: %s", name.c_str(), error.message.c_str());
	return data;
}
#endif

}  // namespace test

#define CHECK(condition)                                                       \
	::test::check(bool(condition), #condition, __FILE__, __LINE__)
