/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_map_gui.cpp The zones window: the whole map in coloured areas (one colour per money zone), the borders between the zones as bright lines, the capitals and a legend. A click scrolls the main view there. See zone_market.h. */

#include "stdafx.h"
#include "zone_map_gui.h"
#include "zone_market.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "widgets/central_bank_widget.h"
#include "core/format.hpp"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "window_gui.h"
#include "zoom_func.h"
#include "map_func.h"
#include "town.h"
#include "company_base.h"
#include "company_func.h"
#include "hq_gate.h"
#include "viewport_func.h"

#include "table/strings.h"

#include "safeguards.h"

/** Window number inside the class WC_CENTRAL_BANK (0 bank, 1 statement, 2 past statements, 3 buy gold, 4 policy, 5 payroll news, 6 market news, 7 cargo prices, 8 oil market, 9 zones map). */
static constexpr int ZM_WINDOW_NUMBER = 9;

static constexpr int ZM_CELLS = 96;       ///< The map is drawn as ZM_CELLS x ZM_CELLS cells.
static constexpr int ZM_LEGEND_WIDTH = 250; ///< Width of the legend next to the map, in unscaled pixels.

/** The colour of each zone (fill = dark shade, so the bright borders and the capitals stand out). */
static constexpr Colours ZONE_COLOURS[zonemath::MAX_ZONES] = {COLOUR_ORANGE, COLOUR_BLUE, COLOUR_RED, COLOUR_GREEN};

/** Where the square map is inside the panel (a plain struct, so the same numbers serve drawing and clicking). */
struct ZoneMapArea {
	int left = 0;
	int top = 0;
	int side = 0; ///< Width and height of the map in pixels.
};

/** The map takes the biggest square on the left of the panel; the legend gets the strip on the right. */
static ZoneMapArea ComputeMapArea(int left, int top, int width, int height)
{
	const int pad = ScaleGUITrad(8);
	const int legend = ScaleGUITrad(ZM_LEGEND_WIDTH);
	ZoneMapArea a;
	int side = std::min(width - legend - pad * 3, height - pad * 2);
	if (side < ZM_CELLS) side = 0; // Too small: nothing is drawn.
	a.side = side;
	a.left = left + pad;
	a.top = top + pad;
	return a;
}

/** The zones window. */
struct ZoneMapWindow : Window {
	/** Redraw regularly: the rates move every game day. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		this->SetDirty();
	}};

	ZoneMapWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(ZM_WINDOW_NUMBER);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	/** The capitals of the zones as points on the map. */
	static std::vector<zonemath::Pt> Capitals()
	{
		std::vector<zonemath::Pt> caps;
		for (int i = 0; i < _zone_market.count; i++) {
			const TileIndex t = _zone_market.zones[i].capital;
			caps.push_back({static_cast<int64_t>(TileX(t)), static_cast<int64_t>(TileY(t))});
		}
		return caps;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_ZM_PANEL) return;

		const int pad = ScaleGUITrad(8);
		const int row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);

		if (_zone_market.count <= 0) {
			DrawString(r.left + pad, r.right - pad, r.top + pad, "ยังไม่มีโซน: จะสร้างในวันแรกของเกม เมื่อแผนที่มีเมืองห่างกันพอ", TC_GREY, SA_LEFT);
			return;
		}

		const ZoneMapArea a = ComputeMapArea(r.left, r.top, r.Width(), r.Height());
		const std::vector<zonemath::Pt> caps = Capitals();

		if (a.side > 0) {
			/* The zone of every cell (the nearest capital, the same rule as the game). */
			std::vector<int8_t> zone(static_cast<size_t>(ZM_CELLS) * ZM_CELLS, -1);
			for (int cy = 0; cy < ZM_CELLS; cy++) {
				for (int cx = 0; cx < ZM_CELLS; cx++) {
					const int64_t tx = (2 * static_cast<int64_t>(cx) + 1) * Map::SizeX() / (2 * ZM_CELLS);
					const int64_t ty = (2 * static_cast<int64_t>(cy) + 1) * Map::SizeY() / (2 * ZM_CELLS);
					zone[static_cast<size_t>(cy) * ZM_CELLS + cx] = static_cast<int8_t>(zonemath::ZoneOf({tx, ty}, caps));
				}
			}
			auto zone_at = [&](int cx, int cy) -> int {
				if (cx < 0 || cy < 0 || cx >= ZM_CELLS || cy >= ZM_CELLS) return -2;
				return zone[static_cast<size_t>(cy) * ZM_CELLS + cx];
			};
			auto px = [&](int c) { return a.left + c * a.side / ZM_CELLS; };
			auto py = [&](int c) { return a.top + c * a.side / ZM_CELLS; };

			/* 1. the coloured areas */
			for (int cy = 0; cy < ZM_CELLS; cy++) {
				for (int cx = 0; cx < ZM_CELLS; cx++) {
					const int z = zone_at(cx, cy);
					if (z < 0) continue;
					GfxFillRect(px(cx), py(cy), px(cx + 1) - 1, py(cy + 1) - 1, GetColourGradient(ZONE_COLOURS[z], SHADE_DARK));
				}
			}
			/* 2. the borders: a bright line (2 px) wherever two neighbouring cells belong to different zones */
			const auto border = GetColourGradient(COLOUR_WHITE, SHADE_LIGHT);
			for (int cy = 0; cy < ZM_CELLS; cy++) {
				for (int cx = 0; cx < ZM_CELLS; cx++) {
					const int z = zone_at(cx, cy);
					if (z < 0) continue;
					if (zone_at(cx + 1, cy) >= 0 && zone_at(cx + 1, cy) != z) GfxFillRect(px(cx + 1) - 1, py(cy), px(cx + 1), py(cy + 1) - 1, border);
					if (zone_at(cx, cy + 1) >= 0 && zone_at(cx, cy + 1) != z) GfxFillRect(px(cx), py(cy + 1) - 1, px(cx + 1) - 1, py(cy + 1), border);
				}
			}
			/* 3. the frame of the map */
			const auto frame = GetColourGradient(COLOUR_GREY, SHADE_LIGHT);
			GfxFillRect(a.left - 1, a.top - 1, a.left + a.side, a.top - 1, frame);
			GfxFillRect(a.left - 1, a.top + a.side, a.left + a.side, a.top + a.side, frame);
			GfxFillRect(a.left - 1, a.top - 1, a.left - 1, a.top + a.side, frame);
			GfxFillRect(a.left + a.side, a.top - 1, a.left + a.side, a.top + a.side, frame);
			/* 4. the capitals: a yellow square with the number of the zone */
			const int half = ScaleGUITrad(3);
			for (int i = 0; i < _zone_market.count; i++) {
				const int x = a.left + static_cast<int>(caps[i].x * a.side / Map::SizeX());
				const int y = a.top + static_cast<int>(caps[i].y * a.side / Map::SizeY());
				GfxFillRect(x - half - 1, y - half - 1, x + half + 1, y + half + 1, GetColourGradient(COLOUR_GREY, SHADE_BLACK));
				GfxFillRect(x - half, y - half, x + half, y + half, GetColourGradient(COLOUR_YELLOW, SHADE_LIGHT));
				DrawString(x + half + 3, a.left + a.side, y - row_h / 2, fmt::format("{}", i + 1), TC_WHITE, SA_LEFT);
			}
			/* 5. the headquarters of the local company: a white square with a red centre */
			if (Company::IsValidID(_local_company) && Company::Get(_local_company)->location_of_HQ != INVALID_TILE) {
				const TileIndex hq = Company::Get(_local_company)->location_of_HQ;
				const int x = a.left + static_cast<int>(static_cast<int64_t>(TileX(hq)) * a.side / Map::SizeX());
				const int y = a.top + static_cast<int>(static_cast<int64_t>(TileY(hq)) * a.side / Map::SizeY());
				GfxFillRect(x - half - 1, y - half - 1, x + half + 1, y + half + 1, GetColourGradient(COLOUR_WHITE, SHADE_LIGHT));
				GfxFillRect(x - half + 1, y - half + 1, x + half - 1, y + half - 1, GetColourGradient(COLOUR_RED, SHADE_NORMAL));
			}
		} else {
			DrawString(r.left + pad, r.right - pad, r.top + pad, "ขยายหน้าต่างเพื่อดูแผนที่โซน", TC_GREY, SA_LEFT);
		}

		/* The legend: one row per zone. */
		const int lx = a.side > 0 ? a.left + a.side + pad * 2 : r.left + pad;
		const int rx = r.right - pad;
		if (rx <= lx) return;
		int y = r.top + pad;
		DrawString(lx, rx, y, "โซนเงิน (MONEY ZONES)", TC_LIGHT_BLUE, SA_LEFT);
		y += row_h + ScaleGUITrad(4);
		const int sw = GetCharacterHeight(FS_NORMAL);
		for (int i = 0; i < _zone_market.count; i++) {
			const ZoneState &z = _zone_market.zones[i];
			const Town *capital = nullptr;
			for (const Town *t : Town::Iterate()) {
				if (t->xy == z.capital) { capital = t; break; }
			}
			GfxFillRect(lx, y, lx + sw, y + sw, GetColourGradient(ZONE_COLOURS[i], SHADE_DARK));
			GfxFillRect(lx, y, lx + sw, y, GetColourGradient(COLOUR_WHITE, SHADE_LIGHT));
			DrawString(lx + sw + ScaleGUITrad(6), rx, y, fmt::format("{}  {}", i + 1, capital != nullptr ? GetString(STR_TOWN_NAME, capital->index) : std::string("-")), TC_WHITE, SA_LEFT);
			y += row_h;
			DrawString(lx + sw + ScaleGUITrad(6), rx, y, fmt::format("{} {}.{:03}   ผู้คน {}", ZoneCurrencyName(i), z.rate_ppm / 1'000'000, (z.rate_ppm % 1'000'000) / 1'000, z.population), TC_GREY, SA_LEFT);
			y += row_h + ScaleGUITrad(4);
		}
		if (Company::IsValidID(_local_company)) {
			const TileIndex hq = Company::Get(_local_company)->location_of_HQ;
			if (hq == INVALID_TILE) {
				DrawString(lx, rx, y, "HQ ของคุณ: ยังไม่ได้วาง", TC_RED, SA_LEFT);
			} else {
				const int hz = _zone_market.ZoneOfTile(hq);
				DrawString(lx, rx, y, fmt::format("HQ ของคุณ (ช่องแดง): โซน {} ({})", hz + 1, ZoneCurrencyName(hz)), TC_YELLOW, SA_LEFT);
			}
			y += row_h;
		}
		y += ScaleGUITrad(4);
		DrawString(lx, rx, y, "เส้นขาว = เส้นแบ่งโซน ส่งสินค้าข้ามเส้นเสียค่าแลกเปลี่ยน", TC_GREY, SA_LEFT, false, FS_SMALL);
		y += row_h;
		DrawString(lx, rx, y, "คลิกบนแผนที่เพื่อเลื่อนหน้าจอเกมไปที่นั่น", TC_GREY, SA_LEFT, false, FS_SMALL);
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_ZM_PANEL || _zone_market.count <= 0) return;
		const Rect panel = this->GetWidget<NWidgetBase>(WID_ZM_PANEL)->GetCurrentRect(); // window coordinates, like pt
		const ZoneMapArea a = ComputeMapArea(panel.left, panel.top, panel.Width(), panel.Height());
		if (a.side <= 0) return;
		const int rx = pt.x - a.left;
		const int ry = pt.y - a.top;
		if (rx < 0 || ry < 0 || rx >= a.side || ry >= a.side) return;
		const int tx = static_cast<int>(static_cast<int64_t>(rx) * Map::SizeX() / a.side);
		const int ty = static_cast<int>(static_cast<int64_t>(ry) * Map::SizeY() / a.side);
		ScrollMainWindowToTile(TileXY(tx, ty));
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_zone_map_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_ZM_CAPTION), SetStringTip(STR_ZONE_MAP_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_ZM_PANEL), SetMinimalSize(640, 400), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
};

static WindowDesc _zone_map_desc(
	WDP_AUTO, "zone_map", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_zone_map_widgets
);

void ShowZoneMapWindow()
{
	if (BringWindowToFrontById(WC_CENTRAL_BANK, ZM_WINDOW_NUMBER) != nullptr) return;
	new ZoneMapWindow(_zone_map_desc);
}
