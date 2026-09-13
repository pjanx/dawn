//
// volumes-unix.cpp: the filesystem root, for want of a mount list
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <libdn/gettext.hpp>

#include "volumes.hpp"

using namespace std;

namespace dn
{

vector<Volume>
list_volumes()
{
	return {{"/", _("Computer"), "computer-symbolic"}};
}

}  // namespace dn
