/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargo_market_gui.cpp The cargo prices window (it takes the place of the cargo payment rates graph of OpenTTD). See cargo_market_gui.h, cargo_market.h and CargoMarketMath.h.
 * Three tabs: market overview (table with base price, price now, index, change over the chosen period and a trend line, plus the top 5 rising and falling cargo),
 * price graph and price history of one cargo. All numbers are the real prices of the game; the history of the price (one value per game day, 366 days) is kept in
 * _cargo_market.history and is not saved. */

#include "stdafx.h"
#include "cargo_market_gui.h"
#include "cargo_market.h"
#include "cargotype.h"
#include "economy_func.h"
#include "town.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include "dropdown_func.h"
#include "dropdown_type.h"
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

#include "table/strings.h"

#include "safeguards.h"

/** Window number inside the class WC_CENTRAL_BANK (0 bank, 1 statement, 2 past statements, 3 buy gold, 4 policy, 5 payroll news, 6 market news, 7 cargo prices). */
static constexpr int CP_WINDOW_NUMBER = 7;

static constexpr uint CARGO_REF_UNITS = 10; ///< The prices are for this many units ...
static constexpr uint CARGO_REF_TILES = 20; ///< ... carried this many tiles, with no time lost on the way.

static constexpr int PERIOD_COUNT = 4;
static constexpr uint32_t PERIOD_DAYS[PERIOD_COUNT] = {7, 30, 90, 365}; ///< The periods of the dropdown, in game days.

static const char *PeriodLabel(int i)
{
	static const char *const labels[PERIOD_COUNT] = {"1 สัปดาห์", "1 เดือน", "3 เดือน", "1 ปี"};
	return labels[std::clamp(i, 0, PERIOD_COUNT - 1)];
}

static const char *PeriodUnit(int i)
{
	static const char *const units[PERIOD_COUNT] = {"สัปดาห์", "เดือน", "3 เดือน", "ปี"};
	return units[std::clamp(i, 0, PERIOD_COUNT - 1)];
}

/** A percentage of a ppm value with two decimals, for example 103.45%. */
static std::string PercentText(int64_t ppm)
{
	const int64_t v = ppm < 0 ? -ppm : ppm;
	return fmt::format("{}{}.{:02}%", ppm < 0 ? "-" : "", v / 10000, v % 10000 / 100);
}

/** A price index with one decimal, for example 102.4%. */
static std::string IndexText(int64_t ppm)
{
	return fmt::format("{}.{}%", ppm / 10000, ppm % 10000 / 1000);
}

/** A change in tenths of a percent, with its sign, for example +2.4% or -15.2%. */
static std::string ChangeText(int64_t x10)
{
	const int64_t v = x10 < 0 ? -x10 : x10;
	return fmt::format("{}{}.{}%", x10 < 0 ? "-" : "+", v / 10, v % 10);
}

/** More pay is good for the player: green above the normal pay, red below it. */
static TextColour PriceColour(int64_t ppm)
{
	if (ppm > cargomarket::PPM + 5000) return TC_GREEN;
	if (ppm < cargomarket::PPM - 5000) return TC_RED;
	return TC_WHITE;
}

static TextColour ChangeColour(bool has_change, int64_t x10)
{
	if (!has_change || (x10 < 1 && x10 > -1)) return TC_GREY;
	return x10 > 0 ? TC_GREEN : TC_RED;
}

/** A small filled triangle, pointing up or down, centred on cx, cy. */
static void DrawTriangle(int cx, int cy, int half, bool up, bool green)
{
	const auto colour = GetColourGradient(green ? COLOUR_GREEN : COLOUR_RED, SHADE_NORMAL);
	for (int j = 0; j <= half; j++) {
		const int hw = up ? j : half - j;
		const int y = cy - half / 2 + j;
		GfxFillRect(cx - hw, y, cx + hw, y, colour);
	}
}

/** One row of the tables: a cargo with its prices and its change over the chosen period. */
struct PriceRow {
	const CargoSpec *cs = nullptr;
	size_t index = 0;
	bool gold = false;           ///< Priced by the gold market: no history here.
	int64_t shown_ppm = 0;       ///< Price times freight rate, 1.0 = the normal pay.
	Money current = 0;           ///< What 10 units carried 20 tiles pay now.
	Money base = 0;              ///< The same at the normal pay (index 100%).
	bool has_change = false;     ///< False while there is less than 2 days of history.
	int64_t change_x10 = 0;      ///< Change over the period, in tenths of a percent.
	Money diff = 0;              ///< Change of the money value over the period.
	uint32_t back = 0;           ///< Days of history that the change looks back.
};

/** The cargo prices window. */
struct CargoPricesWindow : Window {
	int tab = 0;                 ///< 0 overview, 1 price graph, 2 price history.
	int period = 1;              ///< Index into PERIOD_DAYS.
	int filter = 0;              ///< 0 = all cargo, else cargo index + 1.
	int sel = -1;                ///< The cargo index chosen for the graph and the history, -1 = the first one.

	/** Redraw regularly: the prices move every game day. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		this->SetDirty();
	}};

	CargoPricesWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(CP_WINDOW_NUMBER);
		this->UpdateTabs();
	}

	void UpdateTabs()
	{
		this->SetWidgetLoweredState(WID_CP_TAB_OVERVIEW, this->tab == 0);
		this->SetWidgetLoweredState(WID_CP_TAB_GRAPH, this->tab == 1);
		this->SetWidgetLoweredState(WID_CP_TAB_HISTORY, this->tab == 2);
		this->SetDirty();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	/** Number of cargo types that can be shown. */
	static int CargoCount()
	{
		int n = 0;
		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			if (static_cast<size_t>(cs->Index()) < _cargo_market.cargo.size()) n++;
		}
		return n;
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_CP_FILTER) {
			std::string text;
			if (this->filter == 0) {
				text = fmt::format("แสดง: สินค้าทั้งหมด ({})", CargoCount());
			} else {
				text = "แสดง: ";
				for (const CargoSpec *cs : CargoSpec::Iterate()) {
					if (static_cast<size_t>(cs->Index()) + 1 == static_cast<size_t>(this->filter)) text += GetString(cs->name);
				}
			}
			return GetString(STR_CARGO_PRICES_DROPDOWN, std::move(text));
		}
		if (widget == WID_CP_PERIOD) {
			return GetString(STR_CARGO_PRICES_DROPDOWN, fmt::format("ช่วงเวลา: {}", PeriodLabel(this->period)));
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	/** One row per cargo, with the prices and the change over the period. */
	std::vector<PriceRow> BuildRows(bool apply_filter) const
	{
		std::vector<PriceRow> rows;
		const uint32_t avail = _cargo_market.HistoryAvailable();
		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			const size_t i = static_cast<size_t>(cs->Index());
			if (i >= _cargo_market.cargo.size()) continue;
			if (apply_filter && this->filter != 0 && static_cast<size_t>(this->filter) != i + 1) continue;
			PriceRow row;
			row.cs = cs;
			row.index = i;
			const CargoType ct = cs->Index();
			row.gold = CargoMarketIsGoldCargo(ct);
			row.current = GetTransportedGoodsIncome(CARGO_REF_UNITS, CARGO_REF_TILES, 0, ct);
			row.base = row.current;
			if (!row.gold) {
				const cargomarket::CargoState &c = _cargo_market.cargo[i];
				row.shown_ppm = c.price_ppm * c.freight_ppm / cargomarket::PPM;
				if (row.shown_ppm > 0) row.base = row.current * cargomarket::PPM / row.shown_ppm;
				if (avail >= 2 && row.shown_ppm > 0) {
					row.back = std::min<uint32_t>(PERIOD_DAYS[this->period], avail - 1);
					const int64_t old = _cargo_market.HistoryPpm(i, row.back);
					if (old > 0) {
						row.has_change = true;
						row.change_x10 = (row.shown_ppm - old) * 1000 / old;
						row.diff = row.current - row.current * old / row.shown_ppm;
					}
				}
			}
			rows.push_back(row);
		}
		return rows;
	}

	/** The cargo chosen for the graph and the history (the first one that has a price if none is chosen). */
	size_t SelectedIndex(const std::vector<PriceRow> &rows) const
	{
		for (const PriceRow &row : rows) {
			if (this->sel >= 0 && row.index == static_cast<size_t>(this->sel)) return row.index;
		}
		for (const PriceRow &row : rows) {
			if (!row.gold) return row.index;
		}
		return rows.empty() ? 0 : rows.front().index;
	}

	/** Where the list of cargo is: used both for drawing and for clicks. */
	struct ListGeom { int left, right, top, bottom, row_h; };

	ListGeom ListGeometry(const Rect &r) const
	{
		const int pad = ScaleGUITrad(6);
		const int row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
		ListGeom g;
		g.row_h = row_h;
		g.left = r.left + pad;
		g.right = this->tab == 0 ? r.left + (r.right - r.left) * 62 / 100 - pad / 2 : r.left + pad + ScaleGUITrad(250);
		g.top = r.top + pad + (row_h + ScaleGUITrad(6)) + (row_h + ScaleGUITrad(4));
		g.bottom = r.bottom - pad - (this->tab == 0 ? row_h + ScaleGUITrad(6) : 0);
		return g;
	}

	void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		switch (widget) {
			case WID_CP_TAB_OVERVIEW: this->tab = 0; this->UpdateTabs(); break;
			case WID_CP_TAB_GRAPH: this->tab = 1; this->UpdateTabs(); break;
			case WID_CP_TAB_HISTORY: this->tab = 2; this->UpdateTabs(); break;

			case WID_CP_FILTER: {
				DropDownList list;
				list.push_back(MakeDropDownListStringItem(fmt::format("สินค้าทั้งหมด ({})", CargoCount()), 0));
				for (const CargoSpec *cs : CargoSpec::Iterate()) {
					const size_t i = static_cast<size_t>(cs->Index());
					if (i >= _cargo_market.cargo.size()) continue;
					list.push_back(MakeDropDownListStringItem(GetString(cs->name), static_cast<int>(i) + 1));
				}
				ShowDropDownList(this, std::move(list), this->filter, WID_CP_FILTER, 0);
				break;
			}

			case WID_CP_PERIOD: {
				DropDownList list;
				for (int i = 0; i < PERIOD_COUNT; i++) list.push_back(MakeDropDownListStringItem(std::string(PeriodLabel(i)), i));
				ShowDropDownList(this, std::move(list), this->period, WID_CP_PERIOD, 0);
				break;
			}

			case WID_CP_CLOSE:
				this->Close();
				return;

			case WID_CP_PANEL: {
				const Rect r = this->GetWidget<NWidgetBase>(WID_CP_PANEL)->GetCurrentRect();
				const ListGeom g = this->ListGeometry(r);
				if (pt.x < g.left || pt.x > g.right || pt.y < g.top || pt.y >= g.bottom) break;
				const std::vector<PriceRow> rows = this->BuildRows(this->tab == 0);
				const size_t k = static_cast<size_t>((pt.y - g.top) / g.row_h);
				if (k >= rows.size()) break;
				this->sel = static_cast<int>(rows[k].index);
				/* A double click on the overview opens the graph of that cargo. */
				if (this->tab == 0 && click_count >= 2) {
					this->tab = 1;
					this->UpdateTabs();
				}
				this->SetDirty();
				break;
			}

			default: break;
		}
	}

	void OnDropdownSelect(WidgetID widget, int index, [[maybe_unused]] int click_result) override
	{
		if (widget == WID_CP_FILTER && index >= 0) {
			this->filter = index;
			this->SetDirty();
		} else if (widget == WID_CP_PERIOD && index >= 0 && index < PERIOD_COUNT) {
			this->period = index;
			this->SetDirty();
		}
	}

	/* ---- Drawing ------------------------------------------------------------------------------------------------------------------- */

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_CP_HEADER: this->DrawHeader(r); break;
			case WID_CP_FOOT: this->DrawFoot(r); break;
			case WID_CP_PANEL:
				if (this->tab == 0) this->DrawOverview(r);
				else if (this->tab == 1) this->DrawGraph(r);
				else this->DrawHistory(r);
				break;
			default: break;
		}
	}

	void DrawHeader(const Rect &r) const
	{
		const int pad = ScaleGUITrad(8);
		const int left = r.left + pad;
		const int right = r.right - pad;
		if (right <= left) return;
		DrawString(left, right, r.top + ScaleGUITrad(4), "ราคาสินค้าและอันดับการเปลี่ยนแปลง", TC_WHITE, SA_LEFT, false, FS_LARGE);
		DrawString(left, right, r.top + ScaleGUITrad(4) + GetCharacterHeight(FS_LARGE) + ScaleGUITrad(3),
				fmt::format("100% = ค่าขนส่งปกติของ OpenTTD • ขนส่ง {} หน่วย ระยะทาง {} ช่อง", CARGO_REF_UNITS, CARGO_REF_TILES), TC_GREY, SA_LEFT);
	}

	void DrawFoot(const Rect &r) const
	{
		const int pad = ScaleGUITrad(8);
		const int left = r.left + pad;
		const int right = r.right - pad;
		if (right <= left) return;
		const int line = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(1);
		DrawString(left, right, r.top + ScaleGUITrad(3), "(1) ราคาฐานอ้างอิงจากค่าขนส่งปกติของ OpenTTD   (2) เปลี่ยนแปลงเทียบกับช่วงเวลาที่เลือก   (3) เส้นแนวโน้มของช่วงเวลาที่เลือก", TC_GREY, SA_LEFT);
		DrawString(left, right, r.top + ScaleGUITrad(3) + line, "ทองคำใช้ราคาตลาดทอง ไม่รวมในดัชนี • ประวัติราคานับตั้งแต่เปิดเกม (ไม่ถูกบันทึกในไฟล์เซฟ)", TC_GREY, SA_LEFT);
	}

	/** A trend line of the last days of one cargo, inside the given box. */
	void DrawSparkline(int x0, int y0, int x1, int y1, const PriceRow &row) const
	{
		if (!row.has_change || row.back < 1 || x1 - x0 < 4) return;
		const uint32_t n = std::min<uint32_t>(row.back + 1, 40);
		std::vector<int64_t> v;
		v.reserve(n);
		for (uint32_t k = 0; k < n; k++) {
			const uint32_t days_ago = row.back - static_cast<uint32_t>(static_cast<uint64_t>(k) * row.back / (n - 1));
			v.push_back(_cargo_market.HistoryPpm(row.index, days_ago));
		}
		const int64_t mn = *std::min_element(v.begin(), v.end());
		const int64_t mx = *std::max_element(v.begin(), v.end());
		const auto colour = GetColourGradient(row.change_x10 > 0 ? COLOUR_GREEN : (row.change_x10 < 0 ? COLOUR_RED : COLOUR_GREY), SHADE_NORMAL);
		int px = 0, py = 0;
		for (uint32_t k = 0; k < n; k++) {
			const int x = x0 + static_cast<int>(static_cast<int64_t>(x1 - x0) * k / (n - 1));
			const int y = mx > mn ? y1 - static_cast<int>(static_cast<int64_t>(y1 - y0) * (v[k] - mn) / (mx - mn)) : (y0 + y1) / 2;
			if (k > 0) GfxDrawLine(px, py, x, y, colour);
			px = x;
			py = y;
		}
	}

	void DrawOverview(const Rect &r) const
	{
		const int pad = ScaleGUITrad(6);
		const ListGeom g = this->ListGeometry(r);
		const int row_h = g.row_h;
		const int band_h = row_h + ScaleGUITrad(6);
		const int hdr_h = row_h + ScaleGUITrad(4);
		const int box_top = r.top + pad;
		const std::vector<PriceRow> rows = this->BuildRows(true);
		const std::vector<PriceRow> all = this->BuildRows(false);
		const size_t sel = this->SelectedIndex(all);

		/* Title band of the table. */
		GfxFillRect(g.left, box_top, g.right, box_top + band_h - 1, GREY_SCALE(3));
		DrawString(g.left + 6, g.right - 6, box_top + 3, "รายการราคาสินค้า", TC_WHITE, SA_LEFT);
		DrawString(g.left + 6, g.right - 6, box_top + 3, fmt::format("{} รายการ", rows.size()), TC_GREY, SA_RIGHT);

		/* Column headers. */
		const int w = g.right - g.left;
		const int name_r = g.left + w * 21 / 100;
		const int base_r = g.left + w * 36 / 100;
		const int cur_r = g.left + w * 53 / 100;
		const int idx_r = g.left + w * 67 / 100;
		const int chg_r = g.left + w * 83 / 100;
		const int spark_l = g.left + w * 85 / 100;
		const int spark_r = g.right - 8;
		const int hy = box_top + band_h + 1;
		GfxFillRect(g.left, box_top + band_h, g.right, g.top - 1, GREY_SCALE(2));
		DrawString(g.left + 6, name_r, hy, "สินค้า", TC_GREY, SA_LEFT);
		DrawString(name_r, base_r, hy, "ราคาฐาน (1)", TC_GREY, SA_RIGHT);
		DrawString(base_r, cur_r, hy, "ราคาปัจจุบัน", TC_GREY, SA_RIGHT);
		DrawString(cur_r, idx_r, hy, "ดัชนีราคา", TC_GREY, SA_RIGHT);
		DrawString(idx_r, chg_r, hy, "เปลี่ยนแปลง (2)", TC_GREY, SA_RIGHT);
		DrawString(spark_l, spark_r, hy, "แนวโน้ม (3)", TC_GREY, SA_LEFT);

		/* Rows. */
		int ups = 0, downs = 0, flats = 0;
		for (const PriceRow &row : rows) {
			if (!row.has_change || (row.change_x10 < 1 && row.change_x10 > -1)) flats++;
			else if (row.change_x10 > 0) ups++;
			else downs++;
		}
		for (size_t k = 0; k < rows.size(); k++) {
			const PriceRow &row = rows[k];
			const int y = g.top + static_cast<int>(k) * row_h;
			if (y + row_h > g.bottom) break;
			if (row.index == sel) GfxFillRect(g.left + 1, y, g.right - 1, y + row_h - 1, GREY_SCALE(4));
			else if (k % 2 == 1) GfxFillRect(g.left + 1, y, g.right - 1, y + row_h - 1, GREY_SCALE(1));
			const int ty = y + 1;
			DrawString(g.left + 6, name_r - 2, ty, GetString(row.cs->name), TC_WHITE, SA_LEFT);
			DrawString(name_r, base_r - 6, ty, GetString(STR_JUST_CURRENCY_LONG, row.base), TC_GREY, SA_RIGHT);
			DrawString(base_r, cur_r - 6, ty, GetString(STR_JUST_CURRENCY_LONG, row.current), row.gold ? TC_WHITE : PriceColour(row.shown_ppm), SA_RIGHT);
			if (row.gold) {
				DrawString(cur_r, chg_r - 6, ty, "ตลาดทอง", TC_YELLOW, SA_RIGHT);
				continue;
			}
			DrawString(cur_r, idx_r - 6, ty, IndexText(row.shown_ppm), PriceColour(row.shown_ppm), SA_RIGHT);
			if (row.has_change) {
				const std::string text = ChangeText(row.change_x10);
				const TextColour col = ChangeColour(true, row.change_x10);
				DrawString(idx_r, chg_r - 6, ty, text, col, SA_RIGHT);
				if (row.change_x10 >= 1 || row.change_x10 <= -1) {
					const int tw = GetStringBoundingBox(text).width;
					DrawTriangle(chg_r - 6 - tw - 7, y + row_h / 2, 3, row.change_x10 > 0, row.change_x10 > 0);
				}
				this->DrawSparkline(spark_l, y + 2, spark_r, y + row_h - 3, row);
			} else {
				DrawString(idx_r, chg_r - 6, ty, "—", TC_GREY, SA_RIGHT);
			}
		}

		/* Footer of the table: how many cargo rise and fall. */
		const int fy = g.bottom + 1;
		GfxFillRect(g.left, fy, g.right, fy + row_h + ScaleGUITrad(4), GREY_SCALE(2));
		DrawTriangle(g.left + 12, fy + row_h / 2 + 2, 3, true, true);
		DrawString(g.left + 22, g.right, fy + 3, fmt::format("ขึ้น {}", ups), TC_GREEN, SA_LEFT);
		DrawTriangle(g.left + 82, fy + row_h / 2 + 2, 3, false, false);
		DrawString(g.left + 92, g.right, fy + 3, fmt::format("ลง {}", downs), TC_RED, SA_LEFT);
		DrawString(g.left + 150, g.right, fy + 3, fmt::format("— คงที่ {}", flats), TC_GREY, SA_LEFT);

		/* The two top-5 boxes on the right. */
		std::vector<const PriceRow *> rising, falling;
		for (const PriceRow &row : all) {
			if (!row.has_change) continue;
			if (row.change_x10 >= 1) rising.push_back(&row);
			else if (row.change_x10 <= -1) falling.push_back(&row);
		}
		std::sort(rising.begin(), rising.end(), [](const PriceRow *a, const PriceRow *b) {
			return a->change_x10 != b->change_x10 ? a->change_x10 > b->change_x10 : a->index < b->index;
		});
		std::sort(falling.begin(), falling.end(), [](const PriceRow *a, const PriceRow *b) {
			return a->change_x10 != b->change_x10 ? a->change_x10 < b->change_x10 : a->index < b->index;
		});
		const int rx0 = g.right + pad;
		const int rx1 = r.right - pad;
		if (rx1 - rx0 < 120) return;
		const int total = r.bottom - pad - box_top;
		const int box_h = (total - pad) / 2;
		this->DrawTopBox(rx0, rx1, box_top, box_top + box_h, rising, true);
		this->DrawTopBox(rx0, rx1, box_top + box_h + pad, box_top + 2 * box_h + pad, falling, false);
		(void)band_h;
		(void)hdr_h;
	}

	void DrawTopBox(int left, int right, int top, int bottom, const std::vector<const PriceRow *> &list, bool up) const
	{
		const int row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
		const int w = right - left;
		const auto band = GetColourGradient(up ? COLOUR_GREEN : COLOUR_RED, SHADE_DARK);
		GfxFillRect(left, top, right, bottom, GREY_SCALE(1));
		GfxFillRect(left, top, right, top + row_h + ScaleGUITrad(5), band);
		DrawTriangle(left + 10, top + row_h / 2 + 3, 3, up, up);
		DrawString(left + 20, right - 6, top + 3, up ? "อันดับสินค้าราคาขึ้น" : "อันดับสินค้าราคาลง", TC_WHITE, SA_LEFT);
		DrawString(left + 6, right - 6, top + 3, "TOP 5", TC_WHITE, SA_RIGHT);

		const int rank_x = left + 8;
		const int name_x = left + w * 14 / 100;
		const int price_r = left + w * 57 / 100;
		const int diff_r = left + w * 78 / 100;
		const int pct_r = right - 6;
		int y = top + row_h + ScaleGUITrad(7);
		GfxFillRect(left, y - 1, right, y + row_h - 1, GREY_SCALE(2));
		DrawString(rank_x, name_x, y, "อันดับ", TC_GREY, SA_LEFT);
		DrawString(name_x, price_r, y, "สินค้า", TC_GREY, SA_LEFT);
		DrawString(name_x, price_r, y, "ราคา", TC_GREY, SA_RIGHT);
		DrawString(price_r, diff_r, y, "ส่วนต่าง", TC_GREY, SA_RIGHT);
		DrawString(diff_r, pct_r, y, fmt::format("% / {}", PeriodUnit(this->period)), TC_GREY, SA_RIGHT);
		y += row_h + 2;

		if (list.empty()) {
			DrawString(left + 8, right - 8, y + 2, _cargo_market.HistoryAvailable() < 2 ? "ยังไม่มีประวัติราคา รอให้เกมผ่านไปอย่างน้อย 2 วัน" : (up ? "ไม่มีสินค้าที่ราคาขึ้น" : "ไม่มีสินค้าที่ราคาลง"), TC_GREY, SA_LEFT);
		}
		const int foot_h = row_h + ScaleGUITrad(4);
		for (size_t k = 0; k < list.size() && k < 5; k++) {
			if (y + row_h > bottom - foot_h) break;
			const PriceRow &row = *list[k];
			if (k == 0) GfxFillRect(left + 1, y, right - 1, y + row_h - 1, GREY_SCALE(4));
			DrawString(rank_x + 2, name_x, y + 1, fmt::format("{}", k + 1), TC_WHITE, SA_LEFT);
			DrawString(name_x, price_r - 4, y + 1, GetString(row.cs->name), TC_WHITE, SA_LEFT);
			DrawString(name_x, price_r - 4, y + 1, GetString(STR_JUST_CURRENCY_LONG, row.current), TC_GREY, SA_RIGHT);
			const Money d = row.diff < 0 ? -row.diff : row.diff;
			DrawString(price_r, diff_r - 4, y + 1, fmt::format("{}{}", row.diff < 0 ? "-" : "+", GetString(STR_JUST_CURRENCY_LONG, d)), up ? TC_GREEN : TC_RED, SA_RIGHT);
			DrawString(diff_r, pct_r, y + 1, ChangeText(row.change_x10), up ? TC_GREEN : TC_RED, SA_RIGHT);
			y += row_h;
		}
		DrawString(left + 8, right - 8, bottom - foot_h + 3, up ? "เรียง % เพิ่มขึ้นมากที่สุด → น้อยที่สุด" : "เรียง % ลดลงมากที่สุด → น้อยที่สุด", TC_GREY, SA_LEFT);
	}

	/** The list of cargo on the left of the graph and the history tabs. */
	void DrawCargoList(const Rect &r, const std::vector<PriceRow> &rows, size_t sel) const
	{
		const int pad = ScaleGUITrad(6);
		const ListGeom g = this->ListGeometry(r);
		const int row_h = g.row_h;
		const int band_h = row_h + ScaleGUITrad(6);
		const int box_top = r.top + pad;
		GfxFillRect(g.left, box_top, g.right, box_top + band_h - 1, GREY_SCALE(3));
		DrawString(g.left + 6, g.right - 6, box_top + 3, "เลือกสินค้า", TC_WHITE, SA_LEFT);
		GfxFillRect(g.left, box_top + band_h, g.right, g.top - 1, GREY_SCALE(2));
		DrawString(g.left + 6, g.right - 6, box_top + band_h + 1, "สินค้า", TC_GREY, SA_LEFT);
		DrawString(g.left + 6, g.right - 6, box_top + band_h + 1, "ดัชนีราคา", TC_GREY, SA_RIGHT);
		for (size_t k = 0; k < rows.size(); k++) {
			const PriceRow &row = rows[k];
			const int y = g.top + static_cast<int>(k) * row_h;
			if (y + row_h > g.bottom) break;
			if (row.index == sel) GfxFillRect(g.left + 1, y, g.right - 1, y + row_h - 1, GREY_SCALE(4));
			else if (k % 2 == 1) GfxFillRect(g.left + 1, y, g.right - 1, y + row_h - 1, GREY_SCALE(1));
			DrawString(g.left + 6, g.right - 70, y + 1, GetString(row.cs->name), TC_WHITE, SA_LEFT);
			if (row.gold) DrawString(g.left + 6, g.right - 6, y + 1, "ตลาดทอง", TC_YELLOW, SA_RIGHT);
			else DrawString(g.left + 6, g.right - 6, y + 1, IndexText(row.shown_ppm), PriceColour(row.shown_ppm), SA_RIGHT);
		}
	}

	void DrawGraph(const Rect &r) const
	{
		const int pad = ScaleGUITrad(6);
		const std::vector<PriceRow> rows = this->BuildRows(false);
		if (rows.empty()) return;
		const size_t sel = this->SelectedIndex(rows);
		this->DrawCargoList(r, rows, sel);

		const ListGeom g = this->ListGeometry(r);
		const int row_h = g.row_h;
		const int left = g.right + pad;
		const int right = r.right - pad;
		const int top = r.top + pad;
		const int bottom = r.bottom - pad;
		if (right - left < 160 || bottom - top < 120) return;

		const PriceRow *cur = nullptr;
		for (const PriceRow &row : rows) {
			if (row.index == sel) cur = &row;
		}
		if (cur == nullptr) return;

		GfxFillRect(left, top, right, top + row_h + ScaleGUITrad(5), GREY_SCALE(3));
		DrawString(left + 8, right - 8, top + 3, fmt::format("{} — ดัชนีราคา ({})", GetString(cur->cs->name), PeriodLabel(this->period)), TC_WHITE, SA_LEFT);
		GfxFillRect(left, top + row_h + ScaleGUITrad(6), right, bottom, GREY_SCALE(1));

		const uint32_t avail = _cargo_market.HistoryAvailable();
		if (cur->gold) {
			DrawString(left + 12, right - 12, top + row_h * 3, "สินค้านี้ใช้ราคาตลาดทอง ดูกราฟได้ในหน้าต่างทอง", TC_YELLOW, SA_LEFT);
			return;
		}
		if (avail < 2) {
			DrawString(left + 12, right - 12, top + row_h * 3, "ยังไม่มีประวัติราคา รอให้เกมผ่านไปอย่างน้อย 2 วัน", TC_GREY, SA_LEFT);
			return;
		}

		const uint32_t back = std::min<uint32_t>(PERIOD_DAYS[this->period], avail - 1);
		std::vector<int64_t> v;
		v.reserve(back + 1);
		for (uint32_t k = 0; k <= back; k++) v.push_back(_cargo_market.HistoryPpm(cur->index, back - k));
		int64_t mn = *std::min_element(v.begin(), v.end());
		int64_t mx = *std::max_element(v.begin(), v.end());
		const int64_t spread = std::max<int64_t>(mx - mn, cargomarket::PPM / 50);
		const int64_t lo = mn - spread / 8;
		const int64_t hi = mx + spread / 8;

		/* Numbers above the chart: now, highest, lowest. */
		const int sy = top + row_h + ScaleGUITrad(10);
		DrawString(left + 12, right - 12, sy, fmt::format("ตอนนี้ {}   สูงสุด {}   ต่ำสุด {}", PercentText(v.back()), PercentText(mx), PercentText(mn)), TC_WHITE, SA_LEFT);

		const int cl = left + ScaleGUITrad(70);
		const int cr = right - ScaleGUITrad(16);
		const int ct = sy + row_h + ScaleGUITrad(8);
		const int cb = bottom - ScaleGUITrad(26);
		if (cr - cl < 40 || cb - ct < 40) return;

		/* Grid and the labels of the vertical axis. */
		for (int i = 0; i <= 4; i++) {
			const int y = cb - (cb - ct) * i / 4;
			const int64_t value = lo + (hi - lo) * i / 4;
			GfxDrawLine(cl, y, cr, y, GREY_SCALE(3));
			DrawString(left + 4, cl - 6, y - GetCharacterHeight(FS_NORMAL) / 2, PercentText(value), TC_GREY, SA_RIGHT);
		}
		if (cargomarket::PPM > lo && cargomarket::PPM < hi) {
			const int y = cb - static_cast<int>(static_cast<int64_t>(cb - ct) * (cargomarket::PPM - lo) / (hi - lo));
			GfxDrawLine(cl, y, cr, y, GetColourGradient(COLOUR_YELLOW, SHADE_NORMAL));
		}
		/* The line. */
		const auto colour = GetColourGradient(v.back() >= v.front() ? COLOUR_GREEN : COLOUR_RED, SHADE_LIGHT);
		int px = 0, py = 0;
		for (size_t k = 0; k < v.size(); k++) {
			const int x = cl + static_cast<int>(static_cast<int64_t>(cr - cl) * static_cast<int64_t>(k) / static_cast<int64_t>(v.size() - 1));
			const int y = cb - static_cast<int>(static_cast<int64_t>(cb - ct) * (v[k] - lo) / (hi - lo));
			if (k > 0) GfxDrawLine(px, py, x, y, colour, 2);
			px = x;
			py = y;
		}
		DrawString(cl, cr, cb + ScaleGUITrad(6), fmt::format("{} วันก่อน", back), TC_GREY, SA_LEFT);
		DrawString(cl, cr, cb + ScaleGUITrad(6), "ตอนนี้", TC_GREY, SA_RIGHT);
		DrawString(cl, cr, cb + ScaleGUITrad(6), "เส้นเหลือง = ราคาปกติ (100%)", TC_YELLOW, SA_HOR_CENTER);
	}

	void DrawHistory(const Rect &r) const
	{
		const int pad = ScaleGUITrad(6);
		const std::vector<PriceRow> rows = this->BuildRows(false);
		if (rows.empty()) return;
		const size_t sel = this->SelectedIndex(rows);
		this->DrawCargoList(r, rows, sel);

		const ListGeom g = this->ListGeometry(r);
		const int row_h = g.row_h;
		const int left = g.right + pad;
		const int right = r.right - pad;
		const int top = r.top + pad;
		const int bottom = r.bottom - pad;
		if (right - left < 200 || bottom - top < 80) return;

		const PriceRow *cur = nullptr;
		for (const PriceRow &row : rows) {
			if (row.index == sel) cur = &row;
		}
		if (cur == nullptr) return;

		GfxFillRect(left, top, right, top + row_h + ScaleGUITrad(5), GREY_SCALE(3));
		DrawString(left + 8, right - 8, top + 3, fmt::format("{} — ประวัติราคา ({})", GetString(cur->cs->name), PeriodLabel(this->period)), TC_WHITE, SA_LEFT);
		const int hy = top + row_h + ScaleGUITrad(6);
		GfxFillRect(left, hy, right, hy + row_h + ScaleGUITrad(2), GREY_SCALE(2));
		const int w = right - left;
		const int c1 = left + w * 28 / 100;
		const int c2 = left + w * 52 / 100;
		const int c3 = left + w * 78 / 100;
		DrawString(left + 8, c1, hy + 1, "เมื่อ", TC_GREY, SA_LEFT);
		DrawString(c1, c2 - 6, hy + 1, "ดัชนีราคา", TC_GREY, SA_RIGHT);
		DrawString(c2, c3 - 6, hy + 1, "ราคาขนส่ง", TC_GREY, SA_RIGHT);
		DrawString(c3, right - 8, hy + 1, "เทียบวันก่อนหน้า", TC_GREY, SA_RIGHT);

		const uint32_t avail = _cargo_market.HistoryAvailable();
		int y = hy + row_h + ScaleGUITrad(4);
		if (cur->gold) {
			DrawString(left + 12, right - 12, y + 4, "สินค้านี้ใช้ราคาตลาดทอง ไม่มีประวัติราคาในหน้านี้", TC_YELLOW, SA_LEFT);
			return;
		}
		if (avail < 1) {
			DrawString(left + 12, right - 12, y + 4, "ยังไม่มีประวัติราคา รอให้เกมผ่านไปอย่างน้อย 1 วัน", TC_GREY, SA_LEFT);
			return;
		}
		const uint32_t span = std::min<uint32_t>(PERIOD_DAYS[this->period], avail - 1);
		const uint32_t step = std::max<uint32_t>(1, PERIOD_DAYS[this->period] / 30);
		for (uint32_t d = 0; d <= span; d += step) {
			if (y + row_h > bottom) break;
			const int64_t ppm = _cargo_market.HistoryPpm(cur->index, d);
			const int64_t before = _cargo_market.HistoryPpm(cur->index, d + step);
			if (((d / step) & 1) == 1) GfxFillRect(left + 1, y, right - 1, y + row_h - 1, GREY_SCALE(1));
			DrawString(left + 8, c1, y + 1, d == 0 ? std::string("ตอนนี้") : fmt::format("{} วันก่อน", d), d == 0 ? TC_WHITE : TC_GREY, SA_LEFT);
			DrawString(c1, c2 - 6, y + 1, IndexText(ppm), PriceColour(ppm), SA_RIGHT);
			DrawString(c2, c3 - 6, y + 1, GetString(STR_JUST_CURRENCY_LONG, cur->base * ppm / cargomarket::PPM), TC_GREY, SA_RIGHT);
			if (before > 0 && d + step < avail) {
				const int64_t x10 = (ppm - before) * 1000 / before;
				DrawString(c3, right - 8, y + 1, ChangeText(x10), ChangeColour(true, x10), SA_RIGHT);
			} else {
				DrawString(c3, right - 8, y + 1, "—", TC_GREY, SA_RIGHT);
			}
			y += row_h;
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_cargo_prices_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CP_CAPTION), SetStringTip(STR_CARGO_PRICES_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CP_HEADER), SetMinimalSize(860, 50), SetFill(1, 0), SetResize(1, 0),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CP_TAB_OVERVIEW), SetMinimalSize(110, 16), SetFill(0, 0), SetStringTip(STR_CARGO_PRICES_TAB_OVERVIEW, STR_NULL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CP_TAB_GRAPH), SetMinimalSize(110, 16), SetFill(0, 0), SetStringTip(STR_CARGO_PRICES_TAB_GRAPH, STR_NULL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CP_TAB_HISTORY), SetMinimalSize(110, 16), SetFill(0, 0), SetStringTip(STR_CARGO_PRICES_TAB_HISTORY, STR_NULL),
		NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 1), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_CP_FILTER), SetMinimalSize(210, 16), SetFill(0, 0), SetStringTip(STR_CARGO_PRICES_DROPDOWN, STR_CARGO_PRICES_TIP_FILTER),
		NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_CP_PERIOD), SetMinimalSize(150, 16), SetFill(0, 0), SetStringTip(STR_CARGO_PRICES_DROPDOWN, STR_CARGO_PRICES_TIP_PERIOD),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CP_PANEL), SetMinimalSize(860, 440), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_CP_FOOT), SetMinimalSize(640, 34), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CP_CLOSE), SetMinimalSize(120, 34), SetFill(0, 1), SetStringTip(STR_CARGO_PRICES_CLOSE, STR_CARGO_PRICES_TIP_CLOSE),
	EndContainer(),
};

static WindowDesc _cargo_prices_desc(
	WDP_AUTO, "cargo_prices", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_cargo_prices_widgets
);

void ShowCargoPricesWindow()
{
	if (BringWindowToFrontById(WC_CENTRAL_BANK, CP_WINDOW_NUMBER) != nullptr) return;
	new CargoPricesWindow(_cargo_prices_desc);
}

bool IsCargoPricesWindowOpen()
{
	return FindWindowById(WC_CENTRAL_BANK, CP_WINDOW_NUMBER) != nullptr;
}
