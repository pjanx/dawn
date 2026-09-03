//
// app-menu-macos.mm: append to Qt Cocoa's QCocoaMenuLoader bar
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "app-menu-macos.hpp"

#include "action.hpp"
#include "app.hpp"
#include "url.hpp"
#include "window.hpp"

#include <QDir>
#include <QKeySequence>
#include <QString>

#import <AppKit/AppKit.h>

#include <span>
#include <vector>

using namespace std;

static NSEventModifierFlags
ns_mods(dn::Accel a)
{
	NSEventModifierFlags f = 0;
	if (a.mods & Qt::ControlModifier)
		f |= NSEventModifierFlagCommand;
	if (a.mods & Qt::AltModifier)
		f |= NSEventModifierFlagOption;
	if (a.mods & Qt::ShiftModifier)
		f |= NSEventModifierFlagShift;
	if (a.mods & Qt::MetaModifier)
		f |= NSEventModifierFlagControl;
	return f;
}

static NSString *
ns_equiv(dn::Accel a, NSEventModifierFlags *mods)
{
	*mods = ns_mods(a);
	const uint32_t k = a.key;
	if (!k)
		return @"";

	if (k >= Qt::Key_A && k <= Qt::Key_Z) {
		unichar c = unichar('a' + (k - Qt::Key_A));
		if (*mods & NSEventModifierFlagShift) {
			c = unichar('A' + (k - Qt::Key_A));
			*mods &= ~NSEventModifierFlagShift;
		}
		return [NSString stringWithCharacters:&c length:1];
	}
	if (k >= Qt::Key_0 && k <= Qt::Key_9) {
		unichar c = unichar('0' + (k - Qt::Key_0));
		return [NSString stringWithCharacters:&c length:1];
	}
	if (k >= Qt::Key_F1 && k <= Qt::Key_F12) {
		unichar c = unichar(NSF1FunctionKey + (k - Qt::Key_F1));
		return [NSString stringWithCharacters:&c length:1];
	}

	unichar c = 0;
	switch (k) {
	case Qt::Key_Return:
	case Qt::Key_Enter:
		c = '\r';
		break;
	case Qt::Key_Escape:
		c = '\033';
		break;
	case Qt::Key_Backspace:
		c = 0x7f;
		break;
	case Qt::Key_Tab:
		c = '\t';
		break;
	case Qt::Key_Left:
		c = NSLeftArrowFunctionKey;
		break;
	case Qt::Key_Right:
		c = NSRightArrowFunctionKey;
		break;
	case Qt::Key_Up:
		c = NSUpArrowFunctionKey;
		break;
	case Qt::Key_Down:
		c = NSDownArrowFunctionKey;
		break;
	case Qt::Key_Home:
		c = NSHomeFunctionKey;
		break;
	case Qt::Key_End:
		c = NSEndFunctionKey;
		break;
	case Qt::Key_PageUp:
		c = NSPageUpFunctionKey;
		break;
	case Qt::Key_PageDown:
		c = NSPageDownFunctionKey;
		break;
	case Qt::Key_Insert:
		c = NSInsertFunctionKey;
		break;
	case Qt::Key_Delete:
		c = NSDeleteFunctionKey;
		break;
	default:
		if (k >= 32 && k < 128)
			c = unichar(k);
		else
			return @"";
	}
	return [NSString stringWithCharacters:&c length:1];
}

static const dn::MenuNode *
find_section(span<const dn::MenuNode> tree, NSString *title)
{
	const QString want = QString::fromNSString(title);
	for (const dn::MenuNode &n : tree) {
		if (dn::menu_label(n.title, nullptr) == want)
			return &n;
	}
	return nullptr;
}

static bool
skip_action(dn::Action a)
{
	return a == dn::Action::Quit || a == dn::Action::About ||
		a == dn::Action::Settings || a == dn::Action::Fullscreen;
}

static void
sync_hidden(NSMenu *main, id delegate, span<const dn::MenuNode> tree)
{
	for (NSMenuItem *top in main.itemArray) {
		if (top.submenu.delegate != delegate)
			continue;
		top.hidden = find_section(tree, top.title) ? NO : YES;
	}
}

// Qt's Cocoa plugin builds the application menu, and owns the About and
// Settings items that we want pointed at ourselves.  It installs no public
// header for its menu loader, so redeclare just what we ask of it: the class
// name outlives item titles, which Qt both translates and has renamed.
@protocol DnCocoaMenuLoader <NSObject>
- (NSMenuItem *)aboutMenuItem;
- (NSMenuItem *)preferencesMenuItem;
@end

@interface DnMenuDelegate : NSObject <NSMenuDelegate>
@property(nonatomic, assign) dn::App *app;
@end

// Keep native key equivalents for their standard menu presentation, but let
// Qt deliver the actual key event through Dawn's normal shortcut path.
@interface DnMenu : NSMenu
@end

@implementation DnMenu

- (BOOL)performKeyEquivalent:(NSEvent *)event
{
	(void)event;
	return NO;
}

@end

@implementation DnMenuDelegate

- (dn::Window *)window
{
	return _app ? _app->key_window() : nullptr;
}

- (const dn::Actor *)actor
{
	if (dn::Window *w = [self window])
		return w->active_actor();
	return nullptr;
}

- (void)invoke:(NSMenuItem *)sender
{
	const dn::Action a = dn::Action(sender.tag);
	const dn::Actor *actor = [self actor];
	if (actor && actor->apply)
		actor->apply(a);
	else if (dn::Window *w = [self window]) {
		if (w->host().apply)
			w->host().apply(a);
	} else if (a == dn::Action::NewWindow && _app)
		_app->open(dn::path_to_url(QDir::currentPath()));
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
	const dn::Action tag = dn::Action(item.tag);
	if (tag == dn::Action::About || tag == dn::Action::Settings)
		return [self window] != nullptr;
	const dn::Actor *actor = [self actor];
	if (!actor || !actor->enabled)
		return YES;
	return actor->enabled(dn::Action(item.tag));
}

- (void)menuNeedsUpdate:(NSMenu *)menu
{
	dn::Window *w = [self window];
	const span<const dn::MenuNode> tree =
		w ? w->active_menu() : span<const dn::MenuNode>{};
	const dn::Actor *actor = w ? w->active_actor() : nullptr;
	sync_hidden([NSApp mainMenu], self, tree);

	const dn::MenuNode *node = find_section(tree, menu.title);
	[menu removeAllItems];
	if (!node)
		return;

	bool pending_sep = false;
	bool any = false;
	for (const dn::MenuNode &n : node->items) {
		if (!n.title && n.action == dn::Action::None) {
			pending_sep = any;
			continue;
		}
		if (n.action == dn::Action::None || skip_action(n.action))
			continue;
		if (pending_sep) {
			[menu addItem:[NSMenuItem separatorItem]];
			pending_sep = false;
		}
		const dn::ActionDef &def = dn::action_def(n.action);
		const bool checked =
			actor && actor->checked && actor->checked(n.action);
		const QString title =
			dn::menu_label(dn::action_label(def, checked), nullptr);
		NSString *key = @"";
		NSEventModifierFlags mods = 0;
		if (!(def.accel && def.keys[0].key == 0))
			key = ns_equiv(def.keys[0], &mods);
		NSMenuItem *it = [[[NSMenuItem alloc] initWithTitle:title.toNSString()
													 action:@selector(invoke:)
											  keyEquivalent:key] autorelease];
		it.tag = NSInteger(n.action);
		it.target = self;
		it.keyEquivalentModifierMask = mods;
		const bool on =
			(def.flags & dn::ActionToggle) && !def.label[1] && checked;
		it.state = on ? NSControlStateValueOn : NSControlStateValueOff;
		[menu addItem:it];
		any = true;
	}
}

@end

namespace dn
{

static DnMenuDelegate *g_menu_delegate;

static bool
has_menu(NSMenu *main, NSString *title)
{
	for (NSMenuItem *it in main.itemArray) {
		if ([it.title isEqualToString:title])
			return true;
	}
	return false;
}

static void
add_menu(NSMenu *main, NSString *title, id delegate)
{
	if (has_menu(main, title))
		return;
	NSMenuItem *top = [[[NSMenuItem alloc] initWithTitle:title
												  action:nil
										   keyEquivalent:@""] autorelease];
	NSMenu *sub = [[[DnMenu alloc] initWithTitle:title] autorelease];
	sub.delegate = delegate;
	top.submenu = sub;
	[main addItem:top];
}

// Qt hides and disables the items it has found nothing to merge into.
static void
claim_item(NSMenuItem *item, Action action, id target, NSString *key,
	NSEventModifierFlags mods)
{
	if (!item)
		return;

	item.hidden = NO;
	item.enabled = YES;
	item.tag = NSInteger(action);
	item.target = target;
	item.action = @selector(invoke:);
	item.keyEquivalent = key;
	item.keyEquivalentModifierMask = mods;
}

// macOS usually keeps About and Settings in the application menu.
static void
adopt_app_menu(id target)
{
	id cls = NSClassFromString(@"QCocoaMenuLoader");
	if (![cls respondsToSelector:@selector(sharedMenuLoader)])
		return;

	id<DnCocoaMenuLoader> loader =
		[cls performSelector:@selector(sharedMenuLoader)];
	if ([loader respondsToSelector:@selector(aboutMenuItem)])
		claim_item([loader aboutMenuItem], Action::About, target, @"", 0);
	if ([loader respondsToSelector:@selector(preferencesMenuItem)])
		claim_item([loader preferencesMenuItem], Action::Settings, target,
			@",", NSEventModifierFlagCommand);
}

void
sync_macos_app_menu(App *app)
{
	NSMenu *main = [NSApp mainMenu];
	if (!main || !app || !g_menu_delegate)
		return;
	g_menu_delegate.app = app;

	if (Window *w = app->key_window())
		sync_hidden(main, g_menu_delegate, w->active_menu());
}

void
install_macos_app_menu(App *app)
{
	NSMenu *main = [NSApp mainMenu];
	if (!main || !app)
		return;

	if (!g_menu_delegate)
		g_menu_delegate = [[DnMenuDelegate alloc] init];
	DnMenuDelegate *delegate = g_menu_delegate;
	delegate.app = app;

	adopt_app_menu(delegate);
	vector<QString> titles;
	auto consider = [&](span<const MenuNode> tree) {
		for (const MenuNode &n : tree) {
			const QString t = menu_label(n.title, nullptr);
			if (t.isEmpty())
				continue;

			bool seen = false;
			for (const QString &e : titles) {
				if (e == t) {
					seen = true;
					break;
				}
			}
			if (!seen)
				titles.push_back(t);
		}
	};
	consider(viewer_menu());
	consider(browser_menu());
	for (const QString &t : titles)
		add_menu(main, t.toNSString(), delegate);
}

}  // namespace dn
