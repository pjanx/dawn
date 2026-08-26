//
// test-xdg.cpp: extract_mime_globs against shared-mime-info
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "xdg.hpp"

#include <cstdio>

using namespace std;

int
main()
{
	const vector<QString> globs = dn::extract_mime_globs({"image/jpeg"});
	bool jpg = false;
	bool jpeg = false;
	for (const QString &g : globs) {
		if (g == QLatin1String("*.jpg"))
			jpg = true;
		if (g == QLatin1String("*.jpeg"))
			jpeg = true;
	}
	if (!jpg || !jpeg) {
		fprintf(stderr,
			"extract_mime_globs(image/jpeg) missing *.jpg / *.jpeg (%d "
			"globs)\n",
			int(globs.size()));
		for (const QString &g : globs)
			fprintf(stderr, "  %s\n", qUtf8Printable(g));
		return 1;
	}

	const vector<QString> types =
		dn::types_for_filename(QStringLiteral("photo.jpg"));
	bool jpeg_type = false;
	for (const QString &t : types) {
		if (t == QLatin1String("image/jpeg"))
			jpeg_type = true;
	}
	if (!jpeg_type) {
		fprintf(stderr, "types_for_filename(photo.jpg) missing image/jpeg\n");
		for (const QString &t : types)
			fprintf(stderr, "  %s\n", qUtf8Printable(t));
		return 1;
	}
	return 0;
}
