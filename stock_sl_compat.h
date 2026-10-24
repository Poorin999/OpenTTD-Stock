/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_sl_compat.h Loading of stock chunks. */

#ifndef SAVELOAD_COMPAT_STOCK_H
#define SAVELOAD_COMPAT_STOCK_H

#include "../saveload.h"

/** Original field order for _stock_desc. */
const SaveLoadCompat _stock_sl_compat[] = {
	SLC_VAR("kind"),
	SLC_VAR("a"),
	SLC_VAR("b"),
	SLC_VAR("ym"),
	SLC_VAR("v1"),
	SLC_VAR("v2"),
	SLC_VAR("v3"),
	SLC_VAR("v4"),
	SLC_VAR("v5"),
};

#endif /* SAVELOAD_COMPAT_STOCK_H */
