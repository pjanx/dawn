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
	const auto &vertices = list.mesh().vertices;
	CHECK(vertices.size() == 12);
	for (size_t i = 0; i < 4; i++) {
		CHECK(vertices[i].u == u);
		CHECK(vertices[i].v == v);
		CHECK(vertices[i + 4].u == vertices[i + 8].u);
		CHECK(vertices[i + 4].v == vertices[i + 8].v);
	}
	CHECK(vertices[4].u == float(glyph.x));
	CHECK(vertices[6].v == float(glyph.y + glyph.h));
	const size_t at = (size_t(white.y) * size_t(atlas.w) + white.x) * 4;
	CHECK(atlas.pixels[at] == 65535);
	CHECK(atlas.pixels[at + 3] == 65535);
	CHECK(list.mesh().cmds.size() == 1);
	CHECK(list.mesh().cmds.front().tex == dn::kOverlayTexFont);
}

static void
test_thumbnails()
{
	dn::Sheet atlas(32, false);
	const auto slot = atlas.alloc(8, 8);
	const dn::Uv uv = atlas.uv(slot);
	dn::OverlayList list;
	list.begin(100, 100, {.5f, .5f, .5f, .5f});
	list.add_thumb({0, 0, 16, 16}, uv, {1, 1, 1, 1}, {});
	list.add_rect_filled({20, 0, 30, 10}, {1, 1, 1, 1});
	list.end();
	CHECK(uv.u1 - uv.u0 == .25f);
	const auto &vertex = list.mesh().vertices.front();
	CHECK(vertex.u == uv.u0);
	CHECK(vertex.atlas_x1 == uv.u1);
	CHECK(vertex.dest_w == 16);
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
	CHECK(cmds[0].idx_count == 12);
	CHECK(cmds[0].background.origin_x == 7);
	CHECK(cmds[1].background.origin_x == 8);
	CHECK(cmds[2].background.size == 6);
	CHECK(cmds[3].background.even.r == .5f);
	CHECK(cmds[4].clip.x1 == 10);
	for (size_t i = 1; i < cmds.size(); i++)
		CHECK(cmds[i].idx_offset ==
			cmds[i - 1].idx_offset + cmds[i - 1].idx_count);
}

int
main()
{
	return test::run({
		{"growth during painting", test_growth},
		{"thumbnail coordinates", test_thumbnails},
		{"thumbnail background batches", test_thumbnail_backgrounds},
	});
}
