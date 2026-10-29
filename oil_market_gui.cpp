/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file oil_market_gui.cpp The oil market window (XTI): price, stock, what fuel costs the vehicles and the latest oil news. See oil_market_math.h and valuables.h. */

#include "stdafx.h"
#include "oil_market_gui.h"
#include "oil_market_math.h"
#include "CargoMarketMath.h"
#include "market_events.h"
#include "market_news_ai.h"
#include "stock_base.h"
#include "valuables.h"
#include "game_time/game_time_manager.h"
#include <algorithm>
#include <chrono>
#include <string>
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

/** Window number inside the class WC_CENTRAL_BANK (0 bank, 1 statement, 2 past statements, 3 buy gold, 4 policy, 5 payroll news, 6 market news, 7 cargo prices, 8 oil market). */
static constexpr int OM_WINDOW_NUMBER = 8;

static constexpr size_t OIL_NEWS_LINES = 5; ///< The latest oil stories that are listed.

/** A ppm value as a percentage with two decimals, for example 103.45%. */
static std::string OilPercentText(int64_t ppm)
{
	const int64_t v = ppm < 0 ? -ppm : ppm;
	return fmt::format("{}{}.{:02}%", ppm < 0 ? "-" : "", v / 10000, v % 10000 / 100);
}

/** A number with a space between the thousands. */
static std::string OilGrouped(int64_t value)
{
	std::string digits = fmt::format("{}", value < 0 ? -value : value);
	std::string out;
	for (size_t i = 0; i < digits.size(); i++) {
		if (i != 0 && (digits.size() - i) % 3 == 0) out += ' ';
		out += digits[i];
	}
	return value < 0 ? "-" + out : out;
}

/** The oil market window. */
struct OilMarketWindow : Window {
	/** Redraw regularly: the price and the stock move every game hour. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		this->SetDirty();
	}};

	OilMarketWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(OM_WINDOW_NUMBER);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_OM_PANEL) return;

		const int pad = ScaleGUITrad(8);
		const int row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
		const int left = r.left + pad;
		const int right = r.right - pad;
		if (right <= left) return;
		const int bottom = r.bottom - pad;
		int y = r.top + pad;

		/* Draws one line (label on the left, value on the right) when it fits. */
		auto line = [&](const std::string &label, const std::string &value, TextColour colour) {
			if (y + row_h > bottom) return;
			DrawString(left, right, y, label, TC_GREY, SA_LEFT);
			DrawString(left, right, y, value, colour, SA_RIGHT);
			y += row_h;
		};

		DrawString(left, right, y, "ตลาดน้ำมัน (OIL MARKET, XTI)", TC_LIGHT_BLUE, SA_LEFT);
		DrawString(left, right, y, "1 หน่วย = 1 บาร์เรล เทรดได้ในเทอร์มินัลเหมือนทอง", TC_GREY, SA_RIGHT);
		y += row_h + ScaleGUITrad(4);

		const int64_t price = GetStockPrice(CompanyID{static_cast<uint8_t>(STOCK_OIL_SYMBOL)});
		const int64_t stock = _valuables.oil_stock_barrels;
		const int64_t permille = oilmath::StockPermille(stock);
		const int64_t index_ppm = OilPriceIndexPpm();
		const int64_t news_ppm = _market_events.FuelIndexPpm(static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes()));
		const int64_t fuel_ppm = FuelIndexWithOilPpm(news_ppm);
		const int64_t excess_ppm = cargomarket::FuelExcessPpm(fuel_ppm);

		if (price > 0) {
			line("ราคาน้ำมันตอนนี้ (ต่อบาร์เรล)", GetString(STR_JUST_CURRENCY_LONG, price), TC_WHITE);
		} else {
			line("ราคาน้ำมันตอนนี้ (ต่อบาร์เรล)", "ตลาดยังไม่เริ่ม", TC_GREY);
		}
		line("ราคาเทียบกับตอนเริ่มเกม", OilPercentText(index_ppm), index_ppm > cargomarket::PPM + 5000 ? TC_RED : (index_ppm < cargomarket::PPM - 5000 ? TC_GREEN : TC_WHITE));
		y += ScaleGUITrad(4);

		line("สต็อกน้ำมันในตลาด", fmt::format("{} บาร์เรล", OilGrouped(stock)), TC_WHITE);
		line("สต็อกเทียบกับตอนเริ่มเกม", OilPercentText(permille * 1000), permille < oilmath::SHORT_PERMILLE_1 ? TC_RED : (permille > oilmath::GLUT_PERMILLE_1 ? TC_YELLOW : TC_WHITE));
		line("ตัวคูณราคาจากความขาดแคลน", OilPercentText(_valuables.OilScarcityPpm()), TC_WHITE);
		line("บ่อน้ำมันเติมได้อีกวันนี้", fmt::format("{} บาร์เรล", OilGrouped(_valuables.oil_well_allowance)), TC_GREY);
		y += ScaleGUITrad(4);

		line("ดัชนีเชื้อเพลิงของยานพาหนะ", OilPercentText(fuel_ppm), excess_ppm > 0 ? TC_RED : TC_WHITE);
		line("ค่าเชื้อเพลิงเพิ่มจากราคาปกติ (จ่ายเฉพาะส่วนที่เกิน)", OilPercentText(excess_ppm), excess_ppm > 0 ? TC_RED : TC_GREEN);
		line("ยานพาหนะรับผลจากราคาน้ำมัน", fmt::format("{}%", ValuablesManager::OIL_FUEL_PASS_PERCENT), TC_GREY);
		y += ScaleGUITrad(6);

		/* The latest oil stories, newest first. */
		if (y + row_h * 2 > bottom) return;
		GfxFillRect(left, y - 3, right, y - 2, GetColourGradient(COLOUR_GREY, SHADE_LIGHT));
		DrawString(left, right, y, "ข่าวน้ำมันล่าสุด (จากสต็อกจริง)", TC_LIGHT_BLUE, SA_LEFT);
		y += row_h;
		const int64_t now = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());
		size_t shown = 0;
		for (const MarketEvent &e : _market_events.log) {
			if (!MarketKindIsRealOilKind(e.kind)) continue;
			if (shown >= OIL_NEWS_LINES || y + row_h > bottom) break;
			const MarketStory story = ComposeMarketStory(e, std::string());
			const int64_t days = std::max<int64_t>(0, (now - e.minute) / GameTimeManager::MINUTES_PER_DAY);
			DrawString(left, right, y, fmt::format("{} วันก่อน   {}", days, story.headline), e.kind == MarketEventKind::OilSupplyShort ? TC_RED : TC_GREEN, SA_LEFT, false, FS_NORMAL);
			y += row_h;
			shown++;
		}
		if (shown == 0) DrawString(left, right, y, "ยังไม่มีข่าว: สต็อกอยู่ในระดับปกติ", TC_GREY, SA_LEFT);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_oil_market_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_OM_CAPTION), SetStringTip(STR_OIL_MARKET_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_OM_PANEL), SetMinimalSize(620, 330), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
};

static WindowDesc _oil_market_desc(
	WDP_AUTO, "oil_market", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_oil_market_widgets
);

void ShowOilMarketWindow()
{
	if (BringWindowToFrontById(WC_CENTRAL_BANK, OM_WINDOW_NUMBER) != nullptr) return;
	new OilMarketWindow(_oil_market_desc);
}

bool IsOilMarketWindowOpen()
{
	return FindWindowById(WC_CENTRAL_BANK, OM_WINDOW_NUMBER) != nullptr;
}
