/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargo_market_gui.cpp The cargo prices window (it takes the place of the cargo payment rates graph of OpenTTD). See cargo_market_gui.h, cargo_market.h and CargoMarketMath.h. */

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

static constexpr uint CARGO_REF_UNITS = 10; ///< The pay column shows this many units ...
static constexpr uint CARGO_REF_TILES = 20; ///< ... carried this many tiles, with no time lost on the way.

static constexpr size_t TOWNS_BEST = 5;   ///< Towns listed as paying most.
static constexpr size_t TOWNS_WORST = 3;  ///< Towns listed as paying least.

/** A percentage of a ppm value with two decimals, for example 103.45%. */
static std::string PercentText(int64_t ppm)
{
	const int64_t v = ppm < 0 ? -ppm : ppm;
	return fmt::format("{}{}.{:02}%", ppm < 0 ? "-" : "", v / 10000, v % 10000 / 100);
}

/** A change against 100%, with its sign, for example +3.45% or -1.20%. */
static std::string ChangeText(int64_t ppm)
{
	const int64_t d = ppm - cargomarket::PPM;
	const int64_t v = d < 0 ? -d : d;
	return fmt::format("{}{}.{:02}%", d < 0 ? "-" : "+", v / 10000, v % 10000 / 100);
}

/** Units per day with two decimals (the state keeps them times 100). */
static std::string UnitsText(int64_t x100)
{
	return fmt::format("{}.{:02}", x100 / 100, x100 % 100);
}

/** More pay is good for the player: green above the normal pay, red below it. */
static TextColour PriceColour(int64_t ppm)
{
	if (ppm > cargomarket::PPM + 5000) return TC_GREEN;
	if (ppm < cargomarket::PPM - 5000) return TC_RED;
	return TC_WHITE;
}

/** One line of the list of towns. */
struct TownPay {
	TownID town;
	int64_t factor_ppm;
};

/** The cargo prices window: a table with one row per cargo and the towns that pay most and least for passengers. */
struct CargoPricesWindow : Window {
	/** Redraw regularly: the prices move every game day, the towns change by themselves. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		this->SetDirty();
	}};

	CargoPricesWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(CP_WINDOW_NUMBER);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_CP_PANEL) return;

		const int pad = ScaleGUITrad(8);
		const int row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
		const int left = r.left + pad;
		const int right = r.right - pad;
		if (right <= left) return;
		const int width = right - left;
		int y = r.top + pad;

		/* Columns: name | price now | news push | market state | what 10 units over 20 tiles pay | normal / recent deliveries per day. */
		const int col_price = left + width * 21 / 100;
		const int col_news = left + width * 32 / 100;
		const int col_state = left + width * 43 / 100;
		const int col_pays = left + width * 68 / 100;
		const int col_ref = left + width * 80 / 100;

		DrawString(left, right, y, "ราคาสินค้า (CARGO PRICES)", TC_LIGHT_BLUE, SA_LEFT);
		DrawString(left, right, y, "100% = ค่าขนส่งปกติของ OpenTTD", TC_GREY, SA_RIGHT);
		y += row_h + ScaleGUITrad(4);

		DrawString(left, col_price - 4, y, "สินค้า", TC_GREY, SA_LEFT);
		DrawString(col_price, col_news - 4, y, "ราคาตอนนี้", TC_GREY, SA_RIGHT);
		DrawString(col_news, col_state - 4, y, "ข่าว", TC_GREY, SA_RIGHT);
		DrawString(col_state + 8, col_pays - 4, y, "ตลาด", TC_GREY, SA_LEFT);
		DrawString(col_pays, col_ref - 4, y, "10 หน่วย 20 ช่อง", TC_GREY, SA_RIGHT);
		DrawString(col_ref, right, y, "ปกติ / ล่าสุด ต่อวัน", TC_GREY, SA_RIGHT);
		y += row_h;
		GfxFillRect(left, y - 2, right, y - 1, GetColourGradient(COLOUR_GREY, SHADE_LIGHT));

		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			if (y + row_h > r.bottom - pad) break;
			const CargoType ct = cs->Index();
			DrawString(left, col_price - 4, y, GetString(cs->name), TC_WHITE, SA_LEFT);
			if (CargoMarketIsGoldCargo(ct)) {
				DrawString(col_price, col_pays - 4, y, "ราคาตามตลาดทอง (ดูหน้าต่างทอง)", TC_YELLOW, SA_LEFT);
				DrawString(col_pays, col_ref - 4, y, GetString(STR_JUST_CURRENCY_LONG, GetTransportedGoodsIncome(CARGO_REF_UNITS, CARGO_REF_TILES, 0, ct)), TC_GREY, SA_RIGHT);
				y += row_h;
				continue;
			}
			const cargomarket::CargoState &c = _cargo_market.cargo[static_cast<size_t>(ct)];
			DrawString(col_price, col_news - 4, y, PercentText(c.price_ppm), PriceColour(c.price_ppm), SA_RIGHT);

			const int64_t push = CargoMarketNewsPushPpm(ct);
			if (push != 0) DrawString(col_news, col_state - 4, y, fmt::format("{}{}", push < 0 ? "-" : "+", PercentText(push < 0 ? -push : push)), push > 0 ? TC_GREEN : TC_RED, SA_RIGHT);

			if (c.days <= cargomarket::BASELINE_DAYS) {
				DrawString(col_state + 8, col_pays - 4, y, fmt::format("กำลังวัด {}/{} วัน", c.days, cargomarket::BASELINE_DAYS), TC_GREY, SA_LEFT);
			} else {
				switch (cargomarket::SupplyState(c.fast_x100, c.ref_x100, c.days)) {
					case 1: DrawString(col_state + 8, col_pays - 4, y, "ล้นตลาด (ราคาลด)", TC_RED, SA_LEFT); break;
					case -1: DrawString(col_state + 8, col_pays - 4, y, "ขาดตลาด (ราคาขึ้น)", TC_GREEN, SA_LEFT); break;
					default: DrawString(col_state + 8, col_pays - 4, y, "ปกติ", TC_GREY, SA_LEFT); break;
				}
			}
			/* What the delivery pays now: the base pay of OpenTTD for 10 units over 20 tiles without delay, times the price of the cargo
			 * (GetTransportedGoodsIncome has the price in it). It is what the old payment rates graph was for. */
			DrawString(col_pays, col_ref - 4, y, GetString(STR_JUST_CURRENCY_LONG, GetTransportedGoodsIncome(CARGO_REF_UNITS, CARGO_REF_TILES, 0, ct)), PriceColour(c.price_ppm), SA_RIGHT);
			DrawString(col_ref, right, y, fmt::format("{} / {}", UnitsText(c.ref_x100), UnitsText(c.fast_x100)), TC_GREY, SA_RIGHT);
			y += row_h;
		}

		/* The towns that pay most and least for passengers (the cargo that follows the town fully). */
		y += ScaleGUITrad(6);
		const CargoType pass = GetCargoTypeByLabel(CargoLabel{'PASS'});
		if (!IsValidCargoType(pass) || y + row_h * 3 > r.bottom - pad) return;
		GfxFillRect(left, y - 3, right, y - 2, GetColourGradient(COLOUR_GREY, SHADE_LIGHT));
		DrawString(left, right, y, "เมืองที่จ่ายค่าผู้โดยสารมากที่สุด / น้อยที่สุด (ตัวคูณของเมือง คูณกับราคาด้านบน)", TC_LIGHT_BLUE, SA_LEFT);
		y += row_h;

		std::vector<TownPay> towns;
		for (const Town *t : Town::Iterate()) towns.push_back({t->index, CargoMarketLocalFactorPpm(pass, t->index)});
		if (towns.empty()) {
			DrawString(left, right, y, "ยังไม่มีเมือง", TC_GREY, SA_LEFT);
			return;
		}
		/* Sort by factor, the town id breaks a tie: the order is the same every time. */
		std::sort(towns.begin(), towns.end(), [](const TownPay &a, const TownPay &b) {
			return a.factor_ppm != b.factor_ppm ? a.factor_ppm > b.factor_ppm : a.town.base() < b.town.base();
		});
		const int half = left + width / 2;
		const size_t best = std::min(TOWNS_BEST, towns.size());
		const size_t worst = std::min(TOWNS_WORST, towns.size() - best);
		int yb = y;
		for (size_t i = 0; i < best && yb + row_h <= r.bottom - pad; i++, yb += row_h) {
			DrawString(left, half - 8, yb, GetString(STR_TOWN_NAME, towns[i].town), TC_WHITE, SA_LEFT);
			DrawString(left, half - 8, yb, ChangeText(towns[i].factor_ppm), PriceColour(towns[i].factor_ppm), SA_RIGHT);
		}
		int yw = y;
		for (size_t i = 0; i < worst && yw + row_h <= r.bottom - pad; i++, yw += row_h) {
			const TownPay &tp = towns[towns.size() - 1 - i];
			DrawString(half + 8, right, yw, GetString(STR_TOWN_NAME, tp.town), TC_WHITE, SA_LEFT);
			DrawString(half + 8, right, yw, ChangeText(tp.factor_ppm), PriceColour(tp.factor_ppm), SA_RIGHT);
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
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CP_PANEL), SetMinimalSize(760, 360), SetFill(1, 1), SetResize(1, 1),
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
