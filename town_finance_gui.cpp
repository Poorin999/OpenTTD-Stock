/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_finance_gui.cpp The finance window of one town. See town_finance_gui.h. It only reads the account (_town_treasury), it changes nothing in the game. */

#include "stdafx.h"
#include "town_finance_gui.h"
#include "town_treasury.h"
#include "central_bank.h"
#include "gold_reserve.h"
#include "stock_base.h"
#include <chrono>
#include <string>
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "core/format.hpp"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "town.h"
#include "window_gui.h"
#include "zoom_func.h"

#include "table/strings.h"

#include "safeguards.h"

/** True: the window shows Thai text. False: English only. Change it here and build again. */
static constexpr bool USE_THAI = true;

/** The text in Thai or in English. */
static std::string TfText(const char *thai, const char *english)
{
	return USE_THAI ? std::string(thai) : std::string(english);
}

/** Widgets of the town finance window (here, not in src/widgets: the script API generator reads that folder). */
enum TownFinanceWidgets : WidgetID {
	WID_TFN_CAPTION,
	WID_TFN_PANEL,
};

/** The window number of a town finance window inside the class WC_CENTRAL_BANK (0-6 are the bank windows, see market_news_gui.cpp): this + the index of the town. */
static constexpr int TF_WINDOW_BASE = 100000;

static std::string TfMoney(int64_t v)
{
	return GetString(STR_JUST_CURRENCY_LONG, v);
}

/** Ounces with two decimals from hundredths of an ounce. */
static std::string TfOunces(int64_t cz)
{
	const int64_t a = cz < 0 ? -cz : cz;
	return fmt::format("{}{}.{:02}", cz < 0 ? "-" : "", a / citizengold::CZ_PER_OZ, a % citizengold::CZ_PER_OZ);
}

struct TownFinanceWindow : Window {
	uint32_t town_index; ///< The town of this window.

	/** The numbers change every game hour: redraw regularly. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		/* The town can be removed while the window is open. */
		if (!Town::IsValidID(static_cast<TownID>(this->town_index))) {
			this->Close();
			return;
		}
		this->SetDirty();
	}};

	TownFinanceWindow(WindowDesc &desc, uint32_t town) : Window(desc), town_index(town)
	{
		this->InitNested(TF_WINDOW_BASE + static_cast<int>(town));
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_TFN_CAPTION) return GetString(STR_TOWN_FINANCE_CAPTION, static_cast<TownID>(this->town_index));
		return this->Window::GetWidgetString(widget, stringid);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_TFN_PANEL) return;

		const Town *t = Town::GetIfValid(static_cast<TownID>(this->town_index));
		if (t == nullptr) return;

		const int pad = ScaleGUITrad(8);
		const int row = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
		const int left = r.left + pad;
		const int right = r.right - pad;
		const int bottom = r.bottom - pad;
		int y = r.top + pad;

		auto it = _town_treasury.accounts.find(this->town_index);
		if (it == _town_treasury.accounts.end() || !it->second.started) {
			DrawString(left, right, y, TfText("เมืองนี้ยังไม่มีคลังเมือง (ต้องมีประชากร 200 คนขึ้นไป)", "This town has no treasury yet (it needs 200 citizens)"), TC_GREY, SA_LEFT);
			y += row;
			DrawString(left, right, y, fmt::format("{}: {}", TfText("ประชากร", "Population"), static_cast<int64_t>(t->cache.population)), TC_WHITE, SA_LEFT);
			return;
		}
		const TownAccount &a = it->second;
		const int64_t bid = _gold_reserve.GetBidCents();

		/* The treasury. Two columns of label and value. */
		const int mid = left + (right - left) / 2;
		auto pair = [&](int x0, int x1, int yy, const std::string &label, const std::string &value, TextColour col) {
			DrawString(x0, x1, yy, label, TC_LIGHT_BLUE, SA_LEFT);
			DrawString(x0, x1, yy, value, col, SA_RIGHT);
		};
		DrawString(left, right, y, TfText("คลังเมือง", "Town treasury"), TC_LIGHT_BLUE, SA_LEFT);
		y += row;
		pair(left, mid - pad, y, TfText("เงินในคลัง", "Treasury"), TfMoney(a.treasury), TC_WHITE);
		pair(mid + pad, right, y, TfText("ประชากร", "Population"), fmt::format("{}", static_cast<int64_t>(t->cache.population)), TC_WHITE);
		y += row;
		pair(left, mid - pad, y, TfText("ใช้พัฒนาเมืองไปแล้ว", "Spent on development"), TfMoney(a.spent), TC_WHITE);
		pair(mid + pad, right, y, TfText("เก็บภาษีได้ (ตั้งแต่เปิดเกม)", "Taxes collected (this session)"), TfMoney(a.taxed), TC_WHITE);
		y += row;
		const int64_t develop_cost = (TOWN_DEVELOP_BASE_COST + TOWN_DEVELOP_COST_PER_CAPITA * static_cast<int64_t>(t->cache.population)) * GetPriceIndexPpm() / 1'000'000;
		pair(left, mid - pad, y, TfText("ค่าพัฒนาครั้งถัดไป", "Next development costs"), TfMoney(develop_cost), a.treasury >= develop_cost ? TC_GREEN : TC_WHITE);
		pair(mid + pad, right, y, TfText("ราคาทอง (ขายคืน)", "Gold price (bid)"), TfMoney(bid / 100), TC_WHITE);
		y += row + ScaleGUITrad(6);

		/* The four groups of citizens. */
		DrawString(left, right, y, TfText("พลเมือง 4 กลุ่ม", "Citizens: four groups"), TC_LIGHT_BLUE, SA_LEFT);
		y += row;
		const int w = right - left;
		/* Column edges (right edges of the numbers); the first column is the name. */
		const int c_name = left + w * 22 / 100;
		const int c_gold = left + w * 36 / 100;
		const int c_cash = left + w * 53 / 100;
		const int c_cost = left + w * 67 / 100;
		const int c_prof = left + w * 83 / 100;
		const int c_wl   = right;
		DrawString(left, c_name, y, TfText("กลุ่ม", "Group"), TC_GREY, SA_LEFT);
		DrawString(c_name, c_gold, y, TfText("ทอง oz", "Gold oz"), TC_GREY, SA_RIGHT);
		DrawString(c_gold, c_cash, y, TfText("เงินสด", "Cash"), TC_GREY, SA_RIGHT);
		DrawString(c_cash, c_cost, y, TfText("ต้นทุน/oz", "Cost/oz"), TC_GREY, SA_RIGHT);
		DrawString(c_cost, c_prof, y, TfText("กำไรสุทธิ", "Profit"), TC_GREY, SA_RIGHT);
		DrawString(c_prof, c_wl, y, TfText("ชนะ/แพ้", "Win/Loss"), TC_GREY, SA_RIGHT);
		y += row;

		static const char *const names_th[] = {"กักตุน", "เก็งกำไร", "เทรดถี่เกินไป", "สมดุล"};
		static const char *const names_en[] = {"Hoarder", "Speculator", "Overtrader", "Balanced"};
		int64_t sum_gold = 0, sum_cash = 0, sum_profit = 0, sum_wealth = 0;
		for (size_t i = 0; i < citizengold::STYLE_COUNT; i++) {
			if (y + row > bottom) return;
			const citizengold::Book &b = a.books[i];
			sum_gold += b.gold_cz; sum_cash += b.cash; sum_profit += b.profit;
			sum_wealth += citizengold::Wealth(b, bid);
			const int64_t cost_oz = b.gold_cz > 0 ? b.cost * citizengold::CZ_PER_OZ / b.gold_cz : 0;
			DrawString(left, c_name, y, TfText(names_th[i], names_en[i]), TC_WHITE, SA_LEFT);
			DrawString(c_name, c_gold, y, TfOunces(b.gold_cz), TC_WHITE, SA_RIGHT);
			DrawString(c_gold, c_cash, y, TfMoney(b.cash), TC_WHITE, SA_RIGHT);
			DrawString(c_cash, c_cost, y, b.gold_cz > 0 ? TfMoney(cost_oz) : std::string("-"), TC_WHITE, SA_RIGHT);
			DrawString(c_cost, c_prof, y, TfMoney(b.profit), b.profit >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
			DrawString(c_prof, c_wl, y, fmt::format("{}/{}", b.wins, b.losses), TC_WHITE, SA_RIGHT);
			y += row;
		}
		if (y + row > bottom) return;
		DrawString(left, c_name, y, TfText("รวม", "Total"), TC_LIGHT_BLUE, SA_LEFT);
		DrawString(c_name, c_gold, y, TfOunces(sum_gold), TC_LIGHT_BLUE, SA_RIGHT);
		DrawString(c_gold, c_cash, y, TfMoney(sum_cash), TC_LIGHT_BLUE, SA_RIGHT);
		DrawString(c_cost, c_prof, y, TfMoney(sum_profit), sum_profit >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
		y += row + ScaleGUITrad(4);

		/* The open positions of the Trading Terminal (speculators and overtraders). */
		int64_t floating = 0;
		for (const citizengold::TermPos &p : a.positions) {
			if (p.symbol < STOCK_SYMBOLS && IsStockSymbolValid(p.symbol)) {
				const CompanyID c{p.symbol};
				floating += p.is_long ? (GetStockBid(c) - p.entry) * p.volume : (p.entry - GetStockAsk(c)) * p.volume;
			}
		}
		if (y + row > bottom) return;
		pair(left, right, y, TfText("ทรัพย์สินรวมของพลเมือง (เงินสด + ทอง + กำไรลอยตัวใน terminal)", "Total wealth of the citizens (cash + gold + floating profit in the terminal)"), TfMoney(sum_wealth + floating), TC_WHITE);
		y += row + ScaleGUITrad(6);
		if (y + row > bottom) return;
		DrawString(left, right, y, TfText("ตำแหน่งที่เปิดอยู่ใน Trading Terminal", "Open positions in the Trading Terminal"), TC_LIGHT_BLUE, SA_LEFT);
		y += row;
		if (a.positions.empty()) {
			if (y + row > bottom) return;
			DrawString(left, right, y, TfText("ไม่มี (เก็งกำไรและเทรดถี่เปิดเมื่อเห็นสัญญาณ)", "None (speculators and overtraders open when they see a signal)"), TC_GREY, SA_LEFT);
			return;
		}
		const int p_sym = left + w * 40 / 100;
		const int p_dir = left + w * 50 / 100;
		const int p_vol = left + w * 63 / 100;
		const int p_ent = left + w * 80 / 100;
		DrawString(left, c_name, y, TfText("กลุ่ม", "Group"), TC_GREY, SA_LEFT);
		DrawString(c_name, p_sym, y, TfText("สัญลักษณ์", "Symbol"), TC_GREY, SA_LEFT);
		DrawString(p_sym, p_dir, y, TfText("ทิศทาง", "Side"), TC_GREY, SA_LEFT);
		DrawString(p_dir, p_vol, y, TfText("ปริมาณ", "Volume"), TC_GREY, SA_RIGHT);
		DrawString(p_vol, p_ent, y, TfText("ราคาเข้า", "Entry"), TC_GREY, SA_RIGHT);
		DrawString(p_ent, right, y, TfText("กำไร/ขาดทุน", "P/L"), TC_GREY, SA_RIGHT);
		y += row;
		for (const citizengold::TermPos &p : a.positions) {
			if (y + row > bottom) return;
			int64_t pl = 0;
			std::string sym = "?";
			if (p.symbol < STOCK_SYMBOLS && IsStockSymbolValid(p.symbol)) {
				const CompanyID c{p.symbol};
				pl = p.is_long ? (GetStockBid(c) - p.entry) * p.volume : (p.entry - GetStockAsk(c)) * p.volume;
				sym = GetStockSymbolShortName(p.symbol);
			}
			const size_t si = static_cast<size_t>(p.style);
			DrawString(left, c_name, y, TfText(names_th[si], names_en[si]), TC_WHITE, SA_LEFT);
			DrawString(c_name, p_sym, y, sym, TC_WHITE, SA_LEFT);
			DrawString(p_sym, p_dir, y, p.is_long ? TfText("ซื้อ (long)", "Long") : TfText("ขาย (short)", "Short"), p.is_long ? TC_GREEN : TC_RED, SA_LEFT);
			DrawString(p_dir, p_vol, y, fmt::format("{}", p.volume), TC_WHITE, SA_RIGHT);
			DrawString(p_vol, p_ent, y, TfMoney(p.entry), TC_WHITE, SA_RIGHT);
			DrawString(p_ent, right, y, TfMoney(pl), pl >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
			y += row;
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_town_finance_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_CAPTION, COLOUR_BROWN, WID_TFN_CAPTION), SetToolTip(STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_BROWN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_BROWN),
		NWidget(WWT_STICKYBOX, COLOUR_BROWN),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN, WID_TFN_PANEL), SetMinimalSize(560, 340), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_SPACER), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, COLOUR_BROWN),
	EndContainer(),
};

static WindowDesc _town_finance_desc(
	WDP_AUTO, "town_finance", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_town_finance_widgets
);

void ShowTownFinanceWindow(uint32_t town)
{
	if (BringWindowToFrontById(WC_CENTRAL_BANK, TF_WINDOW_BASE + static_cast<int>(town)) != nullptr) return;
	new TownFinanceWindow(_town_finance_desc, town);
}
