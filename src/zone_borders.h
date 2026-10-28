/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_borders.h The borders of the money zones in the main game view (coloured lines on the ground) and the zone under the mouse in the status bar. See zone_market.h. */

#ifndef ZONE_BORDERS_H
#define ZONE_BORDERS_H

#include <string>
#include "zoom_type.h"

/** Whether the border lines are drawn in the game view. */
bool ZoneBordersEnabled();

/** Turn the border lines on or off (console command zone_lines). */
void SetZoneBordersEnabled(bool on);

/**
 * Draw the borders. Called by ViewportDoDraw() (viewport.cpp, patched by apply_overlay.py) with the drawing area in screen pixels
 * (_cur_dpi->left and top are already the pixel position of the view, the same as for the signs and the town names).
 * @param zoom The zoom level of the view.
 */
void ZoneBordersDraw(ZoomLevel zoom);

/**
 * The text of the middle part of the status bar (statusbar_gui.cpp, patched by apply_overlay.py): the text that the game wants to show
 * and, when the mouse is over the map, the zone of that spot.
 */
std::string ZoneStatusText(const std::string &base);

#endif /* ZONE_BORDERS_H */
