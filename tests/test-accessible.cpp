//
// test-accessible.cpp: what an AT-SPI client can do with a running dn
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//
// This proves the protocol, and only the protocol.  Whether a screen reader
// says anything useful of it, or a hint overlay puts its chips where the
// compositor put the window, has to be seen by a person.
//
// Run through tests/run-accessible.sh, which provides the private session
// that everything below assumes.

#include "test.hpp"

#include <atspi/atspi.h>

// Not self-contained: this one needs the umbrella header above, so it has to
// sit in a block of its own.  Sorted together, "atspi-object.h" would come
// first, and nothing would compile.
#include <atspi/atspi-object.h>
#include <glib.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace std;

namespace
{

// Bound each failed step so a broken prerequisite does not stall the suite.
constexpr int kTimeoutMs = 3000;
constexpr int kPollMs = 25;

const char *g_dn = nullptr;
const char *g_dir = nullptr;
pid_t g_pid = -1;
AtspiAccessible *g_app = nullptr;
AtspiAccessible *g_window = nullptr;
// Set from the event listener, which is registered before the action that
// should produce it: state read back without one proves only that the state
// changed, not that anybody was ever told.
bool g_saw_checked_event = false;
bool g_saw_children_event = false;
int g_children_adds = 0;
int g_children_removes = 0;
bool g_saw_selection_event = false;
bool g_saw_selected_event = false;
bool g_saw_focus_event = false;
string g_selection_role;
string g_focus_role;
bool g_saw_text_event = false;
bool g_saw_caret_event = false;
string g_text_source_path;

struct TextChange {
	string kind;
	int offset = 0;
	int length = 0;
	string text;
};
vector<TextChange> g_text_changes;
vector<string> g_remove_roles;

}  // namespace

// --- Waiting -----------------------------------------------------------------

static gboolean
expire(gpointer data)
{
	*(bool *) data = true;
	return G_SOURCE_REMOVE;
}

static gboolean
poll_tick(gpointer)
{
	return G_SOURCE_CONTINUE;
}

// A deadline, never a sleep: the bus is pumped until the predicate holds, and
// the caller hears about it the moment it does.  The tick is there to wake
// the loop, so that state nobody announces still gets looked at again.
static bool
wait_until(const function<bool()> &done)
{
	if (done())
		return true;

	bool expired = false;
	const guint deadline = g_timeout_add(guint(kTimeoutMs), expire, &expired);
	const guint tick = g_timeout_add(guint(kPollMs), poll_tick, nullptr);
	while (!done() && !expired)
		g_main_context_iteration(nullptr, TRUE);
	if (!expired)
		g_source_remove(deadline);
	g_source_remove(tick);
	return done();
}

// The inverse of a deadline wait: the predicate must keep holding while the
// bus is pumped, so a caret that must not move is not read once and believed.
static bool
holds_for(const function<bool()> &ok, int ms)
{
	if (!ok())
		return false;

	bool expired = false;
	const guint deadline = g_timeout_add(guint(ms), expire, &expired);
	const guint tick = g_timeout_add(guint(kPollMs), poll_tick, nullptr);
	bool held = true;
	while (held && !expired) {
		g_main_context_iteration(nullptr, TRUE);
		held = ok();
	}
	if (!expired)
		g_source_remove(deadline);
	g_source_remove(tick);
	return held && ok();
}

// --- Tree helpers ------------------------------------------------------------

static string
name_of(AtspiAccessible *obj)
{
	gchar *name = atspi_accessible_get_name(obj, nullptr);
	string result = name ? name : "";
	g_free(name);
	return result;
}

static string
role_of(AtspiAccessible *obj)
{
	gchar *role = atspi_accessible_get_role_name(obj, nullptr);
	string result = role ? role : "";
	g_free(role);
	return result;
}

static void
collect_named(
	AtspiAccessible *scope, const char *name, vector<AtspiAccessible *> &out)
{
	if (name_of(scope) == name)
		out.push_back((AtspiAccessible *) g_object_ref(scope));

	const gint n = atspi_accessible_get_child_count(scope, nullptr);
	for (gint i = 0; i < n; i++) {
		AtspiAccessible *child =
			atspi_accessible_get_child_at_index(scope, i, nullptr);
		if (!child)
			continue;
		collect_named(child, name, out);
		g_object_unref(child);
	}
}

static void
collect_role(AtspiAccessible *scope, const char *role, const char *name,
	vector<AtspiAccessible *> &out)
{
	if (role_of(scope) == role && (!name || name_of(scope) == name))
		out.push_back((AtspiAccessible *) g_object_ref(scope));

	const gint n = atspi_accessible_get_child_count(scope, nullptr);
	for (gint i = 0; i < n; i++) {
		AtspiAccessible *child =
			atspi_accessible_get_child_at_index(scope, i, nullptr);
		if (!child)
			continue;
		collect_role(child, role, name, out);
		g_object_unref(child);
	}
}

static void
unref_all(vector<AtspiAccessible *> &objects)
{
	for (AtspiAccessible *obj : objects)
		g_object_unref(obj);
	objects.clear();
}

// Every match, not the first one: one control turning up twice is exactly
// the failure this whole arrangement exists to rule out.
static AtspiAccessible *
find_one(AtspiAccessible *scope, const char *name)
{
	vector<AtspiAccessible *> found;
	collect_named(scope, name, found);
	if (found.size() == 1)
		return found.front();

	test::fail("expected one \"%s\", found %zu", name, found.size());
	unref_all(found);
	return nullptr;
}

static AtspiAccessible *
find_role(AtspiAccessible *scope, const char *role, const char *name)
{
	vector<AtspiAccessible *> found;
	collect_role(scope, role, name, found);
	if (found.size() == 1)
		return found.front();

	if (name)
		test::fail(
			"expected one %s \"%s\", found %zu", role, name, found.size());
	else
		test::fail("expected one %s, found %zu", role, found.size());
	unref_all(found);
	return nullptr;
}

static AtspiAccessible *
try_role(AtspiAccessible *scope, const char *role, const char *name)
{
	vector<AtspiAccessible *> found;
	collect_role(scope, role, name, found);
	if (found.size() == 1)
		return found.front();
	unref_all(found);
	return nullptr;
}

static bool
has_role(AtspiAccessible *scope, const char *role, const char *name)
{
	AtspiAccessible *obj = try_role(scope, role, name);
	if (!obj)
		return false;
	g_object_unref(obj);
	return true;
}

static size_t
count_named(AtspiAccessible *scope, const char *name)
{
	vector<AtspiAccessible *> found;
	collect_named(scope, name, found);
	const size_t n = found.size();
	unref_all(found);
	return n;
}

static bool
has_named(AtspiAccessible *scope, const char *name)
{
	vector<AtspiAccessible *> found;
	collect_named(scope, name, found);
	const bool any = !found.empty();
	unref_all(found);
	return any;
}

static string
labelled_by_name(AtspiAccessible *obj)
{
	GArray *set = atspi_accessible_get_relation_set(obj, nullptr);
	if (!set)
		return {};

	string result;
	for (guint i = 0; i < set->len; i++) {
		auto *rel = g_array_index(set, AtspiRelation *, i);
		if (!rel)
			continue;
		if (atspi_relation_get_relation_type(rel) != ATSPI_RELATION_LABELLED_BY)
			continue;
		if (atspi_relation_get_n_targets(rel) > 0) {
			AtspiAccessible *target = atspi_relation_get_target(rel, 0);
			if (target) {
				result = name_of(target);
				g_object_unref(target);
			}
		}
		g_object_unref(rel);
	}
	g_array_free(set, TRUE);
	return result;
}

// The node just below scope on obj's way up, which is how to ask what a
// client is really looking at without naming any one AT-SPI role.
static AtspiAccessible *
ancestor_below(AtspiAccessible *obj, AtspiAccessible *scope)
{
	auto *at = (AtspiAccessible *) g_object_ref(obj);
	for (int depth = 0; at && depth < 16; depth++) {
		AtspiAccessible *up = atspi_accessible_get_parent(at, nullptr);
		if (!up)
			break;
		if (up == scope) {
			g_object_unref(up);
			return at;
		}
		g_object_unref(at);
		at = up;
	}
	g_clear_object(&at);
	return nullptr;
}

static void
dump_tree(AtspiAccessible *scope, int depth)
{
	fprintf(stderr, "%*s%s \"%s\"\n", depth * 2, "", role_of(scope).c_str(),
		name_of(scope).c_str());
	if (depth > 6)
		return;

	const gint n = atspi_accessible_get_child_count(scope, nullptr);
	for (gint i = 0; i < n; i++) {
		AtspiAccessible *child =
			atspi_accessible_get_child_at_index(scope, i, nullptr);
		if (!child)
			continue;
		dump_tree(child, depth + 1);
		g_object_unref(child);
	}
}

static AtspiRect extents_of(AtspiAccessible *obj, AtspiCoordType type);

// Like dump_tree, with the window-relative box of every node: a layout that
// has gone wrong says so here and nowhere else a headless run can look.
static void
dump_geometry(AtspiAccessible *scope, int depth)
{
	const AtspiRect r = extents_of(scope, ATSPI_COORD_TYPE_WINDOW);
	fprintf(stderr, "%*s%s \"%s\" %d,%d %dx%d\n", depth * 2, "",
		role_of(scope).c_str(), name_of(scope).c_str(), r.x, r.y, r.width,
		r.height);
	if (depth > 8)
		return;

	const gint n = atspi_accessible_get_child_count(scope, nullptr);
	for (gint i = 0; i < n; i++) {
		AtspiAccessible *child =
			atspi_accessible_get_child_at_index(scope, i, nullptr);
		if (!child)
			continue;
		dump_geometry(child, depth + 1);
		g_object_unref(child);
	}
}

// --- Actions, state and geometry ---------------------------------------------

static bool
has_action(AtspiAccessible *obj, const char *want)
{
	AtspiAction *action = atspi_accessible_get_action_iface(obj);
	if (!action)
		return false;

	bool found = false;
	const gint n = atspi_action_get_n_actions(action, nullptr);
	for (gint i = 0; i < n && !found; i++) {
		gchar *name = atspi_action_get_action_name(action, i, nullptr);
		found = name && !strcmp(name, want);
		g_free(name);
	}
	g_object_unref(action);
	return found;
}

static bool
do_action(AtspiAccessible *obj, const char *want)
{
	AtspiAction *action = atspi_accessible_get_action_iface(obj);
	if (!action)
		return false;

	bool done = false;
	const gint n = atspi_action_get_n_actions(action, nullptr);
	for (gint i = 0; i < n && !done; i++) {
		gchar *name = atspi_action_get_action_name(action, i, nullptr);
		if (name && !strcmp(name, want))
			done = atspi_action_do_action(action, i, nullptr);
		g_free(name);
	}
	g_object_unref(action);
	return done;
}

static bool
act_named(AtspiAccessible *scope, const char *name, const char *action)
{
	AtspiAccessible *obj = find_one(scope, name);
	if (!obj)
		return false;
	const bool done = do_action(obj, action);
	g_object_unref(obj);
	return done;
}

// Read back off the bus, never out of the cache: a client that believed its
// own cache here could not tell an action that worked from one that did not.
static bool
has_state(AtspiAccessible *obj, AtspiStateType state)
{
	atspi_accessible_clear_cache_single(obj);
	AtspiStateSet *set = atspi_accessible_get_state_set(obj);
	if (!set)
		return false;

	const bool has = atspi_state_set_contains(set, state);
	g_object_unref(set);
	return has;
}

// Zero-sized on failure, which is also what an unplaced widget answers.
AtspiRect
extents_of(AtspiAccessible *obj, AtspiCoordType type)
{
	AtspiRect result = {0, 0, 0, 0};
	AtspiComponent *component = atspi_accessible_get_component_iface(obj);
	if (!component)
		return result;

	atspi_accessible_clear_cache_single(obj);
	if (AtspiRect *r = atspi_component_get_extents(component, type, nullptr)) {
		result = *r;
		g_free(r);
	}
	g_object_unref(component);
	return result;
}

static void
on_checked(AtspiEvent *event, void *)
{
	g_saw_checked_event = true;
	g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static void
on_children(AtspiEvent *event, void *)
{
	g_saw_children_event = true;
	const char *type = event->type ? event->type : "";
	if (strstr(type, "remove")) {
		g_children_removes++;
		if (event->source)
			g_remove_roles.push_back(role_of(event->source));
	} else if (strstr(type, "add")) {
		g_children_adds++;
	}
	g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static void
on_selection(AtspiEvent *event, void *)
{
	g_saw_selection_event = true;
	if (event->source)
		g_selection_role = role_of(event->source);
	g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static void
on_selected(AtspiEvent *event, void *)
{
	g_saw_selected_event = true;
	g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static void
on_focus(AtspiEvent *event, void *)
{
	g_saw_focus_event = true;
	if (event->source)
		g_focus_role = role_of(event->source);
	g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static string
path_of(AtspiAccessible *obj)
{
	auto *o = ATSPI_OBJECT(obj);
	return o && o->path ? o->path : "";
}

static bool
from_text_source(AtspiEvent *event)
{
	return event && event->source &&
		path_of(event->source) == g_text_source_path;
}

static void
on_text_changed(AtspiEvent *event, void *)
{
	if (from_text_source(event)) {
		g_saw_text_event = true;
		TextChange change;
		const char *type = event->type ? event->type : "";
		if (strstr(type, "insert"))
			change.kind = "insert";
		else if (strstr(type, "delete"))
			change.kind = "delete";
		change.offset = event->detail1;
		change.length = event->detail2;
		if (G_VALUE_HOLDS_STRING(&event->any_data)) {
			const char *s = g_value_get_string(&event->any_data);
			change.text = s ? s : "";
		}
		g_text_changes.push_back(std::move(change));
	}
	g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static void
on_text_caret(AtspiEvent *event, void *)
{
	if (from_text_source(event))
		g_saw_caret_event = true;
	g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static bool
cached_has_named(AtspiAccessible *scope, const char *name)
{
	const gint n = atspi_accessible_get_child_count(scope, nullptr);
	for (gint i = 0; i < n; i++) {
		AtspiAccessible *child =
			atspi_accessible_get_child_at_index(scope, i, nullptr);
		if (!child)
			continue;
		const bool match = name_of(child) == name;
		g_object_unref(child);
		if (match)
			return true;
	}
	return false;
}

// Ask the compositor, not the application: a frame is not a numeric value.
static bool
compositor_resize(const char *command)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !command)
		return false;

	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	const int n = snprintf(
		addr.sun_path, sizeof addr.sun_path, "%s/dn-a11y-resize", runtime);
	if (n < 0 || n >= int(sizeof addr.sun_path))
		return false;

	const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;

	const size_t len = strlen(command);
	const ssize_t sent =
		sendto(fd, command, len, 0, (sockaddr *) &addr, sizeof addr);
	close(fd);
	return sent == ssize_t(len);
}

static string
text_contents(AtspiAccessible *obj)
{
	AtspiText *text = atspi_accessible_get_text_iface(obj);
	if (!text)
		return {};

	atspi_accessible_clear_cache_single(obj);
	gchar *s = atspi_text_get_text(text, 0, -1, nullptr);
	string result = s ? s : "";
	g_free(s);
	g_object_unref(text);
	return result;
}

static string
text_range(AtspiAccessible *obj, int start, int end)
{
	AtspiText *text = atspi_accessible_get_text_iface(obj);
	if (!text)
		return {};

	atspi_accessible_clear_cache_single(obj);
	gchar *s = atspi_text_get_text(text, start, end, nullptr);
	string result = s ? s : "";
	g_free(s);
	g_object_unref(text);
	return result;
}

static int
caret_offset(AtspiAccessible *obj)
{
	AtspiText *text = atspi_accessible_get_text_iface(obj);
	if (!text)
		return -1;

	atspi_accessible_clear_cache_single(obj);
	const int at = atspi_text_get_caret_offset(text, nullptr);
	g_object_unref(text);
	return at;
}

static bool
set_text_contents(AtspiAccessible *obj, const char *value)
{
	AtspiEditableText *edit = atspi_accessible_get_editable_text_iface(obj);
	if (!edit)
		return false;

	const bool ok = atspi_editable_text_set_text_contents(edit, value, nullptr);
	g_object_unref(edit);
	return ok;
}

static bool
insert_text(AtspiAccessible *obj, int offset, const char *value)
{
	AtspiEditableText *edit = atspi_accessible_get_editable_text_iface(obj);
	if (!edit)
		return false;

	const bool ok = atspi_editable_text_insert_text(
		edit, offset, value, int(strlen(value)), nullptr);
	g_object_unref(edit);
	return ok;
}

static bool
delete_text(AtspiAccessible *obj, int start, int end)
{
	AtspiEditableText *edit = atspi_accessible_get_editable_text_iface(obj);
	if (!edit)
		return false;

	const bool ok = atspi_editable_text_delete_text(edit, start, end, nullptr);
	g_object_unref(edit);
	return ok;
}

static int
character_count(AtspiAccessible *obj)
{
	AtspiText *text = atspi_accessible_get_text_iface(obj);
	if (!text)
		return -1;

	atspi_accessible_clear_cache_single(obj);
	const int n = atspi_text_get_character_count(text, nullptr);
	g_object_unref(text);
	return n;
}

static bool
attribute_range(AtspiAccessible *obj, int offset, int *start, int *end)
{
	AtspiText *text = atspi_accessible_get_text_iface(obj);
	if (!text)
		return false;

	atspi_accessible_clear_cache_single(obj);
	int s = -1;
	int e = -1;
	GHashTable *attrs =
		atspi_text_get_text_attributes(text, offset, &s, &e, nullptr);
	if (attrs)
		g_hash_table_unref(attrs);
	g_object_unref(text);
	if (start)
		*start = s;
	if (end)
		*end = e;
	return true;
}

static bool
scroll_substring(AtspiAccessible *obj, int start, int end)
{
	AtspiText *text = atspi_accessible_get_text_iface(obj);
	if (!text)
		return false;

	const bool ok = atspi_text_scroll_substring_to(
		text, start, end, ATSPI_SCROLL_ANYWHERE, nullptr);
	g_object_unref(text);
	return ok;
}

static bool
saw_text_change(const char *kind, int offset, int length)
{
	for (const TextChange &change : g_text_changes) {
		if (change.kind == kind && change.offset == offset &&
			change.length == length)
			return true;
	}
	return false;
}

static gint
live_child_count(AtspiAccessible *obj)
{
	atspi_accessible_clear_cache_single(obj);
	return atspi_accessible_get_child_count(obj, nullptr);
}

static string
description_of(AtspiAccessible *obj)
{
	gchar *text = atspi_accessible_get_description(obj, nullptr);
	string result = text ? text : "";
	g_free(text);
	return result;
}

static AtspiAccessible *
live_child(AtspiAccessible *obj, gint index)
{
	atspi_accessible_clear_cache_single(obj);
	return atspi_accessible_get_child_at_index(obj, index, nullptr);
}

static bool
select_child(AtspiAccessible *obj, gint index)
{
	AtspiSelection *selection = atspi_accessible_get_selection_iface(obj);
	if (!selection)
		return false;
	const bool ok = atspi_selection_select_child(selection, index, nullptr);
	g_object_unref(selection);
	return ok;
}

static bool
child_selected(AtspiAccessible *obj, gint index)
{
	AtspiSelection *selection = atspi_accessible_get_selection_iface(obj);
	if (!selection)
		return false;
	const bool ok =
		atspi_selection_is_child_selected(selection, index, nullptr);
	g_object_unref(selection);
	return ok;
}

static bool
empty_extents(AtspiAccessible *obj)
{
	const AtspiRect box = extents_of(obj, ATSPI_COORD_TYPE_SCREEN);
	return box.width <= 0 || box.height <= 0;
}

static gint
cached_child_count(AtspiAccessible *obj)
{
	return atspi_accessible_get_child_count(obj, nullptr);
}

static bool
clear_selection(AtspiAccessible *obj)
{
	AtspiSelection *selection = atspi_accessible_get_selection_iface(obj);
	if (!selection)
		return false;
	const bool ok = atspi_selection_clear_selection(selection, nullptr);
	g_object_unref(selection);
	return ok;
}

static bool
listen_children()
{
	static bool registered = false;
	if (registered)
		return true;
	GError *error = nullptr;
	if (!atspi_event_listener_register_from_callback(
			on_children, nullptr, nullptr, "object:children-changed", &error)) {
		test::fail("cannot listen for children changes: %s",
			error ? error->message : "unknown");
		g_clear_error(&error);
		return false;
	}
	registered = true;
	return true;
}

static bool
listen_selection()
{
	GError *error = nullptr;
	if (!atspi_event_listener_register_from_callback(on_selection, nullptr,
			nullptr, "object:selection-changed", &error)) {
		test::fail("cannot listen for selection changes: %s",
			error ? error->message : "unknown");
		g_clear_error(&error);
		return false;
	}
	if (!atspi_event_listener_register_from_callback(on_selected, nullptr,
			nullptr, "object:state-changed:selected", &error)) {
		test::fail("cannot listen for selected state: %s",
			error ? error->message : "unknown");
		g_clear_error(&error);
		return false;
	}
	if (!atspi_event_listener_register_from_callback(on_focus, nullptr, nullptr,
			"object:state-changed:focused", &error)) {
		test::fail("cannot listen for focus changes: %s",
			error ? error->message : "unknown");
		g_clear_error(&error);
		return false;
	}
	return true;
}

static bool
listen_text(AtspiAccessible *obj)
{
	g_text_source_path = path_of(obj);
	g_saw_text_event = false;
	g_saw_caret_event = false;
	g_text_changes.clear();
	GError *error = nullptr;
	if (!atspi_event_listener_register_from_callback(
			on_text_changed, nullptr, nullptr, "object:text-changed", &error)) {
		test::fail("cannot listen for text changes: %s",
			error ? error->message : "unknown");
		g_clear_error(&error);
		return false;
	}
	if (!atspi_event_listener_register_from_callback(on_text_caret, nullptr,
			nullptr, "object:text-caret-moved", &error)) {
		test::fail("cannot listen for caret moves: %s",
			error ? error->message : "unknown");
		g_clear_error(&error);
		return false;
	}
	return true;
}

static bool
replace_and_wait(AtspiAccessible *field, const char *value)
{
	g_saw_text_event = false;
	g_saw_caret_event = false;
	g_text_changes.clear();
	if (!set_text_contents(field, value)) {
		test::fail("SetTextContents(\"%s\") did not go through", value);
		return false;
	}
	const string want = value;
	CHECK(wait_until([field, want] { return text_contents(field) == want; }));
	CHECK(wait_until([] { return g_saw_text_event; }));
	CHECK(wait_until([] { return g_saw_caret_event; }));
	CHECK(caret_offset(field) == character_count(field));
	return true;
}

// --- Process -----------------------------------------------------------------

static bool
dn_running()
{
	if (g_pid <= 0)
		return false;

	int status = 0;
	if (waitpid(g_pid, &status, WNOHANG) != g_pid)
		return true;

	fprintf(stderr, "dn exited: status %d, signal %d\n",
		WIFEXITED(status) ? WEXITSTATUS(status) : -1,
		WIFSIGNALED(status) ? WTERMSIG(status) : 0);
	g_pid = -1;
	return false;
}

static void
reap_dn()
{
	if (g_pid <= 0)
		return;

	kill(g_pid, SIGKILL);
	waitpid(g_pid, nullptr, 0);
	g_pid = -1;
}

// The process we started, not whichever application got to the bus first.
static AtspiAccessible *
find_app()
{
	AtspiAccessible *desktop = atspi_get_desktop(0);
	if (!desktop)
		return nullptr;

	AtspiAccessible *found = nullptr;
	atspi_accessible_clear_cache(desktop);
	const gint n = atspi_accessible_get_child_count(desktop, nullptr);
	for (gint i = 0; i < n && !found; i++) {
		AtspiAccessible *child =
			atspi_accessible_get_child_at_index(desktop, i, nullptr);
		if (!child)
			continue;
		if (atspi_accessible_get_process_id(child, nullptr) == guint(g_pid))
			found = child;
		else
			g_object_unref(child);
	}
	g_object_unref(desktop);
	return found;
}

// --- Cases -------------------------------------------------------------------

static void
case_startup()
{
	g_pid = fork();
	if (!g_pid) {
		execl(g_dn, g_dn, "--new-instance", "--mode", "browse", g_dir,
			(char *) nullptr);
		_exit(127);
	}
	if (g_pid < 0) {
		test::fail("cannot fork: %s", g_strerror(errno));
		return;
	}

	wait_until([] {
		if (!dn_running())
			return true;
		if (!g_app)
			g_app = find_app();
		return g_app && atspi_accessible_get_child_count(g_app, nullptr) > 0;
	});
	if (!dn_running()) {
		test::fail("dn exited before it could be inspected");
		return;
	}
	if (!g_app) {
		test::fail("no application node for pid %d", int(g_pid));
		return;
	}

	// Nothing below may believe a cache: every check re-asks the process.
	atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_NONE);
	wait_until([] {
		if (!dn_running())
			return true;
		return atspi_accessible_get_child_count(g_app, nullptr) > 0;
	});

	const gint windows = atspi_accessible_get_child_count(g_app, nullptr);
	int frames = 0;
	for (gint i = 0; i < windows; i++) {
		AtspiAccessible *child =
			atspi_accessible_get_child_at_index(g_app, i, nullptr);
		if (!child)
			continue;
		const string role = role_of(child);
		fprintf(stderr, "application child %d: %s \"%s\"\n", int(i),
			role.c_str(), name_of(child).c_str());
		if (role == "frame") {
			frames++;
			if (!g_window)
				g_window = child;
			else
				g_object_unref(child);
		} else {
			g_object_unref(child);
		}
	}
	CHECK(frames == 1);
	if (!g_window) {
		test::fail("the application has no window (children %d)", int(windows));
		return;
	}

	// The arrangement this milestone exists to settle.  Printed rather than
	// asserted: what it ought to be is what this reports.
	fprintf(stderr, "--- tree as exposed ---\n");
	dump_tree(g_window, 0);
	fprintf(stderr, "-----------------------\n");

	CHECK(name_of(g_window).find("Dawn") != string::npos);

	// Orca picks the window it reads focus events against from
	// window:activate, which the bridge only sends for a state change on a
	// Window-role node, and whose payload it takes from this state.  The
	// two have to agree, and the compositor has to be handing dn the
	// keyboard at all, or every focus assertion below is about nothing.
	CHECK(wait_until([] { return has_state(g_window, ATSPI_STATE_ACTIVE); }));
}

static void
case_panes()
{
	if (!g_window)
		return;

	AtspiAccessible *computer = find_one(g_window, "Computer");
	AtspiAccessible *list = find_role(g_window, "list", nullptr);
	if (computer && list) {
		AtspiAccessible *side = atspi_accessible_get_parent(computer, nullptr);
		if (side) {
			CHECK(atspi_accessible_get_index_in_parent(side, nullptr) <
				atspi_accessible_get_index_in_parent(list, nullptr));
			g_object_unref(side);
		}
		// Browser order is toolbar, left sidebar, listing. F6 must follow
		// that same order, rather than an independent ownership order.
		CHECK(act_named(g_window, "Filter", "SetFocus"));
		CHECK(compositor_resize("next-pane"));
		CHECK(wait_until(
			[computer] { return has_state(computer, ATSPI_STATE_FOCUSED); }));
		CHECK(compositor_resize("next-pane"));
		CHECK(wait_until(
			[list] { return has_state(list, ATSPI_STATE_FOCUSED); }));
	}
	g_clear_object(&computer);
	g_clear_object(&list);

	CHECK(act_named(g_window, "black.png", "Press"));
	CHECK(wait_until([] { return has_role(g_window, "image", "black.png"); }));
	CHECK(act_named(g_window, "Show Information", "Press"));
	CHECK(wait_until([] { return has_named(g_window, "Name:"); }));
	AtspiAccessible *name = find_one(g_window, "Name:");
	AtspiAccessible *view = find_role(g_window, "image", "black.png");
	if (name && view) {
		AtspiAccessible *side = atspi_accessible_get_parent(name, nullptr);
		if (side) {
			CHECK(atspi_accessible_get_index_in_parent(view, nullptr) <
				atspi_accessible_get_index_in_parent(side, nullptr));
			g_object_unref(side);
		}
		// The viewer has its sidebar on the right, so its image is the
		// first pane after the toolbar, not the sidebar constructed with it.
		CHECK(act_named(g_window, "Browse", "SetFocus"));
		CHECK(compositor_resize("next-pane"));
		CHECK(wait_until(
			[view] { return has_state(view, ATSPI_STATE_FOCUSED); }));
	}
	g_clear_object(&name);
	g_clear_object(&view);
	CHECK(act_named(g_window, "Show Information", "Press"));
	CHECK(act_named(g_window, "Browse", "Press"));
	CHECK(wait_until([] { return has_named(g_window, "Filter"); }));
}

static void
case_button()
{
	if (!g_window)
		return;

	AtspiAccessible *button = find_one(g_window, "Dark Mode");
	if (!button)
		return;

	CHECK(has_action(button, "Press"));
	CHECK(has_state(button, ATSPI_STATE_SENSITIVE));

	const bool was = has_state(button, ATSPI_STATE_CHECKED);
	g_saw_checked_event = false;
	GError *error = nullptr;
	if (!atspi_event_listener_register_from_callback(on_checked, nullptr,
			nullptr, "object:state-changed:checked", &error)) {
		test::fail("cannot listen for state changes: %s",
			error ? error->message : "unknown");
		g_clear_error(&error);
	}

	if (!do_action(button, "Press")) {
		test::fail("the Press action did not go through");
		g_object_unref(button);
		return;
	}

	// The result, not the return value: a successful D-Bus call that changed
	// nothing is the failure worth catching here.
	auto flipped = [button, was] {
		return has_state(button, ATSPI_STATE_CHECKED) != was;
	};
	CHECK(wait_until(flipped));
	CHECK(wait_until([] { return g_saw_checked_event; }));
	g_object_unref(button);
}

static void
case_extents()
{
	if (!g_window)
		return;

	AtspiAccessible *button = find_one(g_window, "Dark Mode");
	if (!button)
		return;

	const AtspiRect frame = extents_of(g_window, ATSPI_COORD_TYPE_SCREEN);
	const AtspiRect screen = extents_of(button, ATSPI_COORD_TYPE_SCREEN);
	const AtspiRect local = extents_of(button, ATSPI_COORD_TYPE_WINDOW);
	fprintf(stderr,
		"window %d,%d %dx%d button %d,%d %dx%d window-relative %d,%d %dx%d\n",
		frame.x, frame.y, frame.width, frame.height, screen.x, screen.y,
		screen.width, screen.height, local.x, local.y, local.width,
		local.height);

	CHECK(frame.width > 0 && frame.height > 0);
	CHECK(screen.width > 0 && screen.height > 0);
	// Inside the window it belongs to, at real coordinates: an empty or a
	// stray rectangle is a clickable ghost to a hint overlay.
	CHECK(screen.x >= frame.x && screen.y >= frame.y);
	CHECK(screen.x + screen.width <= frame.x + frame.width);
	CHECK(screen.y + screen.height <= frame.y + frame.height);
	// Qt derives this from an ancestor's accessible rectangle rather than
	// from the window handle, so the two have to be checked against one
	// another; the content surface also hangs outside its shell by the glow.
	CHECK(local.x == screen.x - frame.x);
	CHECK(local.y == screen.y - frame.y);
	CHECK(local.width == screen.width && local.height == screen.height);

	// Whatever is at the button's middle is the button, and not the toolbar
	// it sits on, nor a widget the layout happens to have nearby.
	AtspiComponent *window = atspi_accessible_get_component_iface(g_window);
	if (window) {
		AtspiAccessible *at = atspi_component_get_accessible_at_point(window,
			screen.x + screen.width / 2, screen.y + screen.height / 2,
			ATSPI_COORD_TYPE_SCREEN, nullptr);
		if (at) {
			CHECK(name_of(at) == "Dark Mode");
			g_object_unref(at);
		} else {
			test::fail("nothing was hit where the button is");
		}
		g_object_unref(window);
	}
	g_object_unref(button);
}

static void
case_dialog()
{
	if (!g_window)
		return;

	AtspiAccessible *menu_button = find_one(g_window, "Menu");
	if (!menu_button)
		return;

	CHECK(has_action(menu_button, "ShowMenu"));
	const bool opened = do_action(menu_button, "ShowMenu") &&
		wait_until([] { return has_named(g_window, "Help"); });
	g_object_unref(menu_button);
	if (!opened) {
		test::fail("the application menu did not open");
		return;
	}

	AtspiAccessible *help = find_one(g_window, "Help");
	if (!help)
		return;

	CHECK(has_action(help, "ShowMenu"));
	const bool submenu = do_action(help, "ShowMenu") &&
		wait_until([] { return has_named(g_window, "About"); });
	g_object_unref(help);
	if (!submenu) {
		test::fail("the Help submenu did not open");
		return;
	}

	AtspiAccessible *about = find_one(g_window, "About");
	if (!about)
		return;

	const bool shown = do_action(about, "Press") &&
		wait_until([] { return has_named(g_window, "Close"); });
	g_object_unref(about);
	if (!shown) {
		test::fail("the About dialog did not open");
		return;
	}

	// A dialog is a child of the window, and Qt converts window-relative
	// coordinates through whichever ancestor is a window or a dialog --
	// which makes this a different path from the toolbar's.
	AtspiAccessible *close = find_one(g_window, "Close");
	if (!close)
		return;

	CHECK(wait_until([close] {
		return extents_of(close, ATSPI_COORD_TYPE_SCREEN).width > 0;
	}));

	// Whatever the dialog is exposed as, it is the window's own child, and
	// Qt converts through it rather than through the window.
	AtspiAccessible *dialog = ancestor_below(close, g_window);
	if (!dialog) {
		test::fail("the dialog is not a child of the window");
		g_object_unref(close);
		return;
	}

	const AtspiRect frame = extents_of(g_window, ATSPI_COORD_TYPE_SCREEN);
	const AtspiRect box = extents_of(dialog, ATSPI_COORD_TYPE_SCREEN);
	const AtspiRect screen = extents_of(close, ATSPI_COORD_TYPE_SCREEN);
	const AtspiRect local = extents_of(close, ATSPI_COORD_TYPE_WINDOW);
	fprintf(stderr,
		"dialog %s \"%s\" %d,%d %dx%d button %d,%d %dx%d relative %d,%d\n",
		role_of(dialog).c_str(), name_of(dialog).c_str(), box.x, box.y,
		box.width, box.height, screen.x, screen.y, screen.width, screen.height,
		local.x, local.y);
	g_object_unref(dialog);

	CHECK(box.width > 0 && box.height > 0);
	CHECK(screen.x >= box.x && screen.y >= box.y);
	CHECK(screen.x + screen.width <= box.x + box.width);
	CHECK(screen.y + screen.height <= box.y + box.height);
	CHECK(screen.x >= frame.x && screen.y >= frame.y);
	// The trap the plan names: this ancestor is a Dialog, not the window,
	// and Qt subtracts whichever it found.  Which one it used is the fact
	// this run is here to record.
	CHECK(local.x == screen.x - box.x);
	CHECK(local.y == screen.y - box.y);
	CHECK(role_of(dialog) == "dialog");

	AtspiAccessible *dark = find_one(g_window, "Dark Mode");
	if (dark) {
		// The dialog is modal: the toolbar is still there, but pressing it
		// would operate the page underneath, which no click can do.
		CHECK(!has_action(dark, "Press"));
		g_object_unref(dark);
	}

	CHECK(do_action(close, "Press"));
	CHECK(wait_until([] { return !has_named(g_window, "Close"); }));
	g_object_unref(close);

	CHECK(wait_until([] {
		AtspiAccessible *button = nullptr;
		vector<AtspiAccessible *> found;
		collect_named(g_window, "Dark Mode", found);
		bool press = false;
		if (found.size() == 1) {
			button = found.front();
			press = has_action(button, "Press");
		}
		unref_all(found);
		return press;
	}));
}

static bool
open_app_menu()
{
	return act_named(g_window, "Menu", "ShowMenu") &&
		wait_until([] { return has_named(g_window, "Help"); });
}

static void
case_settings()
{
	if (!g_window)
		return;

	if (!open_app_menu()) {
		test::fail("the application menu did not open");
		return;
	}
	if (!act_named(g_window, "File", "ShowMenu") ||
		!wait_until([] { return has_named(g_window, "Settings..."); })) {
		test::fail("the File submenu did not open");
		return;
	}
	if (!act_named(g_window, "Settings...", "Press") ||
		!wait_until([] { return has_named(g_window, "Cancel"); })) {
		test::fail("the Settings dialog did not open");
		return;
	}

	AtspiAccessible *dialog = find_role(g_window, "dialog", "Settings");
	if (!dialog)
		return;

	// Discover a control that has never been asked about in a layout
	// sweep of its own, then flip it before the next frame can arm a
	// baseline: the adapter has to have captured its initial state at
	// construction, or the checked event is swallowed.
	AtspiAccessible *filenames =
		find_role(dialog, "check box", "Show filenames by default");
	if (filenames) {
		const bool was = has_state(filenames, ATSPI_STATE_CHECKED);
		g_saw_checked_event = false;
		GError *error = nullptr;
		if (!atspi_event_listener_register_from_callback(on_checked, nullptr,
				nullptr, "object:state-changed:checked", &error)) {
			test::fail("cannot listen for state changes: %s",
				error ? error->message : "unknown");
			g_clear_error(&error);
		}
		CHECK(do_action(filenames, "Toggle"));
		CHECK(wait_until([filenames, was] {
			return has_state(filenames, ATSPI_STATE_CHECKED) != was;
		}));
		CHECK(wait_until([] { return g_saw_checked_event; }));
		g_object_unref(filenames);
	}

	AtspiAccessible *combo = find_role(dialog, "combo box", nullptr);
	if (!combo) {
		g_object_unref(dialog);
		return;
	}
	CHECK(name_of(combo) == "Normal");
	CHECK(labelled_by_name(combo) == "Default thumbnail size");
	CHECK(has_action(combo, "ShowMenu"));
	CHECK(has_state(combo, ATSPI_STATE_SENSITIVE));

	const bool expanded = do_action(combo, "ShowMenu") &&
		wait_until([] { return has_named(g_window, "Huge"); });
	if (!expanded) {
		test::fail("the thumbnail size list did not open");
		g_object_unref(combo);
		g_object_unref(dialog);
		return;
	}

	// The opener still toggles the list, but other dialog controls are
	// blocked until it closes, just as they are for pointer input.
	filenames = find_role(dialog, "check box", "Show filenames by default");
	if (filenames) {
		CHECK(!has_action(filenames, "Toggle"));
		CHECK(!do_action(filenames, "Toggle"));
		g_object_unref(filenames);
	}
	CHECK(has_action(combo, "ShowMenu"));

	AtspiAccessible *huge = find_role(g_window, "list item", "Huge");
	if (!huge) {
		g_object_unref(combo);
		g_object_unref(dialog);
		return;
	}

	// The list says which choice is current by where it drops itself, with
	// no check column to read it off; a client has to be told outright.
	// Focus is not that answer: the arrows walk off the current item, and
	// it stays current.
	AtspiAccessible *normal = find_role(g_window, "list item", "Normal");
	AtspiAccessible *choices = ancestor_below(huge, g_window);
	if (normal && choices) {
		CHECK(has_state(normal, ATSPI_STATE_SELECTED));
		CHECK(has_state(normal, ATSPI_STATE_SELECTABLE));
		CHECK(!has_state(huge, ATSPI_STATE_SELECTED));
		CHECK(has_state(huge, ATSPI_STATE_SELECTABLE));

		// Selecting a choice takes the value without taking the list down,
		// which is what tells it apart from pressing one.
		const gint at = atspi_accessible_get_index_in_parent(huge, nullptr);
		CHECK(at >= 0);
		CHECK(select_child(choices, at));
		CHECK(wait_until([combo] { return name_of(combo) == "Huge"; }));
		CHECK(wait_until(
			[huge] { return has_state(huge, ATSPI_STATE_SELECTED); }));
		CHECK(!has_state(normal, ATSPI_STATE_SELECTED));
		CHECK(child_selected(choices, at));
		CHECK(has_named(g_window, "Small"));

		// A combo always stands on one of its choices, so there is nothing
		// for clearing the selection to mean.
		CHECK(!clear_selection(choices));
		CHECK(select_child(
			choices, atspi_accessible_get_index_in_parent(normal, nullptr)));
		CHECK(wait_until([combo] { return name_of(combo) == "Normal"; }));
	}
	g_clear_object(&choices);
	g_clear_object(&normal);

	CHECK(do_action(huge, "Press"));
	g_object_unref(huge);
	CHECK(wait_until([combo] { return name_of(combo) == "Huge"; }));
	// Pressing does take the list down, unlike selecting.
	CHECK(wait_until([] { return !has_named(g_window, "Small"); }));
	g_object_unref(combo);

	CHECK(has_named(dialog,
		"Image loaders may be able to handle multiple "
		"formats. Failures pass through."));

	vector<AtspiAccessible *> ups;
	collect_named(dialog, "Move up", ups);
	if (ups.empty()) {
		test::fail("no Move up control in Settings");
	} else {
		AtspiAccessible *first = ups.front();
		CHECK(!has_state(first, ATSPI_STATE_SENSITIVE));
		CHECK(!has_action(first, "Press"));
	}
	unref_all(ups);

	g_object_unref(dialog);
	CHECK(act_named(g_window, "Cancel", "Press"));
	CHECK(wait_until([] { return !has_named(g_window, "Cancel"); }));
}

static void
case_location()
{
	if (!g_window)
		return;

	if (!open_app_menu()) {
		test::fail("the application menu did not open");
		return;
	}
	if (!act_named(g_window, "Go", "ShowMenu") ||
		!wait_until([] { return has_named(g_window, "Location..."); })) {
		test::fail("the Go submenu did not open");
		return;
	}
	if (!act_named(g_window, "Location...", "Press") ||
		!wait_until([] { return has_named(g_window, "Enter location"); })) {
		test::fail("the location dialog did not open");
		return;
	}

	AtspiAccessible *dialog = find_role(g_window, "dialog", "Enter location");
	if (!dialog)
		return;

	AtspiAccessible *field = find_role(dialog, "text", "Enter location");
	if (field) {
		CHECK(labelled_by_name(field) == "Enter location");
		g_object_unref(field);
	}
	g_object_unref(dialog);

	CHECK(act_named(g_window, "Cancel", "Press"));
	CHECK(wait_until([] { return !has_named(g_window, "Enter location"); }));
}

static void
case_filter()
{
	if (!g_window)
		return;

	AtspiAccessible *field = find_role(g_window, "text", "Filter");
	if (!field)
		return;

	CHECK(name_of(field) == "Filter");
	AtspiText *text = atspi_accessible_get_text_iface(field);
	CHECK(text != nullptr);
	if (text)
		g_object_unref(text);
	AtspiEditableText *edit = atspi_accessible_get_editable_text_iface(field);
	CHECK(edit != nullptr);
	if (edit)
		g_object_unref(edit);

	AtspiAccessible *list = find_role(g_window, "list", nullptr);
	const gint listed_before = list ? live_child_count(list) : 0;
	fprintf(stderr, "filter listing children before: %d\n", listed_before);

	if (!listen_text(field)) {
		g_object_unref(field);
		if (list)
			g_object_unref(list);
		return;
	}

	if (!replace_and_wait(field, "blue.svg")) {
		g_object_unref(field);
		if (list)
			g_object_unref(list);
		return;
	}
	CHECK(name_of(field) == "Filter");

	if (listed_before > 0) {
		CHECK(wait_until([list, listed_before] {
			const gint n = live_child_count(list);
			return n > 0 && n <= listed_before;
		}));
	}

	if (!replace_and_wait(field, "no-such-dn-file")) {
		g_object_unref(field);
		if (list)
			g_object_unref(list);
		return;
	}
	CHECK(name_of(field) == "Filter");
	if (listed_before > 0)
		CHECK(wait_until([list] { return live_child_count(list) == 0; }));

	// Non-ASCII committed text, still the same named field.  Caret offsets
	// follow Qt's UTF-16, so this only asserts the string that comes back.
	g_saw_text_event = false;
	g_saw_caret_event = false;
	g_text_changes.clear();
	if (!set_text_contents(field, "café"))
		test::fail("SetTextContents(\"café\") did not go through");
	else {
		CHECK(wait_until([field] { return text_contents(field) == "café"; }));
		CHECK(wait_until([] { return g_saw_text_event; }));
		CHECK(wait_until([] { return g_saw_caret_event; }));
	}
	CHECK(name_of(field) == "Filter");

	// NFD e + combining acute: adjacent UTF-16 slices must not both grow
	// into the whole cluster.
	if (!replace_and_wait(field,
			"Ae\xCC\x81"
			"B")) {
		g_object_unref(field);
		if (list)
			g_object_unref(list);
		return;
	}
	CHECK(character_count(field) == 4);
	CHECK(text_range(field, 1, 2) == "e");
	CHECK(text_range(field, 2, 3) == "\xCC\x81");
	CHECK(text_range(field, 1, 2) != text_range(field, 2, 3));

	if (!replace_and_wait(field, "abc")) {
		g_object_unref(field);
		if (list)
			g_object_unref(list);
		return;
	}
	int attr_start = -1;
	int attr_end = -1;
	CHECK(attribute_range(field, 1, &attr_start, &attr_end));
	CHECK(attr_start == 0 && attr_end == 3);

	const int caret_before_scroll = caret_offset(field);
	CHECK(scroll_substring(field, 0, 1));
	CHECK(holds_for(
		[field, caret_before_scroll] {
			return caret_offset(field) == caret_before_scroll;
		},
		250));
	CHECK(text_contents(field) == "abc");

	g_saw_text_event = false;
	g_text_changes.clear();
	if (!insert_text(field, 1, "X"))
		test::fail("InsertText(\"X\") did not go through");
	else {
		CHECK(wait_until([field] { return text_contents(field) == "aXbc"; }));
		CHECK(wait_until([] { return saw_text_change("insert", 1, 1); }));
		CHECK(!saw_text_change("delete", 0, 3));
	}

	if (!replace_and_wait(field, "A😀B")) {
		g_object_unref(field);
		if (list)
			g_object_unref(list);
		return;
	}
	CHECK(character_count(field) == 4);
	g_saw_text_event = false;
	g_text_changes.clear();
	if (!delete_text(field, 1, 2))
		test::fail("DeleteText([1,2)) did not go through");
	else {
		CHECK(wait_until([field] { return text_contents(field) == "AB"; }));
		CHECK(character_count(field) == 2);
		CHECK(wait_until([] { return saw_text_change("delete", 1, 2); }));
	}
	CHECK(name_of(field) == "Filter");

	CHECK(set_text_contents(field, ""));
	CHECK(wait_until([field] { return text_contents(field).empty(); }));
	CHECK(name_of(field) == "Filter");

	g_object_unref(field);
	if (list)
		g_object_unref(list);
}

static void
case_files()
{
	if (!g_window)
		return;

	AtspiAccessible *list = find_role(g_window, "list", nullptr);
	if (!list)
		return;

	CHECK(wait_until([list] { return live_child_count(list) > 1; }));
	const gint n = live_child_count(list);
	if (n < 2) {
		g_object_unref(list);
		return;
	}

	AtspiAccessible *first = live_child(list, 0);
	AtspiAccessible *last = live_child(list, n - 1);
	if (!first || !last) {
		g_clear_object(&first);
		g_clear_object(&last);
		g_object_unref(list);
		test::fail("the listing did not expose its first and last files");
		return;
	}

	CHECK(role_of(first) == "list item");
	CHECK(!name_of(first).empty());
	CHECK(!description_of(first).empty());
	CHECK(has_action(first, "SetFocus"));
	CHECK(has_action(first, "Press"));
	g_object_unref(first);
	g_object_unref(last);
	last = nullptr;

	// An eager client can materialise every row without thumbnails.
	for (gint i = 0; i < n; i++) {
		AtspiAccessible *item = live_child(list, i);
		if (!item) {
			test::fail("listing child %d was missing", int(i));
			break;
		}
		CHECK(!name_of(item).empty());
		g_object_unref(item);
	}

	AtspiAccessible *blue = find_role(g_window, "list item", "blue.svg");
	if (!blue) {
		g_object_unref(list);
		return;
	}
	const string blue_id = path_of(blue);
	CHECK(!blue_id.empty());

	if (!listen_children() || !listen_selection()) {
		g_object_unref(blue);
		g_object_unref(list);
		return;
	}

	atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_DEFAULT);
	(void) atspi_accessible_get_child_count(list, nullptr);

	g_saw_children_event = false;
	g_children_adds = 0;
	g_children_removes = 0;
	if (!act_named(g_window, "Time", "Press")) {
		test::fail("the Time sort did not go through");
		g_object_unref(blue);
		g_object_unref(list);
		return;
	}
	CHECK(wait_until(
		[] { return g_children_adds > 0 && g_children_removes > 0; }));
	AtspiAccessible *blue_sorted = find_role(g_window, "list item", "blue.svg");
	if (!blue_sorted) {
		g_object_unref(blue);
		g_object_unref(list);
		return;
	}
	CHECK(path_of(blue_sorted) == blue_id);
	CHECK(name_of(blue_sorted) == "blue.svg");
	g_object_unref(blue_sorted);

	AtspiAccessible *field = find_role(g_window, "text", "Filter");
	if (field) {
		g_saw_selection_event = false;
		g_selection_role.clear();
		CHECK(select_child(list, 0));
		CHECK(wait_until([] { return g_saw_selection_event; }));
		CHECK(g_selection_role == "list");

		g_saw_children_event = false;
		g_children_adds = 0;
		g_children_removes = 0;
		g_saw_selection_event = false;
		g_selection_role.clear();
		CHECK(set_text_contents(field, "no-such-dn-file"));
		CHECK(wait_until([] { return g_children_removes > 0; }));
		CHECK(wait_until([] { return g_saw_selection_event; }));
		CHECK(g_selection_role == "list");
		CHECK(cached_child_count(list) == 0);
		atspi_accessible_clear_cache_single(blue);
		const string stale = name_of(blue);
		CHECK(stale.empty() || stale == "blue.svg");
		CHECK(stale != "green.svg");
		CHECK(stale != "red.svg");

		g_saw_children_event = false;
		g_children_adds = 0;
		g_children_removes = 0;
		CHECK(set_text_contents(field, ""));
		CHECK(wait_until([] { return g_children_adds > 0; }));
		// Arrivals are not announced one by one: wrappers are made on
		// child(i), and a directory of ten thousand files would otherwise
		// allocate one for each just to say it is there.  What the list
		// sends instead is that it was replaced, and the count is asked
		// for again rather than added up from events.
		CHECK(live_child_count(list) > 1);
		AtspiAccessible *blue_again =
			find_role(g_window, "list item", "blue.svg");
		if (blue_again) {
			CHECK(path_of(blue_again) != blue_id);
			CHECK(name_of(blue_again) == "blue.svg");
			g_object_unref(blue_again);
		}
		g_object_unref(field);
	}
	g_object_unref(blue);

	atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_NONE);
	atspi_accessible_clear_cache(list);

	AtspiAccessible *item = live_child(list, 0);
	if (item) {
		g_saw_selection_event = false;
		g_saw_selected_event = false;
		g_selection_role.clear();
		CHECK(select_child(list, 0));
		CHECK(wait_until([] { return g_saw_selection_event; }));
		CHECK(g_selection_role == "list");
		CHECK(wait_until([] { return g_saw_selected_event; }));
		CHECK(wait_until([list] { return child_selected(list, 0); }));
		CHECK(has_state(item, ATSPI_STATE_SELECTED));

		g_saw_focus_event = false;
		g_focus_role.clear();
		CHECK(do_action(item, "SetFocus"));
		CHECK(wait_until(
			[item] { return has_state(item, ATSPI_STATE_FOCUSED); }));

		g_saw_focus_event = false;
		g_focus_role.clear();
		CHECK(clear_selection(list));
		CHECK(wait_until([] { return g_saw_focus_event; }));
		CHECK(wait_until([item, list] {
			return !has_state(item, ATSPI_STATE_FOCUSED) &&
				has_state(list, ATSPI_STATE_FOCUSED);
		}));
		g_object_unref(item);
	}

	CHECK(compositor_resize("shrink"));
	CHECK(wait_until([list] {
		const gint count = live_child_count(list);
		if (count < 2)
			return false;
		AtspiAccessible *row = live_child(list, count - 1);
		if (!row)
			return false;
		const bool off = empty_extents(row);
		g_object_unref(row);
		return off;
	}));

	const gint listed = live_child_count(list);
	last = live_child(list, listed - 1);
	if (!last) {
		g_object_unref(list);
		compositor_resize("restore");
		test::fail("the last listing row vanished after shrink");
		return;
	}
	const string last_name = name_of(last);
	CHECK(!last_name.empty());
	CHECK(empty_extents(last));
	CHECK(select_child(list, listed - 1));
	CHECK(wait_until(
		[list, listed] { return child_selected(list, listed - 1); }));
	CHECK(has_state(last, ATSPI_STATE_SELECTED));
	CHECK(name_of(g_window).find(last_name) == string::npos);
	CHECK(has_role(g_window, "list", nullptr));
	CHECK(live_child_count(list) == listed);

	// Whatever a point hits must be something whose own rectangle holds that
	// point.  The well is short enough here to clip its bottom row, and a
	// clipped row reports the clipped rectangle: a hit test against the
	// whole tile would hand back a target with nothing to click in it.
	// Walked down the middle of the listing, because that is where the rows
	// are, and the last one of them is the one cut off.
	if (AtspiComponent *well = atspi_accessible_get_component_iface(list)) {
		const AtspiRect box = extents_of(list, ATSPI_COORD_TYPE_SCREEN);
		const gint x = box.x + box.width / 2;
		bool consistent = true, hit_something = false;
		for (gint y = box.y; consistent && y < box.y + box.height; y += 4) {
			AtspiAccessible *at = atspi_component_get_accessible_at_point(
				well, x, y, ATSPI_COORD_TYPE_SCREEN, nullptr);
			if (!at)
				continue;
			hit_something = true;
			const AtspiRect r = extents_of(at, ATSPI_COORD_TYPE_SCREEN);
			consistent =
				x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
			if (!consistent)
				test::fail("(%d, %d) hit \"%s\" at %d,%d %dx%d", int(x), int(y),
					name_of(at).c_str(), int(r.x), int(r.y), int(r.width),
					int(r.height));
			g_object_unref(at);
		}
		CHECK(box.height > 0);
		CHECK(hit_something);

		// And the other side of it.  A row straddling the bottom of the
		// well keeps its tile past the edge, but not its rectangle: the
		// part that got clipped away is outside the listing altogether, so
		// asking the listing what is down there has to come back empty.
		for (gint y = box.y + box.height; y < box.y + box.height + 24; y += 2) {
			AtspiAccessible *at = atspi_component_get_accessible_at_point(
				well, x, y, ATSPI_COORD_TYPE_SCREEN, nullptr);
			if (!at)
				continue;
			test::fail("(%d, %d) is below the listing, and hit \"%s\"", int(x),
				int(y), name_of(at).c_str());
			g_object_unref(at);
			break;
		}
		g_object_unref(well);
	}

	CHECK(do_action(last, "SetFocus"));
	CHECK(wait_until([last] { return !empty_extents(last); }));
	CHECK(name_of(g_window).find(last_name) == string::npos);
	CHECK(live_child_count(list) == listed);

	// Switching modes replaces every child of the client area at once, and
	// destroys nothing: both pages stay alive, and only one of them is the
	// root of the tree.  A client that keeps a tree from events would go on
	// showing the listing unless the container itself says otherwise, so
	// the removal has to arrive, and it has to arrive on the client area.
	g_saw_children_event = false;
	g_children_adds = 0;
	g_children_removes = 0;
	g_remove_roles.clear();

	CHECK(do_action(last, "Press"));
	CHECK(wait_until(
		[&] { return name_of(g_window).find(last_name) != string::npos; }));
	CHECK(wait_until([&] {
		AtspiAccessible *image = try_role(g_window, "image", last_name.c_str());
		if (!image)
			return false;
		const bool named = name_of(image) == last_name;
		const bool sized = description_of(image).find("\u00d7") != string::npos;
		g_object_unref(image);
		return named && sized;
	}));
	CHECK(wait_until([] { return g_children_removes > 0; }));
	CHECK(wait_until([] { return g_children_adds > 0; }));
	CHECK(find(g_remove_roles.begin(), g_remove_roles.end(), "filler") !=
		g_remove_roles.end());

	g_object_unref(last);
	g_object_unref(list);

	g_saw_children_event = false;
	g_remove_roles.clear();
	if (!act_named(g_window, "Browse", "Press") ||
		!wait_until([] { return has_role(g_window, "list", nullptr); })) {
		test::fail("Browse did not return to the listing");
	}
	// And back the other way, which is the same container losing the viewer.
	CHECK(wait_until([] {
		return find(g_remove_roles.begin(), g_remove_roles.end(), "filler") !=
			g_remove_roles.end();
	}));
	CHECK(compositor_resize("restore"));
	CHECK(wait_until([] { return has_named(g_window, "Filter"); }));
}

// The file chooser, reached the way Settings reaches it -- which puts it over
// a dialog that is already up.  Everything here is the stack seen from
// outside: a chooser that is itself modal, a prompt that stacks over that,
// rows the platform can name and press, and the reaping that happens a frame
// after a footer button has closed the very tree it was running in.
static void
case_chooser()
{
	if (!g_window)
		return;

	if (!open_app_menu()) {
		test::fail("the application menu did not open");
		return;
	}
	if (!act_named(g_window, "File", "ShowMenu") ||
		!wait_until([] { return has_named(g_window, "Settings..."); })) {
		test::fail("the File submenu did not open");
		return;
	}
	if (!act_named(g_window, "Settings...", "Press") ||
		!wait_until([] { return has_role(g_window, "dialog", "Settings"); })) {
		test::fail("the Settings dialog did not open");
		return;
	}

	AtspiAccessible *settings = find_role(g_window, "dialog", "Settings");
	if (!settings)
		return;

	AtspiAccessible *icc = find_role(settings, "text", "ICC profile override");
	if (!icc) {
		test::fail("the ICC field is not named by its label");
		g_object_unref(settings);
		return;
	}

	// Where the chooser starts is the directory of what the field holds.
	const string start = string(g_dir) + "/cmyk-lab.icc";
	const string chosen = string(g_dir) + "/black.png";
	CHECK(set_text_contents(icc, start.c_str()));
	if (!act_named(settings, "Browse...", "Press") ||
		!wait_until([] { return has_role(g_window, "dialog", "Open"); })) {
		test::fail("the file chooser did not open");
		g_object_unref(icc);
		g_object_unref(settings);
		return;
	}

	AtspiAccessible *chooser = find_role(g_window, "dialog", "Open");
	if (!chooser) {
		g_object_unref(icc);
		g_object_unref(settings);
		return;
	}

	fprintf(stderr, "--- chooser geometry ---\n");
	dump_geometry(chooser, 0);
	fprintf(stderr, "------------------------\n");

	// The header is three controls, and re-sorting is pressing one of them.
	for (const char *column : {"Name", "Size", "Modified"})
		CHECK(has_named(chooser, column));
	CHECK(has_named(chooser, "Files of type"));
	CHECK(!has_named(chooser, "File name"));
	CHECK(wait_until([chooser] { return has_named(chooser, "cmyk-lab.icc"); }));
	CHECK(act_named(chooser, "Size", "Press"));
	CHECK(wait_until([chooser] { return has_named(chooser, "cmyk-lab.icc"); }));

	AtspiAccessible *rows = find_role(chooser, "list", nullptr);
	if (rows) {
		AtspiAccessible *row = find_one(rows, "cmyk-lab.icc");
		if (row) {
			const gint index =
				atspi_accessible_get_index_in_parent(row, nullptr);
			g_saw_selection_event = g_saw_selected_event = false;
			CHECK(select_child(rows, index));
			CHECK(wait_until(
				[] { return g_saw_selection_event && g_saw_selected_event; }));
			CHECK(has_state(row, ATSPI_STATE_SELECTED));
			CHECK(has_role(g_window, "dialog", "Open"));
			CHECK(act_named(chooser, "Name", "SetFocus"));
			CHECK(has_state(row, ATSPI_STATE_SELECTED));
			CHECK(act_named(chooser, "Name", "Press"));
			CHECK(has_state(row, ATSPI_STATE_SELECTED));
			CHECK(act_named(chooser, "Refresh", "Press"));
			g_object_unref(row);
			row = find_one(rows, "cmyk-lab.icc");
			CHECK(row && has_state(row, ATSPI_STATE_SELECTED));
			g_clear_object(&row);
			CHECK(clear_selection(rows));
		}
		g_object_unref(rows);
	}

	// The path field commits before anything that depends on where we are,
	// and a commit that fails puts back the directory that was listed.
	AtspiAccessible *path = find_role(chooser, "text", nullptr);
	if (!path) {
		test::fail("the chooser has no path field");
	} else {
		const string here = text_contents(path);
		CHECK(here == g_dir);
		CHECK(set_text_contents(path, "/no/such/directory/at/all"));
		CHECK(act_named(chooser, "Refresh", "Press"));
		CHECK(
			wait_until([path, &here] { return text_contents(path) == here; }));
		CHECK(has_named(chooser, "cmyk-lab.icc"));

		// And a commit that works navigates, taking the listing with it.
		const string up = string(g_dir) + "/..";
		CHECK(do_action(path, "SetFocus"));
		CHECK(set_text_contents(path, up.c_str()));
		CHECK(act_named(chooser, "Name", "SetFocus"));
		CHECK(wait_until([] { return has_named(g_window, "fixtures"); }));
		CHECK(!has_named(chooser, "cmyk-lab.icc"));
		CHECK(set_text_contents(path, here.c_str()));
		AtspiAccessible *types = find_role(chooser, "combo box", nullptr);
		if (types) {
			CHECK(do_action(types, "ShowMenu"));
			CHECK(wait_until(
				[] { return has_role(g_window, "list item", "All files"); }));
			{
				AtspiAccessible *item =
					find_role(g_window, "list item", "All files");
				CHECK(item && do_action(item, "Press"));
				g_clear_object(&item);
			}
			g_object_unref(types);
		}
		CHECK(wait_until([] { return has_named(g_window, "cmyk-lab.icc"); }));
		g_object_unref(path);
	}

	// A filter that matches everything overflows the listing, which is the
	// state the rows have to be clipped and scrolled in rather than shrunk
	// to fit: the first is on screen, and the last is not.
	AtspiAccessible *type = find_role(chooser, "combo box", nullptr);
	if (!type) {
		test::fail("the chooser has no type selector");
	} else {
		const bool listed = do_action(type, "ShowMenu") && wait_until([] {
			return has_role(g_window, "list item", "All files");
		});
		if (!listed)
			test::fail("the type list did not open");
		else {
			AtspiAccessible *item =
				find_role(g_window, "list item", "All files");
			CHECK(item && do_action(item, "Press"));
			g_clear_object(&item);
		}
		g_object_unref(type);
		CHECK(
			wait_until([chooser] { return has_named(chooser, "white.png"); }));

		fprintf(stderr, "--- listing overflowing ---\n");
		dump_geometry(chooser, 0);
		fprintf(stderr, "---------------------------\n");

		AtspiAccessible *first = find_one(chooser, "black.png");
		AtspiAccessible *last = find_one(chooser, "white.png");
		if (first && last) {
			const AtspiRect box = extents_of(first, ATSPI_COORD_TYPE_WINDOW);
			// The font's own height, not a share of the listing's.
			CHECK(box.height >= 20);
			// Scrolled out of the viewport, which is the bridge's reading
			// of the offscreen state the kit reports for a clipped row.
			CHECK(has_state(first, ATSPI_STATE_SHOWING));
			CHECK(!has_state(last, ATSPI_STATE_SHOWING));
		}
		g_clear_object(&first);
		g_clear_object(&last);
	}

	// Modal, exactly as the About dialog is: nothing underneath answers,
	// the Settings dialog it stands on included.
	AtspiAccessible *dark = find_one(g_window, "Dark Mode");
	if (dark) {
		CHECK(!has_action(dark, "Press"));
		g_object_unref(dark);
	}
	CHECK(!has_action(icc, "SetFocus"));

	// A prompt over the chooser, which is the whole point of the stack: two
	// dialogs stay up underneath it, and both come back afterwards.
	AtspiAccessible *prompt = nullptr;
	if (act_named(chooser, "New folder", "Press") &&
		wait_until([] { return has_role(g_window, "dialog", "New Folder"); }))
		prompt = find_role(g_window, "dialog", "New Folder");
	if (!prompt) {
		test::fail("the New Folder prompt did not stack");
	} else {
		CHECK(has_named(prompt, "Create"));
		CHECK(has_named(chooser, "cmyk-lab.icc"));
		CHECK(!has_action(icc, "SetFocus"));
		AtspiAccessible *cancel = find_one(chooser, "Cancel");
		if (cancel) {
			CHECK(!has_action(cancel, "Press"));
			CHECK(!do_action(cancel, "Press"));
			g_object_unref(cancel);
		}

		CHECK(act_named(prompt, "Cancel", "Press"));
		g_object_unref(prompt);
		CHECK(wait_until(
			[] { return !has_role(g_window, "dialog", "New Folder"); }));
		CHECK(has_named(chooser, "cmyk-lab.icc"));
	}

	// Pressing a row opens it, as pressing a file in the browser does; the
	// chooser goes, and the dialog it stood on is live again.
	AtspiAccessible *row = find_one(chooser, "black.png");
	g_object_unref(chooser);
	if (!row) {
		test::fail("the listing has no row to press");
	} else {
		CHECK(do_action(row, "Press"));
		g_object_unref(row);
		CHECK(wait_until([] { return !has_role(g_window, "dialog", "Open"); }));
		CHECK(wait_until(
			[icc, &chosen] { return text_contents(icc) == chosen; }));
	}
	g_object_unref(icc);

	CHECK(act_named(settings, "Cancel", "Press"));
	g_object_unref(settings);
	CHECK(wait_until([] { return !has_role(g_window, "dialog", "Settings"); }));

	// And the toolbar answers again once the stack is empty.
	CHECK(wait_until([] {
		AtspiAccessible *button = try_role(g_window, "button", "Dark Mode");
		if (!button)
			return false;
		const bool press = has_action(button, "Press");
		g_object_unref(button);
		return press;
	}));
}

static void
case_export()
{
	if (!g_window || !act_named(g_window, "black.png", "Press"))
		return;
	CHECK(wait_until([] { return has_role(g_window, "image", "black.png"); }));
	CHECK(open_app_menu());
	CHECK(act_named(g_window, "File", "ShowMenu"));
	CHECK(wait_until([] { return has_named(g_window, "Save As..."); }));
	CHECK(act_named(g_window, "Save As...", "Press"));
	CHECK(wait_until([] { return has_role(g_window, "dialog", "Save As"); }));
	AtspiAccessible *chooser = find_role(g_window, "dialog", "Save As");
	if (!chooser)
		return;

	char dir[] = "/tmp/dawn-export-XXXXXX";
	if (!mkdtemp(dir)) {
		test::fail("cannot create export directory");
		g_object_unref(chooser);
		return;
	}
	AtspiAccessible *path = find_role(chooser, "text", "");
	AtspiAccessible *name = find_role(chooser, "text", "File name");
	if (path && name) {
		CHECK(do_action(path, "SetFocus"));
		CHECK(set_text_contents(path, dir));
		CHECK(do_action(name, "SetFocus"));
		CHECK(text_contents(path) == dir);
		CHECK(set_text_contents(name, "missing/result.webp"));
		CHECK(act_named(chooser, "Save", "Press"));
		CHECK(has_role(g_window, "dialog", "Save As"));

		const string output = string(dir) + "/result.webp";
		FILE *file = fopen(output.c_str(), "wb");
		CHECK(file);
		if (file) {
			fputs("keep", file);
			fclose(file);
		}
		CHECK(set_text_contents(name, "result"));
		CHECK(act_named(chooser, "Save", "Press"));
		const char *question = "result.webp already exists. Overwrite it?";
		CHECK(wait_until(
			[question] { return has_role(g_window, "dialog", question); }));
		AtspiAccessible *prompt = find_role(g_window, "dialog", question);
		if (prompt) {
			CHECK(!has_action(name, "SetFocus"));
			CHECK(act_named(prompt, "Cancel", "Press"));
			g_object_unref(prompt);
			CHECK(wait_until([question] {
				return !has_role(g_window, "dialog", question);
			}));
		}
		char header[4] = {};
		file = fopen(output.c_str(), "rb");
		if (file) {
			CHECK(fread(header, 1, 4, file) == 4);
			CHECK(!memcmp(header, "keep", 4));
			fclose(file);
		}
		CHECK(act_named(chooser, "Save", "Press"));
		CHECK(wait_until(
			[question] { return has_role(g_window, "dialog", question); }));
		CHECK(act_named(g_window, "Overwrite", "Press"));
		CHECK(wait_until(
			[] { return !has_role(g_window, "dialog", "Save As"); }));
		file = fopen(output.c_str(), "rb");
		if (file) {
			CHECK(fread(header, 1, 4, file) == 4);
			CHECK(!memcmp(header, "RIFF", 4));
			fclose(file);
		}
		unlink(output.c_str());
	}
	g_clear_object(&path);
	g_clear_object(&name);
	g_object_unref(chooser);
	rmdir(dir);
	CHECK(act_named(g_window, "Browse", "Press"));
	CHECK(wait_until([] { return has_named(g_window, "Filter"); }));
}

static void
case_overflow()
{
	if (!g_window)
		return;

	CHECK(count_named(g_window, "Dark Mode") == 1);
	CHECK(count_named(g_window, "Menu") == 1);

	AtspiAccessible *toolbar = find_role(g_window, "tool bar", nullptr);
	if (!toolbar)
		return;

	AtspiAccessible *filter = find_one(g_window, "Filter");
	if (!filter) {
		g_object_unref(toolbar);
		return;
	}
	const string filter_path = path_of(filter);
	CHECK(!filter_path.empty());

	// Wide enough that nothing is packed away, so the shrink is a real
	// membership change rather than a no-op on an already-narrow bar.
	if (has_named(g_window, "More")) {
		CHECK(compositor_resize("wide"));
		CHECK(wait_until([] { return !has_named(g_window, "More"); }));
	}

	atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_DEFAULT);
	(void) atspi_accessible_get_child_count(toolbar, nullptr);
	CHECK(cached_has_named(toolbar, "Filter"));

	g_saw_children_event = false;
	g_remove_roles.clear();
	if (!listen_children()) {
		g_object_unref(filter);
		g_object_unref(toolbar);
		atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_NONE);
		return;
	}

	CHECK(compositor_resize("shrink"));
	CHECK(wait_until([] { return g_saw_children_event; }));
	CHECK(wait_until([] { return has_named(g_window, "More"); }));

	CHECK(!cached_has_named(toolbar, "Filter"));
	CHECK(count_named(g_window, "Filter") == 0);
	CHECK(name_of(filter) == "Filter");
	CHECK(path_of(filter) == filter_path);

	vector<AtspiAccessible *> more;
	collect_named(g_window, "More", more);
	if (more.size() != 1) {
		test::fail("expected one \"More\", found %zu", more.size());
		unref_all(more);
		g_object_unref(filter);
		g_object_unref(toolbar);
		atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_NONE);
		return;
	}
	CHECK(has_action(more.front(), "ShowMenu"));
	g_saw_children_event = false;
	const bool opened = do_action(more.front(), "ShowMenu") &&
		wait_until([] { return count_named(g_window, "Filter") == 1; });
	if (!opened) {
		test::fail("the overflow menu did not keep a single Filter field");
		unref_all(more);
		g_object_unref(filter);
		g_object_unref(toolbar);
		atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_NONE);
		return;
	}

	CHECK(count_named(g_window, "Filter") == 1);
	CHECK(count_named(g_window, "Dark Mode") == 1);
	CHECK(count_named(g_window, "Menu") == 1);
	// A popup takes the name of what opened it, so while this one is up the
	// button and the menu are two nodes called "More".  Pinned here because
	// every other name in this test is unique, and nothing else would
	// notice if that deliberate pair ever became an accidental duplicate.
	CHECK(count_named(g_window, "More") == 2);

	AtspiAccessible *overflowed = find_one(g_window, "Filter");
	AtspiAccessible *menu = nullptr;
	if (overflowed) {
		CHECK(path_of(overflowed) == filter_path);
		menu = ancestor_below(overflowed, g_window);
		g_object_unref(overflowed);
	}
	if (!menu) {
		test::fail("the overflowed Filter has no popup parent");
		unref_all(more);
		g_object_unref(filter);
		g_object_unref(toolbar);
		atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_NONE);
		return;
	}
	CHECK(role_of(menu) == "popup menu");
	(void) atspi_accessible_get_child_count(menu, nullptr);
	CHECK(cached_has_named(menu, "Filter"));

	g_saw_children_event = false;
	g_remove_roles.clear();
	CHECK(do_action(more.front(), "ShowMenu"));
	CHECK(wait_until([] { return !has_named(g_window, "Filter"); }));
	CHECK(wait_until([] { return g_saw_children_event; }));
	CHECK(!cached_has_named(menu, "Filter"));
	bool from_popup = false;
	bool from_toolbar = false;
	for (const string &role : g_remove_roles) {
		if (role == "popup menu")
			from_popup = true;
		else if (role == "tool bar")
			from_toolbar = true;
	}
	CHECK(from_popup);
	CHECK(!from_toolbar);
	g_object_unref(menu);
	unref_all(more);

	g_saw_children_event = false;
	CHECK(compositor_resize("restore"));
	CHECK(wait_until([] { return !has_named(g_window, "More"); }));
	CHECK(wait_until([] { return count_named(g_window, "Filter") == 1; }));
	CHECK(wait_until([] { return g_saw_children_event; }));

	CHECK(cached_has_named(toolbar, "Filter"));
	CHECK(count_named(g_window, "Dark Mode") == 1);
	AtspiAccessible *restored = find_one(g_window, "Filter");
	if (restored) {
		CHECK(path_of(restored) == filter_path);
		g_object_unref(restored);
	}

	// Resize with the popup still open: its real children must return to
	// the toolbar, keeping both their text and their accessible identity.
	CHECK(compositor_resize("shrink"));
	CHECK(wait_until([] { return has_named(g_window, "More"); }));
	CHECK(act_named(g_window, "More", "ShowMenu"));
	CHECK(wait_until([] { return count_named(g_window, "Filter") == 1; }));
	CHECK(set_text_contents(filter, "overflow-resize"));
	CHECK(wait_until(
		[filter] { return text_contents(filter) == "overflow-resize"; }));
	CHECK(compositor_resize("wide"));
	CHECK(wait_until([] { return !has_named(g_window, "More"); }));
	CHECK(count_named(g_window, "Filter") == 1);
	CHECK(cached_has_named(toolbar, "Filter"));
	CHECK(path_of(filter) == filter_path);
	CHECK(text_contents(filter) == "overflow-resize");
	CHECK(set_text_contents(filter, ""));

	// Reopen after reclaiming, catching stale lender/borrower links.
	CHECK(compositor_resize("shrink"));
	CHECK(wait_until([] { return has_named(g_window, "More"); }));
	CHECK(act_named(g_window, "More", "ShowMenu"));
	CHECK(wait_until([] { return count_named(g_window, "Filter") == 1; }));
	CHECK(path_of(filter) == filter_path);
	CHECK(compositor_resize("restore"));
	CHECK(wait_until([] { return !has_named(g_window, "More"); }));
	CHECK(count_named(g_window, "Filter") == 1);

	g_object_unref(filter);
	g_object_unref(toolbar);
	atspi_accessible_set_cache_mask(g_app, ATSPI_CACHE_NONE);
}

static void
case_teardown()
{
	if (!g_window)
		return;

	// Client-side decorations, which is what a compositor without server-side
	// ones leaves Dawn to draw; the window has no other button to close it.
	AtspiAccessible *close = find_one(g_window, "Close Window");
	if (!close) {
		test::fail("no decoration to close the window with");
		return;
	}

	CHECK(do_action(close, "Press"));
	CHECK(wait_until([] { return !dn_running(); }));

	// The window has gone, and everything pointing into it with it: this has
	// to answer, rather than take the client down.
	atspi_accessible_clear_cache_single(close);
	fprintf(stderr, "stale node after close: \"%s\"\n", name_of(close).c_str());
	CHECK(atspi_accessible_get_child_count(g_window, nullptr) <= 0);
	g_object_unref(close);
}

int
main(int argc, char *argv[])
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s DN-BINARY DIRECTORY\n", argv[0]);
		return 2;
	}
	g_dn = argv[1];
	g_dir = argv[2];

	// However this ends, nothing outlives the test.
	atexit(reap_dn);

	if (atspi_init() > 1) {
		fprintf(stderr, "no accessibility bus\n");
		return 1;
	}

	const int failures = test::run({
		{"startup", case_startup},
		{"panes", case_panes},
		{"button", case_button},
		{"extents", case_extents},
		{"dialog", case_dialog},
		{"settings", case_settings},
		{"location", case_location},
		{"filter", case_filter},
		{"files", case_files},
		{"chooser", case_chooser},
		{"export", case_export},
		{"overflow", case_overflow},
		{"teardown", case_teardown},
	});

	g_clear_object(&g_window);
	g_clear_object(&g_app);
	reap_dn();
	atspi_exit();
	return failures;
}
