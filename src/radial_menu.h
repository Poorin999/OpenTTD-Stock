/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file radial_menu.h The radial menu (design "วงล้อ"): a right click (no drag) on an industry or on open land opens a ring of six actions. */

#ifndef RADIAL_MENU_H
#define RADIAL_MENU_H

#include "tile_type.h"

/** Open the radial menu for this tile at the position of the mouse: an industry tile gives the industry ring, a clear tile the land ring. Other tiles open nothing. */
void ShowRadialMenu(TileIndex tile);

#endif /* RADIAL_MENU_H */
