//
// window-appearance-windows.cpp: native window behaviour Qt does not cover
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "window-appearance.hpp"

#include <QWindow>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>

using namespace std;

namespace dn
{

// DWMWA_USE_IMMERSIVE_DARK_MODE is 20 from Windows 10 20H1.  1809 used 19.
constexpr DWORD kImmersiveDarkMode = 20;
constexpr DWORD kImmersiveDarkMode1809 = 19;

void
sync_window_appearance(QWindow *window, bool dark)
{
	if (!window)
		return;

	HWND hwnd = (HWND)(window->winId());
	if (!hwnd)
		return;

	const BOOL value = dark ? TRUE : FALSE;
	if (FAILED(DwmSetWindowAttribute(
			hwnd, kImmersiveDarkMode, &value, sizeof value)))
		DwmSetWindowAttribute(
			hwnd, kImmersiveDarkMode1809, &value, sizeof value);
	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
			SWP_FRAMECHANGED);
}

}  // namespace dn
