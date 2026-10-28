/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_borders.cpp The borders of the money zones in the main game view and the zone under the mouse in the status bar.
 *
 * The zone of a tile is the zone of the nearest capital (zone_market.h). The map is cut into square cells (CELL tiles wide); the zone of a cell is the zone of its middle.
 * Wherever two neighbouring cells belong to different zones, their common edge is a piece of the border. Pieces in a row are joined, so the list of
 * borders is short (a few thousand pieces even on a big map) and is made once, again only when the zones or the map change.
 * Every piece is drawn as a line that follows the height of the ground, in the colours of the two zones next to it (one line each, side by side). */

#include "stdafx.h"
#include "zone_borders.h"
#include "zone_market.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>
#include "core/format.hpp"
#include "gfx_func.h"
#include "landscape.h"
#include "map_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "tile_map.h"
#include "tile_type.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "town.h"
#include "viewport_func.h"
#include "window_func.h"
#include "window_type.h"
#include "zoom_func.h"

#include "table/strings.h"

#include "safeguards.h"

/** One piece of border, in tile coordinates. A piece is straight along x or along y. */
struct BorderPiece {
	int32_t x0, y0, x1, y1;
	int8_t zone_a; ///< The zone on the low side (smaller x or y).
	int8_t zone_b; ///< The zone on the other side.
};

static bool _zone_borders_enabled = true;
static std::vector<BorderPiece> _border_pieces;
static uint64_t _border_key = 0; ///< What the pieces were made from (zones and map size), 0 = nothing made.

bool ZoneBordersEnabled() { return _zone_borders_enabled; }
void SetZoneBordersEnabled(bool on) { _zone_borders_enabled = on; }

/** The colour of the line of a zone. */
static decltype(PC_RED) ZoneLineColour(int zone)
{
	static const decltype(PC_RED) colours[zonemath::MAX_ZONES] = {PC_ORANGE, PC_LIGHT_BLUE, PC_RED, PC_GREEN};
	return colours[std::clamp(zone, 0, zonemath::MAX_ZONES - 1)];
}

/** A number that changes when the zones or the map change. */
static uint64_t CurrentKey()
{
	if (_zone_market.count <= 0) return 0;
	uint64_t k = 1469598103934665603ULL;
	auto mix = [&](uint64_t v) { k = (k ^ v) * 1099511628211ULL; };
	mix(static_cast<uint64_t>(_zone_market.count));
	mix(Map::SizeX());
	mix(Map::SizeY());
	for (int i = 0; i < _zone_market.count; i++) mix(_zone_market.zones[i].capital.base());
	return k == 0 ? 1 : k;
}

/** Make the list of the pieces of border. */
static void BuildBorders()
{
	_border_pieces.clear();
	_border_key = CurrentKey();
	if (_border_key == 0) return;

	const int sx = static_cast<int>(Map::SizeX());
	const int sy = static_cast<int>(Map::SizeY());
	const int cell = std::max(sx, sy) > 2048 ? 4 : 2;
	const int cols = (sx + cell - 1) / cell;
	const int rows = (sy + cell - 1) / cell;

	std::vector<zonemath::Pt> caps;
	for (int i = 0; i < _zone_market.count; i++) {
		const TileIndex t = _zone_market.zones[i].capital;
		caps.push_back({static_cast<int64_t>(TileX(t)), static_cast<int64_t>(TileY(t))});
	}
	std::vector<int8_t> zone(static_cast<size_t>(cols) * rows, -1);
	for (int cy = 0; cy < rows; cy++) {
		for (int cx = 0; cx < cols; cx++) {
			zone[static_cast<size_t>(cy) * cols + cx] = static_cast<int8_t>(zonemath::ZoneOf({static_cast<int64_t>(cx) * cell + cell / 2, static_cast<int64_t>(cy) * cell + cell / 2}, caps));
		}
	}
	auto at = [&](int cx, int cy) -> int { return zone[static_cast<size_t>(cy) * cols + cx]; };

	/* Edges between cells (cx, cy) and (cx + 1, cy): the line x = (cx + 1) * cell, running along y. Joined down each column of edges. */
	for (int cx = 0; cx + 1 < cols; cx++) {
		int cy = 0;
		while (cy < rows) {
			const int a = at(cx, cy), b = at(cx + 1, cy);
			if (a == b) { cy++; continue; }
			int end = cy + 1;
			while (end < rows && at(cx, end) == a && at(cx + 1, end) == b) end++;
			_border_pieces.push_back({(cx + 1) * cell, cy * cell, (cx + 1) * cell, std::min(end * cell, sy), static_cast<int8_t>(a), static_cast<int8_t>(b)});
			cy = end;
		}
	}
	/* Edges between cells (cx, cy) and (cx, cy + 1): the line y = (cy + 1) * cell, running along x. */
	for (int cy = 0; cy + 1 < rows; cy++) {
		int cx = 0;
		while (cx < cols) {
			const int a = at(cx, cy), b = at(cx, cy + 1);
			if (a == b) { cx++; continue; }
			int end = cx + 1;
			while (end < cols && at(end, cy) == a && at(end, cy + 1) == b) end++;
			_border_pieces.push_back({cx * cell, (cy + 1) * cell, std::min(end * cell, sx), (cy + 1) * cell, static_cast<int8_t>(a), static_cast<int8_t>(b)});
			cx = end;
		}
	}
}

/** The place on the screen (in the pixels of the view, before the scroll of the view is taken off) of a point of the ground, in tile units. */
static Point ScreenOfGround(int tx, int ty, ZoomLevel zoom)
{
	const int wx = std::clamp<int>(tx * TILE_SIZE, 0, static_cast<int>(Map::MaxX()) * TILE_SIZE - 1);
	const int wy = std::clamp<int>(ty * TILE_SIZE, 0, static_cast<int>(Map::MaxY()) * TILE_SIZE - 1);
	const Point p = RemapCoords(wx, wy, GetSlopePixelZ(wx, wy, false));
	return {UnScaleByZoom(p.x, zoom), UnScaleByZoom(p.y, zoom)};
}

void ZoneBordersDraw(ZoomLevel zoom)
{
	if (!_zone_borders_enabled || _zone_market.count <= 0) return;
	if (_border_key != CurrentKey()) BuildBorders();
	if (_border_pieces.empty()) return;

	const DrawPixelInfo *dpi = _cur_dpi;
	/* The visible part, in the same pixels as ScreenOfGround(). The margin is for hills (a point on a high hill is drawn far above the flat ground). */
	const int margin = UnScaleByZoom(1200, zoom) + 64;
	const int left = dpi->left - margin, right = dpi->left + dpi->width + margin;
	const int top = dpi->top - margin, bottom = dpi->top + dpi->height + margin;

	const int step = std::max(1, ScaleByZoom(1, zoom)); // Tiles between two points of a line: coarser when zoomed out.
	for (const BorderPiece &b : _border_pieces) {
		/* Quick test with the two ends on flat ground; pieces that are far off screen cost nothing more. */
		const int fx0 = UnScaleByZoom((b.y0 - b.x0) * 2 * ZOOM_BASE * TILE_SIZE, zoom), fy0 = UnScaleByZoom((b.y0 + b.x0) * ZOOM_BASE * TILE_SIZE, zoom);
		const int fx1 = UnScaleByZoom((b.y1 - b.x1) * 2 * ZOOM_BASE * TILE_SIZE, zoom), fy1 = UnScaleByZoom((b.y1 + b.x1) * ZOOM_BASE * TILE_SIZE, zoom);
		if (std::max(fx0, fx1) < left || std::min(fx0, fx1) > right || std::max(fy0, fy1) < top || std::min(fy0, fy1) > bottom) continue;

		const int len = std::max(b.x1 - b.x0, b.y1 - b.y0);
		const int dx = b.x1 > b.x0 ? 1 : 0;
		const int dy = b.y1 > b.y0 ? 1 : 0;
		const auto ca = ZoneLineColour(b.zone_a);
		const auto cb = ZoneLineColour(b.zone_b);
		Point prev = ScreenOfGround(b.x0, b.y0, zoom);
		for (int t = step;; t += step) {
			const int u = std::min(t, len);
			const Point cur = ScreenOfGround(b.x0 + dx * u, b.y0 + dy * u, zoom);
			/* Two lines side by side, one pixel apart: the colour of each zone on its own side. */
			GfxDrawLine(prev.x, prev.y - 1, cur.x, cur.y - 1, ca, 1);
			GfxDrawLine(prev.x, prev.y, cur.x, cur.y, cb, 1);
			prev = cur;
			if (u >= len) break;
		}
	}
}

/** The zone of the tile under the mouse, or -1 (the mouse is not over the map, or there are no zones). */
static int ZoneUnderMouse(TileIndex *tile_out = nullptr)
{
	if (_zone_market.count <= 0) return -1;
	const Point pt = GetTileBelowCursor();
	if (pt.x == -1) return -1;
	const TileIndex tile = TileVirtXY(pt.x, pt.y);
	if (tile_out != nullptr) *tile_out = tile;
	return _zone_market.ZoneOfTile(tile);
}

std::string ZoneStatusText(const std::string &base)
{
	const int zone = ZoneUnderMouse();
	if (zone < 0) return base;
	const ZoneState &z = _zone_market.zones[zone];
	std::string capital;
	for (const Town *t : Town::Iterate()) {
		if (t->xy == z.capital) { capital = GetString(STR_TOWN_NAME, t->index); break; }
	}
	return fmt::format("{}   |   โซน {} ({}) {}  rate {}.{:03}", base, zone + 1, ZoneCurrencyName(zone), capital, z.rate_ppm / 1'000'000, (z.rate_ppm % 1'000'000) / 1'000);
}

/** Redraw the status bar when the mouse moves into another zone (4 times a second at most; real time, so it also works while the game is paused). */
static const IntervalTimer<TimerWindow> _zone_status_timer = {std::chrono::milliseconds(250), [](auto)
{
	static int last_zone = -2;
	const int zone = ZoneUnderMouse();
	if (zone == last_zone) return;
	last_zone = zone;
	SetWindowDirty(WC_STATUS_BAR, 0);
}};
