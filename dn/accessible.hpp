//
// accessible.hpp: platform accessibility over the Kit widget tree
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit.hpp"

namespace dn
{

class Window;

// Installs the interface factory.  Call before the first window exists: a
// window Qt has already asked about would keep whatever it answered then.
void accessible_init();

// Kit's semantic change channel, as Window binds it.
void accessible_changed(Window *window, Change what, Widget *w);

// The window's page tree is about to be destroyed wholesale.  Retires every
// interface registered for it, while the widgets are still there.
void accessible_retire_page(Window *window);

// The window itself is going, after its pages already have.
void accessible_forget_window(Window *window);

// The window title changed, and with it the name of its semantic root.
void accessible_renamed(Window *window);

// The window gained or lost the keyboard.  Focus that outlives activation
// is not focus, so whatever holds it says so again either way.
void accessible_activated(Window *window);

}  // namespace dn
