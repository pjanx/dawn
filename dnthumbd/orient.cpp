//
// orient.cpp: EXIF orientation bake-in
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "orient.hpp"

#include <cmath>
#include <cstring>
#include <vector>

using namespace std;

namespace dnthumbd {
namespace {

bool invert_matrix(const dn::Matrix &m, dn::Matrix *out)
{
	const double det = m.xx * m.yy - m.xy * m.yx;
	if (fabs(det) < 1e-12)
		return false;
	out->xx = m.yy / det;
	out->xy = -m.xy / det;
	out->yx = -m.yx / det;
	out->yy = m.xx / det;
	out->x0 = -(out->xx * m.x0 + out->xy * m.y0);
	out->y0 = -(out->yx * m.x0 + out->yy * m.y0);
	return true;
}

void map_point(const dn::Matrix &m, double x, double y, double *ox, double *oy)
{
	*ox = m.xx * x + m.xy * y + m.x0;
	*oy = m.yx * x + m.yy * y + m.y0;
}

void copy_pixel_bgra16(uint16_t *dst, const uint16_t *src)
{
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
	dst[3] = src[3];
}

} // namespace

bool bake_orientation(dn::Image &image)
{
	using dn::Orientation;

	if (image.width == 0 || image.height == 0 || image.data.empty())
		return false;

	Orientation orient = image.orientation;
	if (orient == Orientation::Unknown)
		orient = Orientation::Rotate0;
	if (orient == Orientation::Rotate0)
		return true;

	double nw = 0, nh = 0;
	dn::orientation_dimensions(image, orient, &nw, &nh);
	const uint32_t out_w = uint32_t(nw);
	const uint32_t out_h = uint32_t(nh);
	if (out_w == 0 || out_h == 0 || out_w > dn::kMaxDimension ||
	    out_h > dn::kMaxDimension)
		return false;

	const uint32_t in_w = image.width;
	const uint32_t in_h = image.height;
	const size_t out_stride = size_t(out_w) * dn::kBytesPerPixel;
	if (out_h > SIZE_MAX / out_stride)
		return false;

	dn::Matrix fwd = dn::orientation_matrix(orient, nw, nh);
	dn::Matrix inv{};
	if (!invert_matrix(fwd, &inv))
		return false;

	vector<uint8_t> out_data;
	try {
		out_data.assign(out_stride * out_h, 0);
	} catch (const bad_alloc &) {
		return false;
	}

	for (uint32_t y = 0; y < out_h; y++) {
		uint16_t *dst_row = (uint16_t *)(out_data.data() + y * out_stride);
		for (uint32_t x = 0; x < out_w; x++) {
			double sx = 0, sy = 0;
			map_point(inv, double(x) + 0.5, double(y) + 0.5, &sx, &sy);
			const int ix = int(floor(sx));
			const int iy = int(floor(sy));
			if (ix < 0 || iy < 0 || ix >= int(in_w) || iy >= int(in_h)) {
				dst_row[0] = dst_row[1] = dst_row[2] = dst_row[3] = 0;
			} else {
				const uint16_t *src =
					dn::row_u16(image, uint32_t(iy)) + size_t(ix) * 4;
				copy_pixel_bgra16(dst_row, src);
			}
			dst_row += 4;
		}
	}

	image.data = std::move(out_data);
	image.width = out_w;
	image.height = out_h;
	image.stride = uint32_t(out_stride);
	image.orientation = Orientation::Rotate0;
	return true;
}

} // namespace dnthumbd
