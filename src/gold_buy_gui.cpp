/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gold_buy_gui.cpp The "Buy gold" window of the company vault: price, quantity, cost, cash after the purchase, and the confirm button. */

#include "stdafx.h"
#include "3rdparty/fmt/format.h"
#include <chrono>
#include <optional>
#include "gold_panel.h"
#include "gold_reserve.h"
#include "central_bank.h"
#include "stock_gui.h"
#include "graph_gui.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "currency.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "textbuf_gui.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "timer/timer_game_calendar.h"

#include "widgets/central_bank_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** True: the window shows Thai text (as in the design). False: the same window in English only. Change it here and build again. */
static constexpr bool GB_USE_THAI = true;

/** The text in Thai or in English. */
static std::string GbText(const char *thai, const char *english)
{
	return GB_USE_THAI ? std::string(thai) : std::string(english);
}

/** Window number of the buy gold window inside the class #WC_CENTRAL_BANK (0 = bank, 1 = statement, 2 = past statements). */
static constexpr int GB_WINDOW_NUMBER = 3;

/** Largest number of ounces that one order may have (the same limit as the command). */
static constexpr int64_t GB_MAX_ORDER_OZ = 1000000;

/** Money (in 1/100 of a money unit) as text with two decimals, for example 2,032.40. The decimals are only added when the currency is the base one. */
static std::string GbMoney(int64_t cents)
{
	std::string text = GetString(STR_JUST_CURRENCY_LONG, cents / 100);
	if (GetCurrency().rate == 1 && !text.empty() && text.back() >= '0' && text.back() <= '9') {
		const int64_t frac = cents < 0 ? -(cents % 100) : cents % 100;
		text += fmt::format(".{:02}", frac);
	}
	return text;
}

/** All the rectangles of the window, relative to the top left corner of the panel. Used for drawing and for the clicks, so they always agree. */
struct GbLayout {
	Rect heading;     ///< Heading and subtitle (left side).
	Rect badge;       ///< The GOLD badge: opens the gold window.
	Rect price_box;   ///< Dark box with the price per ounce.
	Rect qty_label;   ///< Row with the caption of the quantity and the maximum.
	Rect minus;       ///< Button: one ounce less.
	Rect field;       ///< The quantity field (click to type).
	Rect plus;        ///< Button: one ounce more.
	Rect max_button;  ///< Button: as many ounces as possible.
	Rect quick[3];    ///< Buttons: 10, 50 and 100 ounces.
	Rect summary;     ///< Box with the value of the gold, the fee and the total.
	Rect cash_rows;   ///< The two rows below the box: usable cash and cash after the purchase.
	Rect cancel;      ///< Button: close without buying.
	Rect confirm;     ///< Button: buy.
	Rect footer;      ///< Dark bar at the bottom.
};

static constexpr int GB_QUICK_OUNCES[3] = {10, 50, 100};

/** The layout for a panel of this width and height (in screen pixels). All sizes are in the units of the game and scaled with the GUI. */
static GbLayout GbMakeLayout(int width, int height)
{
	const int m = ScaleGUITrad(14);
	const int w = width;
	GbLayout l;
	l.heading = {m, ScaleGUITrad(12), w - m - ScaleGUITrad(130), ScaleGUITrad(54)};
	l.badge = {w - m - ScaleGUITrad(116), ScaleGUITrad(12), w - m, ScaleGUITrad(52)};
	l.price_box = {m, ScaleGUITrad(62), w - m, ScaleGUITrad(110)};
	l.qty_label = {m, ScaleGUITrad(120), w - m, ScaleGUITrad(134)};

	const int row_top = ScaleGUITrad(138);
	const int row_bottom = ScaleGUITrad(162);
	const int step_w = ScaleGUITrad(26);
	const int max_w = ScaleGUITrad(56);
	const int gap = ScaleGUITrad(6);
	l.minus = {m, row_top, m + step_w, row_bottom};
	l.max_button = {w - m - max_w, row_top, w - m, row_bottom};
	l.plus = {l.max_button.left - gap - step_w, row_top, l.max_button.left - gap, row_bottom};
	l.field = {l.minus.right + gap, row_top, l.plus.left - gap, row_bottom};

	const int quick_w = ScaleGUITrad(56);
	for (int i = 0; i < 3; i++) {
		const int x = m + i * (quick_w + gap);
		l.quick[i] = {x, ScaleGUITrad(168), x + quick_w, ScaleGUITrad(186)};
	}

	l.summary = {m, ScaleGUITrad(196), w - m, ScaleGUITrad(262)};
	l.cash_rows = {m, ScaleGUITrad(270), w - m, ScaleGUITrad(302)};

	const int footer_h = ScaleGUITrad(18);
	l.footer = {0, height - footer_h, w, height};
	const int btn_top = l.footer.top - ScaleGUITrad(36);
	const int btn_bottom = l.footer.top - ScaleGUITrad(10);
	l.cancel = {m, btn_top, m + ScaleGUITrad(92), btn_bottom};
	l.confirm = {l.cancel.right + gap, btn_top, w - m, btn_bottom};
	return l;
}

static Rect GbOffset(const Rect &r, int dx, int dy)
{
	return {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
}

static bool GbHit(const Rect &r, int x, int y)
{
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

/** Why the order can not be placed now, or Ok. */
enum class GbBlock : uint8_t {
	Ok,         ///< The order can be placed.
	Spectator,  ///< There is no company.
	NoMarket,   ///< The gold market has not started.
	NoStock,    ///< The market has no gold for sale.
	NoCash,     ///< There is not enough unborrowed cash even for one ounce.
};

struct GoldBuyWindow : Window {
	int64_t qty = 10; ///< Ounces to buy.

	/** Redraw regularly: the price and the cash change all the time. */
	const IntervalTimer<TimerWindow> refresh_interval = {std::chrono::milliseconds(500), [this](auto) {
		this->SetDirty();
	}};

	GoldBuyWindow(WindowDesc &desc, int64_t start_qty) : Window(desc)
	{
		this->qty = std::clamp<int64_t>(start_qty, 1, GB_MAX_ORDER_OZ);
		this->InitNested(GB_WINDOW_NUMBER);
		this->ClampQuantity();
	}

	/** Cash that gold can be paid from. */
	static int64_t UsableCash()
	{
		return Company::IsValidID(_local_company) ? std::max<int64_t>(0, GetUnborrowedLiquidCash(_local_company)) : 0;
	}

	/** Most ounces that can be bought now: limited by the cash, by the stock of the market and by the size of one order. */
	static int64_t MaxOunces()
	{
		const int64_t price = _gold_reserve.GetPriceCents();
		if (!Company::IsValidID(_local_company) || price <= 0) return 0;
		const int64_t by_cash = _gold_reserve.GetMaxBuyOunces(UsableCash());
		return std::max<int64_t>(0, std::min({by_cash, _gold_reserve.market_stock_oz, GB_MAX_ORDER_OZ}));
	}

	static GbBlock GetBlock()
	{
		if (!Company::IsValidID(_local_company)) return GbBlock::Spectator;
		if (_gold_reserve.GetPriceCents() <= 0) return GbBlock::NoMarket;
		if (_gold_reserve.market_stock_oz <= 0) return GbBlock::NoStock;
		if (MaxOunces() < 1) return GbBlock::NoCash;
		return GbBlock::Ok;
	}

	/** Keep the quantity between 1 and the most that can be bought (it stays 1 when nothing can be bought). */
	void ClampQuantity()
	{
		this->qty = std::clamp<int64_t>(this->qty, 1, std::max<int64_t>(1, MaxOunces()));
	}

	bool CanConfirm() const
	{
		return GetBlock() == GbBlock::Ok && this->qty >= 1 && this->qty <= MaxOunces();
	}

	int64_t TotalCents() const
	{
		return _gold_reserve.GetBuyTotal(this->qty) * 100;
	}

	Rect PanelRect() const
	{
		return this->GetWidget<NWidgetBase>(WID_GB_PANEL)->GetCurrentRect();
	}

	void OnPaint() override
	{
		this->ClampQuantity();
		this->DrawWidgets();
	}

	/** A button of the design: a raised frame with a centred text. Disabled buttons are flat and grey. */
	static void DrawButton(const Rect &r, Colours colour, const std::string &text, bool lowered, bool enabled, FontSize size = FS_NORMAL)
	{
		FrameFlags flags{};
		if (lowered) flags.Set(FrameFlag::Lowered);
		DrawFrameRect(r.left, r.top, r.right, r.bottom, enabled ? colour : COLOUR_GREY, flags);
		const int h = GetCharacterHeight(size);
		DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, h) + (lowered ? 1 : 0), text, enabled ? TC_BLACK : TC_GREY, SA_HOR_CENTER, false, size);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_GB_PANEL) return;

		const GbLayout l = GbMakeLayout(r.Width(), r.Height());
		const int dx = r.left;
		const int dy = r.top;
		auto at = [&](const Rect &rect) { return GbOffset(rect, dx, dy); };

		const PixelColour paper = GetColourGradient(COLOUR_BROWN, SHADE_LIGHTER);
		const PixelColour paper_dark = GetColourGradient(COLOUR_BROWN, SHADE_LIGHTEST);
		const PixelColour ink_box = GetColourGradient(COLOUR_BROWN, SHADE_DARKER);
		const PixelColour ink_bar = GetColourGradient(COLOUR_BROWN, SHADE_DARKEST);
		GfxFillRect(r.left, r.top, r.right, r.bottom, paper);

		const bool valid = Company::IsValidID(_local_company);
		const int64_t price = _gold_reserve.GetPriceCents();
		const bool market_open = price > 0;
		const int64_t max_oz = MaxOunces();
		const int64_t cash = UsableCash();
		const int64_t total = market_open ? this->TotalCents() : 0;
		const int64_t value_cents = market_open ? _gold_reserve.GetBuyCost(this->qty) * 100 : 0;
		const int64_t fee_cents = market_open ? total - value_cents : 0;
		const int h_norm = GetCharacterHeight(FS_NORMAL);
			const int h_large = GetCharacterHeight(FS_LARGE);
		const int pad = ScaleGUITrad(10);

		/* Heading and subtitle. */
		{
			const Rect hd = at(l.heading);
			DrawString(hd.left, hd.right, hd.top, GbText("ซื้อทองเข้าคลัง", "Buy gold for the vault"), TC_BLACK, SA_LEFT, false, FS_LARGE);
			DrawString(hd.left, hd.right, hd.top + h_large + ScaleGUITrad(4), GbText("ทองคำแท่ง · หน่วยซื้อขายเป็นออนซ์ (oz)", "Gold bars · traded in ounces (oz)"), TC_BLACK, SA_LEFT, false, FS_NORMAL);
		}

		/* The GOLD badge: opens the gold window (the Gold Investment Types window). */
		{
			const Rect b = at(l.badge);
			DrawFrameRect(b.left, b.top, b.right, b.bottom, COLOUR_YELLOW, {});
			DrawString(b.left + ScaleGUITrad(8), b.right - ScaleGUITrad(8), b.top + ScaleGUITrad(4), "GOLD", TC_BLACK, SA_LEFT, false, FS_LARGE);
			DrawString(b.left + ScaleGUITrad(8), b.right - ScaleGUITrad(4), b.bottom - h_norm - ScaleGUITrad(4), GbText("ดูกราฟราคาทอง", "View gold price"), TC_BLACK, SA_LEFT, false, FS_NORMAL);
		}

		/* The price box. */
		{
			const Rect pb = at(l.price_box);
			GfxFillRect(pb.left, pb.top, pb.right, pb.bottom, ink_box);
			const int left = pb.left + pad;
			const int right = pb.right - pad;
			DrawString(left, right, pb.top + ScaleGUITrad(5), GbText("ราคาซื้อต่อออนซ์", "Buy price per ounce"), TC_SILVER, SA_LEFT, false, FS_NORMAL);
			const std::string price_text = market_open ? GbMoney(_gold_reserve.GetAskCents()) : std::string("-");
			const int price_y = pb.bottom - h_large - ScaleGUITrad(5);
			DrawString(left, right, price_y, price_text, TC_GOLD, SA_LEFT, false, FS_LARGE);
			const int unit_x = left + GetStringBoundingBox(price_text, FS_LARGE).width + ScaleGUITrad(6);
			if (unit_x < right) DrawString(unit_x, right, price_y + h_large - h_norm - ScaleGUITrad(1), "/ oz", TC_SILVER, SA_LEFT, false, FS_NORMAL);

			/* Market state and stock on the right. */
			const int dot = ScaleGUITrad(6);
			const std::string state = market_open ? GbText("ตลาดเปิด", "Market open") : GbText("ตลาดปิด", "Market closed");
			const int state_w = GetStringBoundingBox(state, FS_NORMAL).width;
			const int state_y = pb.top + ScaleGUITrad(5);
			DrawString(right - state_w, right, state_y, state, TC_WHITE, SA_RIGHT, false, FS_NORMAL);
			const int dot_x = right - state_w - ScaleGUITrad(6) - dot;
			const int dot_y = state_y + (h_norm - dot) / 2;
			GfxFillRect(dot_x, dot_y, dot_x + dot - 1, dot_y + dot - 1, market_open ? PC_GREEN : PC_RED);
			const std::string stock = GbText("มีจำหน่าย ", "Available ") + fmt::format("{} oz", _gold_reserve.market_stock_oz);
			DrawString(left, right, pb.bottom - h_norm - ScaleGUITrad(6), stock, TC_SILVER, SA_RIGHT, false, FS_NORMAL);
		}

		/* Caption of the quantity, and the most that can be bought. */
		{
			const Rect ql = at(l.qty_label);
			DrawString(ql.left, ql.right, ql.top, GbText("จำนวนที่ต้องการซื้อ", "Ounces to buy"), TC_BLACK, SA_LEFT, false, FS_NORMAL);
			const std::string most = GbText("ซื้อได้สูงสุด ", "Most you can buy ") + fmt::format("{} oz", max_oz);
			DrawString(ql.left, ql.right, ql.top + 0, most, TC_BLACK, SA_RIGHT, false, FS_NORMAL);
		}

		/* The quantity row. */
		DrawButton(at(l.minus), COLOUR_BROWN, "-", false, valid && this->qty > 1, FS_LARGE);
		DrawButton(at(l.plus), COLOUR_BROWN, "+", false, valid && this->qty < max_oz, FS_LARGE);
		DrawButton(at(l.max_button), COLOUR_BROWN, GbText("สูงสุด", "Max"), false, max_oz >= 1);
		{
			const Rect f = at(l.field);
			DrawFrameRect(f.left, f.top, f.right, f.bottom, COLOUR_BROWN, FrameFlags{FrameFlag::Lowered});
			const int fl = f.left + ScaleGUITrad(6);
			const int fr = f.right - ScaleGUITrad(6);
			const std::string unit = " oz";
			const int unit_w = GetStringBoundingBox(unit, FS_NORMAL).width;
			DrawString(fr - unit_w, fr, CentreBounds(f.top, f.bottom, h_norm), unit, TC_BLACK, SA_RIGHT, false, FS_NORMAL);
			DrawString(fl, fr - unit_w - ScaleGUITrad(2), CentreBounds(f.top, f.bottom, h_large), fmt::format("{}", this->qty), TC_BLACK, SA_RIGHT, false, FS_LARGE);
		}
		for (int i = 0; i < 3; i++) {
			DrawButton(at(l.quick[i]), COLOUR_BROWN, fmt::format("{} oz", GB_QUICK_OUNCES[i]), this->qty == GB_QUICK_OUNCES[i], max_oz >= 1, FS_NORMAL);
		}

		/* The summary box. */
		{
			const Rect sb = at(l.summary);
			GfxFillRect(sb.left, sb.top, sb.right, sb.bottom, paper_dark);
			DrawRectOutline(sb, ink_bar);
			const int left = sb.left + pad;
			const int right = sb.right - pad;
			const int row = ScaleGUITrad(17);
			int y = sb.top + ScaleGUITrad(6);
			DrawString(left, right, y, GbText("มูลค่าทองคำ ", "Value of ") + fmt::format("{} oz", this->qty), TC_BLACK, SA_LEFT);
			DrawString(left, right, y, market_open ? GbMoney(value_cents) : std::string("-"), TC_BLACK, SA_RIGHT);
			y += row;
			DrawString(left, right, y, GbText("ค่าธรรมเนียม", "Fee"), TC_BLACK, SA_LEFT);
			DrawString(left, right, y, GbMoney(fee_cents), TC_BLACK, SA_RIGHT);
			y += row;
			GfxDrawLine(left, y, right, y, ink_bar);
			y += ScaleGUITrad(5);
			DrawString(left, right, CentreBounds(y, y + h_large, h_norm), GbText("ยอดชำระทั้งหมด", "Total to pay"), TC_BLACK, SA_LEFT);
			DrawString(left, right, y, market_open ? GbMoney(total) : std::string("-"), TC_BLACK, SA_RIGHT, false, FS_LARGE);
		}

		/* The cash rows. */
		{
			const Rect cr = at(l.cash_rows);
			int y = cr.top;
			const int row = ScaleGUITrad(17);
			DrawString(cr.left, cr.right, y, GbText("เงินสดที่ใช้ได้", "Usable cash"), TC_BLACK, SA_LEFT);
			DrawString(cr.left, cr.right, y, GbMoney(cash * 100), TC_BLACK, SA_RIGHT);
			y += row;
			const int64_t after = cash * 100 - total;
			DrawString(cr.left, cr.right, y, GbText("เงินคงเหลือหลังซื้อ", "Cash left after buying"), TC_BLACK, SA_LEFT);
			DrawString(cr.left, cr.right, y, GbMoney(after), after < 0 ? TC_RED : TC_BLACK, SA_RIGHT);
		}

		/* The buttons. */
		DrawButton(at(l.cancel), COLOUR_BROWN, GbText("ยกเลิก", "Cancel"), false, true);
		DrawButton(at(l.confirm), COLOUR_YELLOW, GbText("ยืนยันซื้อทอง · ", "Confirm buying gold · ") + fmt::format("{} oz", this->qty), false, this->CanConfirm());

		/* The footer: the note on the left (or the reason why nothing can be bought), the date on the right. */
		{
			const Rect fb = at(l.footer);
			GfxFillRect(fb.left, fb.top, fb.right, fb.bottom, ink_bar);
			const int left = fb.left + ScaleGUITrad(8);
			const int right = fb.right - ScaleGUITrad(8);
			const int y = CentreBounds(fb.top, fb.bottom, h_norm);
			const std::string date = GetString(STR_JUST_DATE_LONG, TimerGameCalendar::date);
			const int date_w = GetStringBoundingBox(date, FS_NORMAL).width;
			DrawString(right - date_w, right, y, date, TC_SILVER, SA_RIGHT, false, FS_NORMAL);

			std::string note = GbText("ทองที่ซื้อจะถูกเพิ่มเข้าคลังบริษัททันที", "The gold is added to the company vault at once");
			TextColour note_colour = TC_SILVER;
			switch (GetBlock()) {
				case GbBlock::NoMarket: note = GbText("ตลาดทองยังไม่เปิด", "The gold market has not started yet"); note_colour = TC_RED; break;
				case GbBlock::NoStock:  note = GbText("ตลาดไม่มีทองเหลือขาย", "The market has no gold for sale"); note_colour = TC_RED; break;
				case GbBlock::NoCash:   note = GbText("เงินสดที่ไม่ได้กู้ไม่พอซื้อทอง", "Not enough unborrowed cash"); note_colour = TC_RED; break;
				default: break;
			}
			const int note_right = right - date_w - ScaleGUITrad(8);
			if (note_right > left) DrawString(left, note_right, y, note, note_colour, SA_LEFT, false, FS_NORMAL);
		}
	}

	void SetQuantity(int64_t value)
	{
		this->qty = value;
		this->ClampQuantity();
		this->SetDirty();
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_GB_PANEL) return;
		const Rect panel = this->PanelRect();
		const GbLayout l = GbMakeLayout(panel.Width(), panel.Height());
		const int x = pt.x - panel.left;
		const int y = pt.y - panel.top;
		const int64_t max_oz = MaxOunces();

		if (GbHit(l.badge, x, y)) {
			ShowOperatingProfitGraph();
		} else if (GbHit(l.minus, x, y)) {
			if (max_oz >= 1) this->SetQuantity(this->qty - 1);
		} else if (GbHit(l.plus, x, y)) {
			if (max_oz >= 1) this->SetQuantity(this->qty + 1);
		} else if (GbHit(l.max_button, x, y)) {
			if (max_oz >= 1) this->SetQuantity(max_oz);
		} else if (GbHit(l.field, x, y)) {
			if (Company::IsValidID(_local_company)) ShowQueryString({}, STR_GOLD_QUERY_BUY, 10, this, CS_NUMERAL, {});
		} else if (GbHit(l.cancel, x, y)) {
			this->Close();
		} else if (GbHit(l.confirm, x, y)) {
			/* Check again: a click can arrive in the same frame as a change of the cash or of the price. */
			if (this->CanConfirm()) {
				Command<CMD_GOLD_BUY>::Post(STR_GOLD_ERROR_CANT_BUY, static_cast<uint32_t>(this->qty));
				this->SetDirty(); // The window stays open: the cash and the stock change, and the player can buy again.
			}
		} else {
			for (int i = 0; i < 3; i++) {
				if (GbHit(l.quick[i], x, y) && max_oz >= 1) this->SetQuantity(GB_QUICK_OUNCES[i]);
			}
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value()) return;
		double ounces = 0;
		if (!ParseStockNumber(*str, ounces) || ounces < 1) return;
		this->SetQuantity(static_cast<int64_t>(std::min(ounces, static_cast<double>(GB_MAX_ORDER_OZ))));
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_gold_buy_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_CAPTION, COLOUR_BROWN, WID_GB_CAPTION), SetStringTip(STR_GOLD_BUY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, COLOUR_BROWN),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN, WID_GB_PANEL), SetMinimalSize(430, 376), SetFill(1, 1),
	EndContainer(),
};

static WindowDesc _gold_buy_desc(
	WDP_CENTER, "gold_buy", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_gold_buy_widgets
);

/**
 * Open the Buy gold window of the company vault (window number 3 of #WC_CENTRAL_BANK), or bring it to the front.
 * @param start_qty Ounces in the quantity field when the window opens.
 */
void ShowGoldBuyWindow(int64_t start_qty)
{
	if (!Company::IsValidID(_local_company)) return;
	if (BringWindowToFrontById(WC_CENTRAL_BANK, GB_WINDOW_NUMBER) != nullptr) return;
	new GoldBuyWindow(_gold_buy_desc, start_qty);
}
