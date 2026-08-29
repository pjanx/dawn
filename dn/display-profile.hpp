//
// display-profile.hpp: display ICC profile lookup
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QScreen;

namespace dn
{

struct DisplayProfile {
	std::vector<unsigned char> icc;
	std::string source;
	std::string label;
};

/// Platform lookup and change notification for display profiles.
class DisplayProfileSource
{
public:
	virtual ~DisplayProfileSource() = default;
	virtual void start(std::function<void()> on_change) = 0;
	virtual DisplayProfile load(QScreen *screen) = 0;
};

std::unique_ptr<DisplayProfileSource> make_display_profile_source();

/// Process-wide display ICC lookup and change notification.
class DisplayProfileWatch
{
public:
	DisplayProfileWatch();
	~DisplayProfileWatch();
	DisplayProfileWatch(const DisplayProfileWatch &) = delete;
	DisplayProfileWatch &operator=(const DisplayProfileWatch &) = delete;

	void start();
	DisplayProfile load(QScreen *screen);
	void listen(void *key, std::function<void()> fn);
	void unlisten(const void *key);

private:
	void notify() const;

	std::vector<std::pair<void *, std::function<void()>>> listeners_;
	std::unique_ptr<DisplayProfileSource> source_;
};

}  // namespace dn
