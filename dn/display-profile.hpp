//
// display-profile.hpp: display ICC profile lookup
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QScreen;

namespace dn
{

/// An output's dynamic range, as far as its platform tells.
struct DisplayRange {
	bool hdr = false;    ///< An HDR display, whatever its current headroom
	float headroom = 1;  ///< Current, linear, where 1 is SDR white
	/// CIE 1931 xy of the output's primaries (R, G, B), then white.
	std::optional<std::array<double, 8>> primaries;
};

/// Windows Advanced Color, as DXGI and DisplayConfig report it.
struct AdvancedColor {
	/// In either its HDR or its wide-gamut SDR form.
	bool active = false;
	/// `hdr` is the HDR form; `primaries` come from DXGI.
	DisplayRange range;
	/// SDR white in scRGB, where 1.0 is 80 cd/m².
	float white = 1;
};

struct DisplayProfile {
	std::vector<unsigned char> icc;
	std::string source;
	std::string label;
	/// Only Windows fills this in.
	AdvancedColor advanced_color;
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

#ifdef __APPLE__
/// The screen's EDR range: an HDR display when its potential headroom
/// exceeds 1, and its current headroom.
DisplayRange macos_display_range(QScreen *screen);
/// Calls `fn` on every change of screen parameters, EDR ramps included,
/// for as long as the returned handle lives.
std::shared_ptr<void> macos_watch_screen_parameters(std::function<void()> fn);
#else
inline DisplayRange
macos_display_range(QScreen *)
{
	return {};
}
inline std::shared_ptr<void>
macos_watch_screen_parameters(std::function<void()>)
{
	return {};
}
#endif

#ifdef _WIN32
/// Only the Advanced Color part of the screen's DisplayProfile, cheaper.
AdvancedColor windows_advanced_color(QScreen *screen);
#else
inline AdvancedColor
windows_advanced_color(QScreen *)
{
	return {};
}
#endif

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
	void notify();

	std::vector<std::pair<void *, std::function<void()>>> listeners_;
	std::unique_ptr<DisplayProfileSource> source_;
	bool notify_pending_ = false;
};

}  // namespace dn
