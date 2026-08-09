//
// test.hpp: generic test harness
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <initializer_list>

namespace test
{

inline int failures;
inline const char *current = "setup";

inline void
fail(const char *format, ...)
{
	fprintf(stderr, "%s: ", current);
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
	++failures;
}

inline void
check(bool passed, const char *expression, const char *file, int line)
{
	if (!passed)
		fail("CHECK failed: %s (%s:%d)", expression, file, line);
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

}  // namespace test

#define CHECK(condition)                                                       \
	::test::check(bool(condition), #condition, __FILE__, __LINE__)
