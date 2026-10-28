/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_map_gui.h The zones window: the map divided into the money zones, one colour per zone, the borders as bright lines. Opened by the Zones button of the market news window or by the console command zones_map. */

#ifndef ZONE_MAP_GUI_H
#define ZONE_MAP_GUI_H

/** Open (or bring to the front) the zones window. */
void ShowZoneMapWindow();

/** Open (or bring to the front) the window with the taxes of one zone (0 .. count - 1). Opened by a click on a capital in the zones window. */
void ShowZoneTaxWindow(int zone);

#endif /* ZONE_MAP_GUI_H */
