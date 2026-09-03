//
// test-xdg.cpp: extract_mime_globs against shared-mime-info
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "test.hpp"
#include "xdg.hpp"

#include <algorithm>

using namespace std;

static void
test_mime_globs()
{
	const vector<QString> globs = dn::extract_mime_globs({"image/jpeg"});
	CHECK(find(globs.begin(), globs.end(), QLatin1String("*.jpg")) !=
		globs.end());
	CHECK(find(globs.begin(), globs.end(), QLatin1String("*.jpeg")) !=
		globs.end());
}

int
main()
{
	return test::run({
		{"MIME globs", test_mime_globs},
	});
}
