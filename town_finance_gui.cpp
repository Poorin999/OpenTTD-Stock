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
#include "town_wallet_math.h"
#include "central_bank.h"
#include "gold_reserve.h"
#include "stock_base.h"
#include <algorithm>
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
		/* More open positions need more height: grow the window by itself (the resize box still makes it bigger). */
		if (this->NeededHeight() != this->shown_needed_h) this->ReInit();
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

	/** The colours of the window (the look of the finance window of the owner's picture: tan rows, teal title strips, dark brown table heads). Change here and build again. */
	struct Look {
		PixelColour strip, row_a, row_b, head, total, line;
		Look() :
			strip(GetColourGradient(COLOUR_LIGHT_BLUE, SHADE_DARKER)),
			row_a(GetColourGradient(COLOUR_BROWN, SHADE_LIGHT)),
			row_b(GetColourGradient(COLOUR_BROWN, SHADE_NORMAL)),
			head(GetColourGradient(COLOUR_BROWN, SHADE_DARK)),
			total(GetColourGradient(COLOUR_GREY, SHADE_LIGHT)),
			line(GetColourGradient(COLOUR_BROWN, SHADE_DARK)) {}
	};

	/** The height that the window needs for the text of today (more open positions need more height). */
	int NeededHeight() const
	{
		return this->Paint({0, 0, ScaleGUITrad(760), 0}, false);
	}

	/** Make the window taller by itself when the text needs more height than it has (the resize box still works for a bigger window). */
	int shown_needed_h = 0;

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_TFN_PANEL) return;
		this->shown_needed_h = this->NeededHeight();
		size.width = std::max<uint>(size.width, ScaleGUITrad(760));
		size.height = std::max<uint>(size.height, this->shown_needed_h);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_TFN_PANEL) return;
		this->Paint(r, true);
	}

	/**
	 * Draw the window, or only measure it.
	 * @param r The panel.
	 * @param draw False: nothing is drawn, only the height is worked out (UpdateWidgetSize).
	 * @return The height that all the text needs.
	 */
	int Paint(const Rect &r, bool draw) const
	{
		const Town *t = Town::GetIfValid(static_cast<TownID>(this->town_index));
		if (t == nullptr) return 0;

		const Look look;
		const int pad = ScaleGUITrad(8);
		const int cell = ScaleGUITrad(8);                                  ///< Space inside a cell.
		const int fh = GetCharacterHeight(FS_NORMAL);
		const int rh = std::max(fh, GetCharacterHeight(FS_MONO)) + ScaleGUITrad(10); ///< Height of a row.
		const int hdr = fh + ScaleGUITrad(10);                             ///< Height of a teal title strip.
		const int gap = ScaleGUITrad(10);
		const int left = r.left + pad;
		const int right = r.right - pad;
		int y = r.top + pad;

		/* Small drawing helpers; all of them do nothing when only measuring. */
		auto fill = [&](int x0, int y0, int x1, int y1, PixelColour c) { if (draw) GfxFillRect(x0, y0, x1, y1, c); };
		auto text = [&](int x0, int x1, int top, int height, const std::string &s, TextColour col, StringAlignment al, FontSize fs = FS_NORMAL) {
			if (draw) DrawString(x0, x1, top + (height - GetCharacterHeight(fs)) / 2, s, col, al, false, fs);
		};
		auto vline = [&](int x, int y0, int y1) { if (draw) GfxFillRect(x, y0, x, y1, look.line); };
		auto outline = [&](int y0, int y1) { if (draw) DrawRectOutline({left, y0, right, y1}, look.line); };
		/* The teal strip with the title of a block. */
		auto strip = [&](const std::string &title) {
			fill(left, y, right, y + hdr - 1, look.strip);
			text(left + cell, right - cell, y, hdr, title, TC_WHITE, SA_LEFT);
			y += hdr;
		};
		/* Light text on the tan rows turns into dark text, yellow into orange, green into dark green. */
		auto on_tan = [](TextColour c) -> TextColour {
			switch (c) {
				case TC_WHITE: case TC_LIGHT_BLUE: return TC_BLACK;
				case TC_YELLOW: return TC_ORANGE;
				case TC_GREEN: return TC_DARK_GREEN;
				default: return c;
			}
		};

		auto it = _town_treasury.accounts.find(this->town_index);
		if (it == _town_treasury.accounts.end() || !it->second.started) {
			const int top = y;
			strip(TfText("คลังเมือง", "Town treasury"));
			fill(left, y, right, y + 2 * rh - 1, look.row_a);
			text(left + cell, right - cell, y, rh, TfText("เมืองนี้ยังไม่มีคลังเมือง (ต้องมีประชากร 200 คนขึ้นไป)", "This town has no treasury yet (it needs 200 citizens)"), TC_GREY, SA_LEFT);
			y += rh;
			fill(left, y, right, y + rh - 1, look.row_b);
			text(left + cell, right - cell, y, rh, TfText("ประชากร", "Population"), TC_BLACK, SA_LEFT);
			text(left + cell, right - cell, y, rh, fmt::format("{}", static_cast<int64_t>(t->cache.population)), TC_BLACK, SA_RIGHT, FS_MONO);
			y += rh;
			outline(top, y - 1);
			return y - r.top + pad;
		}
		const TownAccount &a = it->second;
		const int64_t bid = _gold_reserve.GetBidCents();
		const int mid = left + (right - left) / 2;

		/* A block of rows with two columns (label and value on both sides of a vertical line). `n` = index of the row inside the block (the colour alternates). */
		int block_top = 0, n = 0;
		auto begin_block = [&](const std::string &title) { strip(title); block_top = y; n = 0; };
		auto pair = [&](const std::string &l_label, const std::string &l_value, TextColour l_col, const std::string &r_label, const std::string &r_value, TextColour r_col) {
			fill(left, y, right, y + rh - 1, (n % 2 == 0) ? look.row_a : look.row_b);
			text(left + cell, mid - cell, y, rh, l_label, TC_BLACK, SA_LEFT);
			text(left + cell, mid - cell, y, rh, l_value, on_tan(l_col), SA_RIGHT, FS_MONO);
			text(mid + cell, right - cell, y, rh, r_label, TC_BLACK, SA_LEFT);
			text(mid + cell, right - cell, y, rh, r_value, on_tan(r_col), SA_RIGHT, FS_MONO);
			vline(mid, y, y + rh - 1);
			y += rh; n++;
		};
		auto end_block = [&]() { outline(block_top - hdr, y - 1); y += gap; };

		/* The treasury. */
		begin_block(TfText("คลังเมือง", "Town treasury"));
		pair(TfText("เงินในคลัง", "Treasury"), TfMoney(a.treasury), TC_WHITE, TfText("ประชากร", "Population"), fmt::format("{}", static_cast<int64_t>(t->cache.population)), TC_WHITE);
		pair(TfText("ใช้พัฒนาเมืองไปแล้ว", "Spent on development"), TfMoney(a.spent), TC_WHITE, TfText("เก็บภาษีได้ (ตั้งแต่เปิดเกม)", "Taxes collected (this session)"), TfMoney(a.taxed), TC_WHITE);
		const int64_t develop_cost = (TOWN_DEVELOP_BASE_COST + TOWN_DEVELOP_COST_PER_CAPITA * static_cast<int64_t>(t->cache.population)) * GetPriceIndexPpm() / 1'000'000;
		pair(TfText("ค่าพัฒนาครั้งถัดไป", "Next development costs"), TfMoney(develop_cost), a.treasury >= develop_cost ? TC_GREEN : TC_WHITE, TfText("ราคาทอง (ขายคืน)", "Gold price (bid)"), TfMoney(bid / 100), TC_WHITE);
		end_block();

		/* The city wallet: what the town pays for consumer cargo, its debts, the business tax and the grants (town_wallet.cpp). */
		{
			const int64_t scaled_pop = std::max<int64_t>(1, static_cast<int64_t>(t->cache.population) * GetPriceIndexPpm() / 1'000'000);
			const int64_t loan_room = std::max<int64_t>(0, townwallet::LoanLimit(scaled_pop) - a.loan_debt);
			const int64_t bond_room = std::max<int64_t>(0, townwallet::BondLimit(scaled_pop) - a.bond_debt);
			begin_block(TfText("กระเป๋าเมือง (City wallet)", "City wallet"));
			pair(TfText("เงินกู้ธนาคารกลาง", "Loan from the central bank"), TfMoney(a.loan_debt), a.loan_debt > 0 ? TC_YELLOW : TC_WHITE, TfText("พันธบัตรเทศบาล", "Municipal bonds"), TfMoney(a.bond_debt), a.bond_debt > 0 ? TC_YELLOW : TC_WHITE);
			pair(TfText("จ่ายค่าสินค้าให้บริษัท", "Paid for Goods / Food / Water"), TfMoney(a.goods_paid), TC_WHITE, TfText("จ่ายไม่ไหว (บริษัทได้น้อยลง)", "Could not pay"), TfMoney(a.shortfall), a.shortfall > 0 ? TC_RED : TC_WHITE);
			pair(TfText("ภาษีธุรกิจที่เก็บได้", "Business tax collected"), TfMoney(a.business_tax), TC_WHITE, TfText("เงินอุดหนุนจากธนาคารกลาง", "Grants from the central bank"), TfMoney(a.grants), TC_WHITE);
			pair(TfText("ดอกเบี้ยที่จ่ายแล้ว", "Interest paid"), TfMoney(a.interest_paid), TC_WHITE, TfText("วงเงินกู้ / พันธบัตรที่เหลือ", "Credit left (loan / bonds)"), fmt::format("{} / {}", TfMoney(loan_room), TfMoney(bond_room)), TC_WHITE);
			pair(TfText("ค่าโดยสารที่พลเมืองจ่าย", "Fares paid by the citizens"), TfMoney(a.fares_paid), TC_WHITE, TfText("ค่าไปรษณีย์ (จากคลัง) / จ่ายไม่ไหว", "Mail (treasury) / could not pay"), fmt::format("{} / {}", TfMoney(a.mail_paid), TfMoney(a.fare_shortfall)), a.fare_shortfall > 0 ? TC_RED : TC_WHITE);
			pair(TfText("ค่าจ้างที่ไหลกลับพลเมือง", "Wages back to the citizens"), TfMoney(a.wages_back), TC_WHITE, TfText("ภาษีที่ธนาคารคืนให้เมือง", "Tax back from the bank"), TfMoney(a.tax_back), TC_WHITE);
			end_block();
		}

		/* Column edges of a table: the right edges of the cells; the first cell is the name. `edges` has `count` entries, the last is `right`. */
		auto table_head = [&](const int *edges, const std::string *heads, int count) {
			fill(left, y, right, y + rh - 1, look.head);
			int x0 = left;
			for (int i = 0; i < count; i++) {
				text(x0 + cell, edges[i] - cell, y, rh, heads[i], TC_WHITE, i == 0 ? SA_LEFT : SA_RIGHT);
				if (i + 1 < count) vline(edges[i], y, y + rh - 1);
				x0 = edges[i];
			}
			y += rh;
		};

		/* The four groups of citizens. */
		const int w = right - left;
		const int c_name = left + w * 22 / 100;
		const int c_gold = left + w * 36 / 100;
		const int c_cash = left + w * 53 / 100;
		const int c_cost = left + w * 67 / 100;
		const int c_prof = left + w * 83 / 100;
		const int c_wl   = right;
		const int edges[6] = {c_name, c_gold, c_cash, c_cost, c_prof, c_wl};
		const int table_top = y;
		strip(TfText("พลเมือง 4 กลุ่ม", "Citizens: four groups"));
		{
			const std::string heads[6] = {TfText("กลุ่ม", "Group"), TfText("ทอง oz", "Gold oz"), TfText("เงินสด", "Cash"), TfText("ต้นทุน/oz", "Cost/oz"), TfText("กำไรสุทธิ", "Profit"), TfText("ชนะ/แพ้", "Win/Loss")};
			table_head(edges, heads, 6);
		}
		auto cell_row = [&](const int *e, int count, const std::string *cells, const TextColour *cols, bool mono_from_1, PixelColour bg) {
			fill(left, y, right, y + rh - 1, bg);
			int x0 = left;
			for (int i = 0; i < count; i++) {
				if (i == 0) text(x0 + cell, e[i] - cell, y, rh, cells[i], on_tan(cols[i]), SA_LEFT);
				else text(x0 + cell, e[i] - cell, y, rh, cells[i], on_tan(cols[i]), SA_RIGHT, mono_from_1 ? FS_MONO : FS_NORMAL);
				if (i + 1 < count) vline(e[i], y, y + rh - 1);
				x0 = e[i];
			}
			y += rh;
		};

		static const char *const names_th[] = {"กักตุน", "เก็งกำไร", "เทรดถี่เกินไป", "สมดุล"};
		static const char *const names_en[] = {"Hoarder", "Speculator", "Overtrader", "Balanced"};
		int64_t sum_gold = 0, sum_cash = 0, sum_profit = 0, sum_wealth = 0;
		for (size_t i = 0; i < citizengold::STYLE_COUNT; i++) {
			const citizengold::Book &b = a.books[i];
			sum_gold += b.gold_cz; sum_cash += b.cash; sum_profit += b.profit;
			sum_wealth += citizengold::Wealth(b, bid);
			const int64_t cost_oz = b.gold_cz > 0 ? b.cost * citizengold::CZ_PER_OZ / b.gold_cz : 0;
			const std::string cells[6] = {TfText(names_th[i], names_en[i]), TfOunces(b.gold_cz), TfMoney(b.cash), b.gold_cz > 0 ? TfMoney(cost_oz) : std::string("-"), TfMoney(b.profit), fmt::format("{}/{}", b.wins, b.losses)};
			const TextColour cols[6] = {TC_WHITE, TC_WHITE, TC_WHITE, TC_WHITE, b.profit >= 0 ? TC_GREEN : TC_RED, TC_WHITE};
			cell_row(edges, 6, cells, cols, true, (i % 2 == 0) ? look.row_a : look.row_b);
		}
		{
			const std::string cells[6] = {TfText("รวม", "Total"), TfOunces(sum_gold), TfMoney(sum_cash), std::string(), TfMoney(sum_profit), std::string()};
			const TextColour cols[6] = {TC_DARK_BLUE, TC_DARK_BLUE, TC_DARK_BLUE, TC_DARK_BLUE, sum_profit >= 0 ? TC_DARK_GREEN : TC_RED, TC_DARK_BLUE};
			cell_row(edges, 6, cells, cols, true, look.total);
		}

		/* The total wealth of the citizens, as the last row of the block. */
		int64_t floating = 0;
		for (const citizengold::TermPos &p : a.positions) {
			if (p.symbol < STOCK_SYMBOLS && IsStockSymbolValid(p.symbol)) {
				const CompanyID c{p.symbol};
				floating += p.is_long ? (GetStockBid(c) - p.entry) * p.volume : (p.entry - GetStockAsk(c)) * p.volume;
			}
		}
		fill(left, y, right, y + rh - 1, look.row_a);
		text(left + cell, right - cell, y, rh, TfText("ทรัพย์สินรวมของพลเมือง (เงินสด + ทอง + กำไรลอยตัวใน terminal)", "Total wealth of the citizens (cash + gold + floating profit in the terminal)"), TC_DARK_BLUE, SA_LEFT);
		text(left + cell, right - cell, y, rh, TfMoney(sum_wealth + floating), TC_BLACK, SA_RIGHT, FS_MONO);
		y += rh;
		outline(table_top, y - 1);
		y += gap;

		/* The open positions of the Trading Terminal (speculators and overtraders). */
		const int pos_top = y;
		strip(TfText("ตำแหน่งที่เปิดอยู่ใน Trading Terminal", "Open positions in the Trading Terminal"));
		if (a.positions.empty()) {
			fill(left, y, right, y + rh - 1, look.row_a);
			text(left + cell, right - cell, y, rh, TfText("ไม่มี (เก็งกำไรและเทรดถี่เปิดเมื่อเห็นสัญญาณ)", "None (speculators and overtraders open when they see a signal)"), TC_GREY, SA_LEFT);
			y += rh;
			outline(pos_top, y - 1);
			return y - r.top + pad;
		}
		const int p_sym = left + w * 40 / 100;
		const int p_dir = left + w * 50 / 100;
		const int p_vol = left + w * 63 / 100;
		const int p_ent = left + w * 80 / 100;
		{
			/* This table has two left-aligned columns (symbol, side), so it draws its own head. */
			const int pe[6] = {c_name, p_sym, p_dir, p_vol, p_ent, right};
			const std::string heads[6] = {TfText("กลุ่ม", "Group"), TfText("สัญลักษณ์", "Symbol"), TfText("ทิศทาง", "Side"), TfText("ปริมาณ", "Volume"), TfText("ราคาเข้า", "Entry"), TfText("กำไร/ขาดทุน", "P/L")};
			fill(left, y, right, y + rh - 1, look.head);
			int x0 = left;
			for (int i = 0; i < 6; i++) {
				text(x0 + cell, pe[i] - cell, y, rh, heads[i], TC_WHITE, i <= 2 ? SA_LEFT : SA_RIGHT);
				if (i + 1 < 6) vline(pe[i], y, y + rh - 1);
				x0 = pe[i];
			}
			y += rh;
			size_t row_i = 0;
			for (const citizengold::TermPos &p : a.positions) {
				int64_t pl = 0;
				std::string sym = "?";
				if (p.symbol < STOCK_SYMBOLS && IsStockSymbolValid(p.symbol)) {
					const CompanyID c{p.symbol};
					pl = p.is_long ? (GetStockBid(c) - p.entry) * p.volume : (p.entry - GetStockAsk(c)) * p.volume;
					sym = GetStockSymbolShortName(p.symbol);
				}
				const size_t si = static_cast<size_t>(p.style);
				fill(left, y, right, y + rh - 1, (row_i++ % 2 == 0) ? look.row_a : look.row_b);
				const std::string cells[6] = {TfText(names_th[si], names_en[si]), sym, p.is_long ? TfText("ซื้อ (long)", "Long") : TfText("ขาย (short)", "Short"), fmt::format("{}", p.volume), TfMoney(p.entry), TfMoney(pl)};
				const TextColour cols[6] = {TC_BLACK, TC_BLACK, p.is_long ? TC_DARK_GREEN : TC_RED, TC_BLACK, TC_BLACK, pl >= 0 ? TC_DARK_GREEN : TC_RED};
				int cx0 = left;
				for (int i = 0; i < 6; i++) {
					const bool mono = i >= 3 || i == 1; // numbers and the symbol; the side has Thai letters
					text(cx0 + cell, pe[i] - cell, y, rh, cells[i], cols[i], i <= 2 ? SA_LEFT : SA_RIGHT, mono ? FS_MONO : FS_NORMAL);
					if (i + 1 < 6) vline(pe[i], y, y + rh - 1);
					cx0 = pe[i];
				}
				y += rh;
			}
		}
		outline(pos_top, y - 1);
		return y - r.top + pad;
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
	NWidget(WWT_PANEL, COLOUR_BROWN, WID_TFN_PANEL), SetMinimalSize(760, 460), SetFill(1, 1), SetResize(1, 1),
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
