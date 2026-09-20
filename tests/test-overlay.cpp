//
// test-overlay.cpp: atlas coordinates across growth during painting
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "overlay.hpp"
#include "test.hpp"

#include <array>

using namespace std;

static void
test_growth()
{
	dn::Sheet atlas(8, true);
	const auto white = atlas.alloc(1, 1);
	const array<uint16_t, 4> pixel{65535, 65535, 65535, 65535};
	atlas.blit(white, pixel.data(), 1, 1, 0);
	const auto glyph = atlas.alloc(3, 4);
	const dn::Uv uv = glyph.texels();
	dn::OverlayList list;
	const float u = float(white.x) + 0.5f;
	const float v = float(white.y) + 0.5f;
	list.begin(100, 100, {u, v, u, v});
	list.add_rect_filled({0, 0, 20, 20}, {1, 1, 1, 1});
	list.add_image({20, 0, 23, 4}, uv, {1, 1, 1, 1});

	// A later draw needs more space. Existing geometry and atlas contents
	// must still address the same texels in the texture uploaded at the end.
	atlas.grow(32);
	CHECK(!atlas.alloc(16, 16).empty());
	list.add_image({30, 0, 33, 4}, glyph.texels(), {1, 1, 1, 1});
	list.end();
	const auto &quads = list.mesh().quads;
	CHECK(quads.size() == 3);
	CHECK(quads[0].uv.u0 == u && quads[0].uv.u1 == u);
	CHECK(quads[0].uv.v0 == v && quads[0].uv.v1 == v);
	CHECK(quads[1].uv.u0 == quads[2].uv.u0);
	CHECK(quads[1].uv.u1 == quads[2].uv.u1);
	CHECK(quads[1].uv.v0 == quads[2].uv.v0);
	CHECK(quads[1].uv.v1 == quads[2].uv.v1);
	CHECK(quads[1].uv.u0 == float(glyph.x));
	CHECK(quads[1].uv.v1 == float(glyph.y + glyph.h));
	const size_t at = (size_t(white.y) * size_t(atlas.w) + size_t(white.x)) * 4;
	CHECK(atlas.pixels[at] == 65535);
	CHECK(atlas.pixels[at + 3] == 65535);
	CHECK(list.mesh().cmds.size() == 1);
	CHECK(list.mesh().cmds.front().tex == dn::kOverlayTexFont);
}

static void
test_dirty_regions()
{
	dn::Sheet atlas(8, true);
	CHECK(atlas.dirty.x == 0 && atlas.dirty.y == 0 && atlas.dirty.w == 8 &&
		atlas.dirty.h == 8);
	atlas.dirty = {};
	const array<uint16_t, 4> pixel{65535, 0, 0, 65535};
	atlas.blit({5, 2, 1, 1}, pixel.data(), 1, 1, 0);
	atlas.blit({2, 4, 1, 1}, pixel.data(), 1, 1, 0);
	CHECK(atlas.dirty.x == 2 && atlas.dirty.y == 2 && atlas.dirty.w == 4 &&
		atlas.dirty.h == 3);
	atlas.grow(16);
	CHECK(atlas.dirty.x == 0 && atlas.dirty.y == 0 && atlas.dirty.w == 16 &&
		atlas.dirty.h == 16);
	CHECK(atlas.pixels[(2 * 16 + 5) * 4] == 65535);
	atlas.clear();
	CHECK(atlas.dirty.empty());
	dn::Sheet logical(8, false);
	CHECK(logical.dirty.empty());
}

static void
test_thumbnails()
{
	dn::Sheet atlas(32, false);
	const auto slot = atlas.alloc(8, 8);
	const dn::Uv uv = slot.texels();
	dn::OverlayList list;
	list.begin(100, 100, {.5f, .5f, .5f, .5f});
	list.add_thumb({0, 0, 16, 16}, uv, {1, 1, 1, 1}, {});
	list.add_rect_filled({20, 0, 30, 10}, {1, 1, 1, 1});
	list.end();
	CHECK(uv.u1 - uv.u0 == 8);
	const auto &quad = list.mesh().quads.front();
	CHECK(quad.uv.u0 == uv.u0);
	CHECK(quad.uv.u1 == uv.u1);
	CHECK(quad.box.x1 - quad.box.x0 == 16);
	CHECK(list.mesh().cmds.size() == 2);
	CHECK(list.mesh().cmds[0].tex == dn::kOverlayTexThumbs);
	CHECK(list.mesh().cmds[1].tex == dn::kOverlayTexFont);
}

static void
test_thumbnail_backgrounds()
{
	dn::OverlayList list;
	const dn::Box box{0, 0, 20, 20};
	const dn::Uv uv{0, 0, 1, 1};
	const dn::Colour white{1, 1, 1, 1};
	dn::ThumbBackground background{white, {}, 7, 9, 5};
	list.begin(100, 100, {});
	list.add_thumb(box, uv, white, background);
	list.add_thumb(box, uv, white, background);
	background.origin_x++;
	list.add_thumb(box, uv, white, background);
	background.size++;
	list.add_thumb(box, uv, white, background);
	background.even.r = .5f;
	list.add_thumb(box, uv, white, background);
	list.push_clip({0, 0, 10, 10});
	list.add_thumb(box, uv, white, background);
	list.pop_clip();
	list.end();
	const auto &cmds = list.mesh().cmds;
	CHECK(cmds.size() == 5);
	CHECK(cmds[0].quad_count == 2);
	CHECK(cmds[0].background.origin_x == 7);
	CHECK(cmds[1].background.origin_x == 8);
	CHECK(cmds[2].background.size == 6);
	CHECK(cmds[3].background.even.r == .5f);
	CHECK(cmds[4].clip.x1 == 10);
	for (size_t i = 1; i < cmds.size(); i++)
		CHECK(cmds[i].quad_offset ==
			cmds[i - 1].quad_offset + cmds[i - 1].quad_count);
}

int
main()
{
	return test::run({
		{"growth during painting", test_growth},
		{"atlas dirty regions", test_dirty_regions},
		{"thumbnail coordinates", test_thumbnails},
		{"thumbnail background batches", test_thumbnail_backgrounds},
	});
}
