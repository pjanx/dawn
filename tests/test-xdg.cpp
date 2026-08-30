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

namespace
{

void
test_mime_globs()
{
	const vector<QString> globs = dn::extract_mime_globs({"image/jpeg"});
	CHECK(find(globs.begin(), globs.end(), QLatin1String("*.jpg")) !=
		globs.end());
	CHECK(find(globs.begin(), globs.end(), QLatin1String("*.jpeg")) !=
		globs.end());
}

void
test_types_for_filename()
{
	const vector<QString> types =
		dn::types_for_filename(QStringLiteral("photo.jpg"));
	CHECK(find(types.begin(), types.end(), QLatin1String("image/jpeg")) !=
		types.end());
}

}  // namespace

int
main()
{
	return test::run({
		{"MIME globs", test_mime_globs},
		{"filename types", test_types_for_filename},
	});
}
