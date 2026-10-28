/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_map_gui.cpp The zones window: the whole map in coloured areas (one colour per money zone), the borders between the zones as bright lines, the capitals and a legend. A click scrolls the main view there. See zone_market.h. */

#include "stdafx.h"
#include "zone_map_gui.h"
#include "zone_market.h"
#include "tax.h"
#include "central_bank.h"
#include "gold_reserve.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
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

/** Window number inside the class WC_CENTRAL_BANK (0 bank, 1 statement, 2 past statements, 3 buy gold, 4 policy, 5 payroll news, 6 market news, 7 cargo prices, 8 oil market, 9 zones map, 10 headquarters gate, 11 zone taxes). */
static constexpr int ZM_WINDOW_NUMBER = 9;
static constexpr int ZT_WINDOW_NUMBER = 11; ///< The zone tax window (opened by a click on a capital).

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
				GfxFillRect(x - half - 1, y - half - 1, x + half + 1, y + half + 1, GetColourGradient(COLOUR_GREY, SHADE_DARKEST));
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
		DrawString(lx, rx, y, "คลิกช่องเหลืองของเมืองหลวง = ดูภาษีของโซนนั้น", TC_YELLOW, SA_LEFT, false, FS_SMALL);
		y += row_h;
		DrawString(lx, rx, y, "คลิกที่อื่นบนแผนที่เพื่อเลื่อนหน้าจอเกมไปที่นั่น", TC_GREY, SA_LEFT, false, FS_SMALL);
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
		/* A click on the square of a capital opens the taxes of that zone. */
		const int hit = ScaleGUITrad(3) + ScaleGUITrad(4);
		const std::vector<zonemath::Pt> caps = Capitals();
		for (int i = 0; i < _zone_market.count; i++) {
			const int cx = static_cast<int>(caps[i].x * a.side / Map::SizeX());
			const int cy = static_cast<int>(caps[i].y * a.side / Map::SizeY());
			if (std::abs(rx - cx) <= hit && std::abs(ry - cy) <= hit) {
				ShowZoneTaxWindow(i);
				return;
			}
		}
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

/** The zone tax window: what was collected in one zone, and the tax rates. */
struct ZoneTaxWindow : Window {
	int zone = 0; ///< The zone that is shown (0 .. count - 1).

	/** Redraw regularly: the taxes grow every game day. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		this->SetDirty();
	}};

	ZoneTaxWindow(WindowDesc &desc, int zone_number) : Window(desc), zone(zone_number)
	{
		this->InitNested(ZT_WINDOW_NUMBER);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	static std::string MoneyText(int64_t amount)
	{
		return GetString(STR_JUST_CURRENCY_LONG, amount);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_ZT_PANEL) return;

		const int pad = ScaleGUITrad(8);
		const int row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
		const int left = r.left + pad;
		const int right = r.right - pad;
		int y = r.top + pad;

		/* Draw one line when it fits above the bottom of the panel (rule 4 of ROADMAP.md section 3). */
		auto line = [&](const std::string &text, auto colour, int small = 0) {
			if (y + row_h > r.bottom) return;
			DrawString(left, right, y, text, colour, SA_LEFT, false, small != 0 ? FS_SMALL : FS_NORMAL);
			y += row_h;
		};

		if (this->zone < 0 || this->zone >= _zone_market.count) {
			line("ไม่มีโซนนี้ (โซนยังไม่ถูกสร้าง)", TC_GREY);
			return;
		}
		const ZoneState &z = _zone_market.zones[this->zone];
		const Town *capital = nullptr;
		for (const Town *t : Town::Iterate()) {
			if (t->xy == z.capital) { capital = t; break; }
		}

		/* The head: which zone. */
		line(fmt::format("โซน {}  เมืองหลวง {}  ({})", this->zone + 1, capital != nullptr ? GetString(STR_TOWN_NAME, capital->index) : std::string("-"), ZoneCurrencyName(this->zone)), TC_LIGHT_BLUE);
		line(fmt::format("อัตราแลกเปลี่ยน {}.{:03}   ผู้คนในโซน {}", z.rate_ppm / 1'000'000, (z.rate_ppm % 1'000'000) / 1'000, z.population), TC_GREY);
		y += ScaleGUITrad(6);

		/* What was collected in this zone since the start of the game. */
		int64_t all_zones = 0;
		for (int i = 0; i < _zone_market.count; i++) {
			const ZoneState &o = _zone_market.zones[i];
			all_zones += o.land_tax + o.transport_tax + o.vat + o.fees + o.company_tax;
		}
		const int64_t total = z.land_tax + z.transport_tax + z.vat + z.fees + z.company_tax;
		line(fmt::format("ระดับภาษีของโซนนี้: {}% ของอัตราฐาน (100% = อัตราฐาน)", z.tax_percent), z.tax_percent > 100 ? TC_ORANGE : (z.tax_percent < 100 ? TC_GREEN : TC_WHITE));
		y += ScaleGUITrad(4);
		line("ภาษีที่เก็บได้ในโซนนี้ (สะสมตั้งแต่เริ่มเกม)", TC_YELLOW);
		line(fmt::format("  ภาษีที่ดิน (ที่ดินในโซน):  {}", MoneyText(z.land_tax)), TC_WHITE);
		line(fmt::format("  ภาษีขนส่ง (ยานพาหนะที่อยู่ในโซน):  {}", MoneyText(z.transport_tax)), TC_WHITE);
		line(fmt::format("  VAT (ส่งสินค้าเข้าโซนนี้):  {}", MoneyText(z.vat)), TC_WHITE);
		line(fmt::format("  ภาษีบริษัทที่ตั้ง HQ ในโซนนี้ (รายได้ กำไรเทรด ปันผล):  {}", MoneyText(z.company_tax)), TC_WHITE);
		line(fmt::format("  ค่าธรรมเนียมแลกเปลี่ยน (ส่งข้ามเส้นเข้าโซนนี้):  {}", MoneyText(z.fees)), TC_WHITE);
		line(fmt::format("  รวม:  {}   ({}% ของทุกโซน)", MoneyText(total), all_zones > 0 ? total * 100 / all_zones : 0), TC_LIGHT_BLUE);
		y += ScaleGUITrad(6);

		/* The rates that apply in this zone: the base rate x the level of the zone. */
		const int64_t pct = z.tax_percent;
		auto rate1 = [pct](int64_t base) {
			const int64_t x = base * pct / 10; // x10, one decimal
			return fmt::format("{}.{}", x / 10, x % 10);
		};
		line("อัตราภาษีที่ใช้ในโซนนี้ (อัตราฐาน x ระดับภาษีของโซน)", TC_YELLOW);
		line(fmt::format("  ที่ดิน: {} ต่อหน่วยต่อวัน (ใกล้เมืองใหญ่แพงสุด +500%)", rate1(TAX_LAND_PER_UNIT_DAY)), TC_GREY);
		line(fmt::format("  ขนส่งต่อวัน: รถไฟ {}  รถบรรทุก {}  เรือ {}  เครื่องบิน {}", rate1(TAX_TRAIN_DAY), rate1(TAX_ROADVEH_DAY), rate1(TAX_SHIP_DAY), rate1(TAX_AIRCRAFT_DAY)), TC_GREY);
		line(fmt::format("  VAT {}% ของรายได้ส่งสินค้า   กำไรเทรด {}%   เงินปันผล {}%", rate1(TAX_VAT_PERCENT), rate1(TAX_CAPITAL_GAINS_PERCENT), rate1(TAX_DIVIDEND_PERCENT)), TC_GREY);
		line(fmt::format("  ภาษีรายได้ต่อไตรมาส: {}% ถึง {}, {}% ถึง {}, เหนือนั้น {}%", rate1(TAX_INCOME_QUARTER_PERCENT), TAX_INCOME_BRACKET1, rate1(TAX_INCOME_PERCENT_2), TAX_INCOME_BRACKET2, rate1(TAX_INCOME_PERCENT_3)), TC_GREY);
		y += ScaleGUITrad(6);
		/* The central bank of this zone: its own policy, money and gold. The taxes of the zone are paid to it. */
		const CentralBank &bk = BankOfZone(this->zone);
		auto pct_text = [](int64_t bp) {
			return fmt::format("{}{}.{:02}%", bp < 0 ? "-" : "", (bp < 0 ? -bp : bp) / 100, (bp < 0 ? -bp : bp) % 100);
		};
		const int64_t backing = BankBackingBp(bk);
		line(fmt::format("ธนาคารกลางของโซนนี้ ({})", ZoneCurrencyName(this->zone)), TC_YELLOW);
		line(fmt::format("  ช่วงดอกเบี้ยนโยบาย {} - {}   (ส่วนต่างของประเทศ {} bp)", pct_text(PolicyLowerBp(bk.ShiftBp())), pct_text(PolicyUpperBp(bk.ShiftBp())), BANK_ZONE_SPREAD_BP[this->zone]), TC_GREY);
		line(fmt::format("  เงินของธนาคาร {}   ทอง {} oz", MoneyText(bk.trading_revenue_cents / 100), bk.reserve_oz), TC_WHITE);
		line(fmt::format("  ทองหนุนหลังเงินสดในโซน {}   เครดิต {}", pct_text(backing), GetCreditTierName(static_cast<CreditTier>(BankCreditTierIndex(bk)))), TC_WHITE);
		y += ScaleGUITrad(6);
		line("ภาษีเข้าเป็นเงินของธนาคารโซนที่เก็บ ภาษีรายได้ กำไรเทรด ปันผล เข้าธนาคารโซนของ HQ บริษัท", TC_GREY, 1);
		line("เปลี่ยนระดับภาษีด้วยคอนโซล: zone_tax <โซน> <เปอร์เซ็นต์> (เล่นคนเดียว)", TC_GREY, 1);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_zone_tax_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_ZT_CAPTION), SetStringTip(STR_ZONE_TAX_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_ZT_PANEL), SetMinimalSize(660, 560), SetFill(1, 1),
	EndContainer(),
};

static WindowDesc _zone_tax_desc(
	WDP_AUTO, "zone_tax", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_zone_tax_widgets
);

void ShowZoneTaxWindow(int zone)
{
	Window *w = FindWindowById(WC_CENTRAL_BANK, ZT_WINDOW_NUMBER);
	if (w != nullptr) {
		static_cast<ZoneTaxWindow *>(w)->zone = zone;
		w->SetDirty();
		BringWindowToFrontById(WC_CENTRAL_BANK, ZT_WINDOW_NUMBER);
		return;
	}
	new ZoneTaxWindow(_zone_tax_desc, zone);
}
