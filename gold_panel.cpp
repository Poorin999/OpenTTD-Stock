/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gold_panel.cpp The gold trading panel (cards with the spot price, backing, credit standing, holdings), the gold price chart, and the Buy / Sell actions. */

#include "stdafx.h"
#include "3rdparty/fmt/format.h"
#include "gold_panel.h"
#include "economy_news.h"
#include "gold_reserve.h"
#include "central_bank.h"
#include "stock_gui.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "textbuf_gui.h"
#include "window_func.h"
#include "zoom_func.h"
#include <algorithm>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

/** True: the panel shows Thai text. False: the same panel in English only. Change it here and build again (like USE_THAI in central_bank_gui.cpp). */
static constexpr bool GP_USE_THAI = true;

/** The text in Thai or in English. */
static std::string GpText(const char *thai, const char *english)
{
	return GP_USE_THAI ? std::string(thai) : std::string(english);
}

/** Why the Buy Gold button can not be used now. */
enum class GoldBuyBlock : uint8_t {
	Ok,        ///< It can be used.
	Spectator, ///< There is no company.
	NoMarket,  ///< The gold market has not started.
	BadQty,    ///< The number of ounces is not valid.
	NoStock,   ///< The market does not have that many ounces for sale.
	Borrowed,  ///< The money that is there is (partly) borrowed from the central bank.
	NoCash,    ///< There is not enough cash.
};

/** Check whether the company can buy this many ounces now: the gold is paid from the unborrowed liquid cash only. */
static GoldBuyBlock GetGoldBuyBlock(int64_t ounces)
{
	if (!Company::IsValidID(_local_company)) return GoldBuyBlock::Spectator;
	if (_gold_reserve.GetPriceCents() <= 0) return GoldBuyBlock::NoMarket;
	if (ounces <= 0 || ounces > 1000000) return GoldBuyBlock::BadQty;
	if (_gold_reserve.market_stock_oz < ounces) return GoldBuyBlock::NoStock;
	const int64_t cost = _gold_reserve.GetBuyTotal(ounces); // the gold and the commission
	if (cost <= GetUnborrowedLiquidCash(_local_company)) return GoldBuyBlock::Ok;
	return static_cast<int64_t>(Company::Get(_local_company)->money) >= cost ? GoldBuyBlock::Borrowed : GoldBuyBlock::NoCash;
}

bool GoldPanel::CanBuy() const
{
	return GetGoldBuyBlock(this->buy_qty) == GoldBuyBlock::Ok;
}

/** The Buy Gold button opens the buy window; the window itself checks cash, stock and borrowed money. */
bool GoldPanel::CanOpenBuyWindow() const
{
	return Company::IsValidID(_local_company) && _gold_reserve.GetPriceCents() > 0;
}

bool GoldPanel::CanSell() const
{
	return Company::IsValidID(_local_company) && this->sell_qty > 0 && _gold_reserve.GetHoldings(_local_company) >= this->sell_qty;
}

void GoldPanel::Buy() const
{
	/* Check again: a click can arrive in the same frame as a change of the cash. */
	if (this->CanBuy()) Command<CMD_GOLD_BUY>::Post(STR_GOLD_ERROR_CANT_BUY, static_cast<uint32_t>(this->buy_qty));
}

void GoldPanel::Sell() const
{
	if (this->CanSell()) Command<CMD_GOLD_SELL>::Post(STR_GOLD_ERROR_CANT_SELL, static_cast<uint32_t>(this->sell_qty));
}

/** Open the query window for the number of ounces; its answer comes to #QueryFinished. */
void GoldPanel::AskQuantity(Window *parent, bool buy)
{
	if (!Company::IsValidID(_local_company)) return;
	this->query_buy = buy;
	ShowQueryString({}, buy ? STR_GOLD_QUERY_BUY : STR_GOLD_QUERY_SELL, 10, parent, CS_NUMERAL, {});
}

void GoldPanel::QueryFinished(const std::optional<std::string> &text)
{
	if (!text.has_value()) return;
	double ounces = 0;
	if (!ParseStockNumber(*text, ounces) || ounces < 1) return;
	(this->query_buy ? this->buy_qty : this->sell_qty) = static_cast<int64_t>(std::min(ounces, 1000000.0));
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Design: a tan paper with light cards, a header with the gold bar icon and the GOLD button (it switches to the chart). */
/* ------------------------------------------------------------------------------------------------------------------ */

/** The colours of the panel, all from the brown palette of the window (so they follow the palette of the game). */
struct GpColours {
	PixelColour paper;   ///< Background of the panel.
	PixelColour card;    ///< Light cards (spot price, chart).
	PixelColour row_alt; ///< Every second row of the table.
	PixelColour border;  ///< Outline of the cards.
	PixelColour grid;    ///< Grid lines of the chart.
	PixelColour area;    ///< Area under the price line.
	PixelColour line;    ///< The price line.
	PixelColour ink;     ///< Dark ink for markers.
};

static GpColours GetGpColours()
{
	GpColours c{
		GetColourGradient(COLOUR_BROWN, SHADE_LIGHTER),
		GetColourGradient(COLOUR_BROWN, SHADE_LIGHTEST),
		GetColourGradient(COLOUR_BROWN, SHADE_LIGHT),
		GetColourGradient(COLOUR_BROWN, SHADE_NORMAL),
		GetColourGradient(COLOUR_BROWN, SHADE_LIGHT),
		GetColourGradient(COLOUR_YELLOW, SHADE_LIGHTER),
		GetColourGradient(COLOUR_BROWN, SHADE_DARKER),
		GetColourGradient(COLOUR_BROWN, SHADE_DARKEST),
	};
	return c;
}

/** The ranges of the chart in days (0 = all the history that is kept) and their captions. */
static constexpr int GP_RANGE_DAYS[4] = {30, 90, 365, 0};
static const char *const GP_RANGE_NAMES[4] = {"30D", "90D", "1Y", "All"};

/** All rectangles of the info area, relative to its top left corner. Used for drawing and for the clicks, so they always agree. */
struct GpLayout {
	Rect icon;      ///< The gold bar icon.
	Rect title;     ///< The title text.
	Rect badge;     ///< The GOLD button: switches between the panel and the price chart.
	Rect news;      ///< The NEWS button: opens the news window (Transport Payrolls).
	Rect spot;      ///< Card: spot price per oz and per ton, buy / sell price.
	Rect table;     ///< Card: the five rows of the gold reserve and the credit standing.
	Rect mine;      ///< Card: the gold and the cash of the company.
	Rect alert;     ///< Room for the alert under the cards.
	Rect chart;     ///< Card of the price chart (chart view).
	Rect ranges[4]; ///< Buttons 30D, 90D, 1Y, All (chart view).
	int row_h = 0;  ///< Height of one row of the table.
	int big_h = 0;  ///< Height of one row with a large price.
};

static GpLayout GpMakeLayout(int w, int h)
{
	GpLayout l;
	const int m = ScaleGUITrad(10);
	const int gap = ScaleGUITrad(8);
	l.row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(9);
	l.big_h = GetCharacterHeight(FS_LARGE) + ScaleGUITrad(12);

	int y = m;
	const int head_h = std::max(ScaleGUITrad(32), GetCharacterHeight(FS_LARGE) + ScaleGUITrad(8));
	l.icon = {m, y, m + head_h, y + head_h};
	const int badge_w = ScaleGUITrad(108);
	l.badge = {w - m - badge_w, y, w - m, y + head_h};
	const int news_w = ScaleGUITrad(84);
	l.news = {l.badge.left - gap - news_w, y, l.badge.left - gap, y + head_h};
	l.title = {l.icon.right + ScaleGUITrad(10), y, l.news.left - gap, y + head_h};
	y += head_h + gap + ScaleGUITrad(2);

	l.spot = {m, y, w - m, y + 2 * l.big_h + l.row_h + ScaleGUITrad(6)};
	y = l.spot.bottom + gap;
	l.table = {m, y, w - m, y + 5 * l.row_h + 2};
	y = l.table.bottom + gap;
	l.mine = {m, y, w - m, y + 2 * l.row_h + 2};
	y = l.mine.bottom + ScaleGUITrad(4);
	l.alert = {m, y, w - m, h - m};

	l.chart = {m, l.spot.top, w - m, h - m};
	const int bw = ScaleGUITrad(46);
	int x = l.chart.right - ScaleGUITrad(8);
	const int by = l.chart.top + ScaleGUITrad(8);
	for (int i = 3; i >= 0; i--) {
		l.ranges[i] = {x - bw, by, x, by + l.row_h};
		x -= bw + ScaleGUITrad(4);
	}
	return l;
}

static Rect GpOffset(const Rect &r, int dx, int dy)
{
	return {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
}

static bool GpHit(const Rect &r, int x, int y)
{
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

static std::string GpMoney(int64_t v)
{
	return GetString(STR_JUST_CURRENCY_LONG, v);
}

/** A card: a light box with an outline. */
static void DrawCard(const Rect &r, PixelColour fill, const GpColours &c)
{
	GfxFillRect(r.left, r.top, r.right, r.bottom, fill);
	DrawRectOutline(r, c.border);
}

/** The gold bar icon: three ingots in a small framed square. */
static void DrawGoldIcon(const Rect &r, const GpColours &c)
{
	DrawCard(r, c.card, c);
	const int s = r.right - r.left + 1;
	const int u = std::max(1, s / 8);
	const PixelColour gold = GetColourGradient(COLOUR_YELLOW, SHADE_NORMAL);
	const PixelColour edge = GetColourGradient(COLOUR_BROWN, SHADE_DARKER);
	auto bar = [&](int x, int y, int bw, int bh) {
		GfxFillRect(x, y, x + bw - 1, y + bh - 1, gold);
		DrawRectOutline(Rect{x, y, x + bw - 1, y + bh - 1}, edge);
	};
	const int cx = r.left + s / 2;
	bar(cx - (3 * u) / 2, r.top + 2 * u, 3 * u, 2 * u + 1);
	bar(cx - 3 * u, r.top + 4 * u + 1, 3 * u, 2 * u + 1);
	bar(cx, r.top + 4 * u + 1, 3 * u, 2 * u + 1);
}

/** The small chart icon of the GOLD button: axes and a rising line. */
static void DrawChartIcon(int left, int top, int w, int h, PixelColour ink)
{
	GfxDrawLine(left, top, left, top + h, ink);
	GfxDrawLine(left, top + h, left + w, top + h, ink);
	const int x1 = left + w / 4, x2 = left + w / 2, x3 = left + (3 * w) / 4;
	GfxDrawLine(left + 1, top + (3 * h) / 4, x1, top + h / 2, ink);
	GfxDrawLine(x1, top + h / 2, x2, top + (2 * h) / 3, ink);
	GfxDrawLine(x2, top + (2 * h) / 3, x3, top + h / 4, ink);
	GfxDrawLine(x3, top + h / 4, left + w, top + h / 8, ink);
}

/** A row of the table: the caption on the left, the value on the right. Every second row is shaded. Rows that do not fit are not drawn. */
static void DrawTableRow(const Rect &card, int index, int row_h, const std::string &caption, const std::string &value, TextColour colour, const GpColours &c)
{
	const int top = card.top + 1 + index * row_h;
	const int bottom = top + row_h - 1;
	if (bottom > card.bottom - 1) return;
	if (index % 2 == 1) GfxFillRect(card.left + 1, top, card.right - 1, bottom, c.row_alt);
	const int pad = ScaleGUITrad(12);
	const int left = card.left + pad;
	const int right = card.right - pad;
	if (right <= left) return;
	const int y = CentreBounds(top, bottom + 1, GetCharacterHeight(FS_NORMAL));
	const int value_w = std::min(static_cast<int>(GetStringBoundingBox(value).width), right - left);
	DrawString(std::max(left, right - value_w), right, y, value, colour, SA_RIGHT);
	const int caption_right = right - value_w - ScaleGUITrad(8);
	if (caption_right > left) DrawString(left, caption_right, y, caption, TC_BLACK, SA_LEFT);
}

/** The credit standing: the letters in a small framed box, green for A and better, orange for B, red for C. */
static void DrawTierRow(const Rect &card, int index, int row_h, const std::string &caption, CreditTier tier, const GpColours &c)
{
	const int top = card.top + 1 + index * row_h;
	const int bottom = top + row_h - 1;
	if (bottom > card.bottom - 1) return;
	if (index % 2 == 1) GfxFillRect(card.left + 1, top, card.right - 1, bottom, c.row_alt);
	const int pad = ScaleGUITrad(12);
	const int left = card.left + pad;
	const int right = card.right - pad;
	if (right <= left) return;
	const Colours colour = tier >= CreditTier::A ? COLOUR_GREEN : (tier == CreditTier::B ? COLOUR_YELLOW : COLOUR_RED);
	const TextColour text_colour = tier >= CreditTier::A ? TC_DARK_GREEN : (tier == CreditTier::B ? TC_ORANGE : TC_RED);
	const std::string name = GetCreditTierName(tier);
	const int hn = GetCharacterHeight(FS_NORMAL);
	const int tw = static_cast<int>(GetStringBoundingBox(name).width);
	const int box_w = tw + ScaleGUITrad(24);
	const Rect box{right - box_w, top + ScaleGUITrad(2), right, bottom - ScaleGUITrad(1)};
	GfxFillRect(box.left, box.top, box.right, box.bottom, GetColourGradient(colour, SHADE_LIGHTEST));
	DrawRectOutline(box, GetColourGradient(colour, SHADE_DARKER));
	DrawString(box.left, box.right, CentreBounds(box.top, box.bottom + 1, hn), name, text_colour, SA_HOR_CENTER);
	const int y = CentreBounds(top, bottom + 1, hn);
	if (box.left - ScaleGUITrad(8) > left) DrawString(left, box.left - ScaleGUITrad(8), y, caption, TC_BLACK, SA_LEFT);
}

/** The header: icon, title and the GOLD button. */
static void DrawHeader(const GpLayout &l, int dx, int dy, bool chart, const GpColours &c)
{
	const Rect icon = GpOffset(l.icon, dx, dy);
	const Rect title = GpOffset(l.title, dx, dy);
	const Rect b = GpOffset(l.badge, dx, dy);
	DrawGoldIcon(icon, c);
	DrawString(title.left, title.right, CentreBounds(title.top, title.bottom + 1, GetCharacterHeight(FS_LARGE)), chart ? GpText("กราฟราคาทองคำ", "GOLD PRICE CHART") : GpText("แผงซื้อขายทองคำ", "GOLD TRADING PANEL"), TC_BLACK, SA_LEFT, false, FS_LARGE);
	FrameFlags flags{};
	if (chart) flags.Set(FrameFlag::Lowered);
	DrawFrameRect(b.left, b.top, b.right, b.bottom, COLOUR_BROWN, flags);
	const int shift = chart ? 1 : 0;
	const int icon_w = ScaleGUITrad(18);
	const int icon_h = ScaleGUITrad(13);
	const int ix = b.left + ScaleGUITrad(12);
	DrawChartIcon(ix + shift, CentreBounds(b.top, b.bottom + 1, icon_h) + shift, icon_w, icon_h, c.ink);
	DrawString(ix + icon_w + ScaleGUITrad(8), b.right - ScaleGUITrad(4), CentreBounds(b.top, b.bottom + 1, GetCharacterHeight(FS_NORMAL)) + shift, GpText("ทอง", "GOLD"), TC_BLACK, SA_LEFT);

	/* The NEWS button: lowered while the news window is open. */
	const Rect n = GpOffset(l.news, dx, dy);
	const bool news_open = IsPayrollNewsWindowOpen();
	FrameFlags news_flags{};
	if (news_open) news_flags.Set(FrameFlag::Lowered);
	DrawFrameRect(n.left, n.top, n.right, n.bottom, COLOUR_BROWN, news_flags);
	DrawString(n.left + 2, n.right - 2, CentreBounds(n.top, n.bottom + 1, GetCharacterHeight(FS_NORMAL)) + (news_open ? 1 : 0), GpText("ข่าว", "NEWS"), TC_BLACK, SA_HOR_CENTER);
}

/** The price chart: last / high / low / change of the chosen range, and the line with the area under it. */
static void DrawGoldChart(const Rect &card, const Rect ranges[4], int days, const GpColours &c)
{
	DrawCard(card, c.card, c);
	const int hn = GetCharacterHeight(FS_NORMAL);
	const int hl = GetCharacterHeight(FS_LARGE);
	const int pad = ScaleGUITrad(12);

	/* The range buttons. */
	for (int i = 0; i < 4; i++) {
		const Rect &b = ranges[i];
		FrameFlags flags{};
		const bool selected = days == GP_RANGE_DAYS[i];
		if (selected) flags.Set(FrameFlag::Lowered);
		DrawFrameRect(b.left, b.top, b.right, b.bottom, COLOUR_BROWN, flags);
		DrawString(b.left + 2, b.right - 2, CentreBounds(b.top, b.bottom + 1, hn) + (selected ? 1 : 0), i == 3 ? GpText("ทั้งหมด", "All") : std::string(GP_RANGE_NAMES[i]), TC_BLACK, SA_HOR_CENTER);
	}

	const std::vector<int64_t> &hist = _gold_reserve.history_cents;
	const size_t total = hist.size();
	const size_t n = (days <= 0 || static_cast<size_t>(days) > total) ? total : static_cast<size_t>(days);
	if (n == 0) return;
	const size_t first = total - n;

	int64_t lo = hist[first], hi = hist[first];
	for (size_t i = first; i < total; i++) {
		lo = std::min(lo, hist[i]);
		hi = std::max(hi, hist[i]);
	}
	const int64_t last = hist[total - 1];
	const int64_t start = hist[first];
	const int64_t change_bp = start > 0 ? (last - start) * 10000 / start : 0;
	const int64_t abs_bp = change_bp < 0 ? -change_bp : change_bp;

	/* The four figures below the buttons. */
	const int stats_top = ranges[0].bottom + ScaleGUITrad(8);
	const int col_w = (card.right - card.left + 1 - 2 * pad) / 4;
	struct Stat { std::string caption; std::string value; TextColour colour; };
	const Stat stats[4] = {
		{GpText("ล่าสุด", "Last"), GpMoney(last / 100), TC_BROWN},
		{GpText("สูงสุด", "High"), GpMoney(hi / 100), TC_BLACK},
		{GpText("ต่ำสุด", "Low"), GpMoney(lo / 100), TC_BLACK},
		{GpText("เปลี่ยนแปลง", "Change"), fmt::format("{}{}.{:02}%", change_bp < 0 ? "-" : "+", abs_bp / 100, abs_bp % 100), change_bp > 0 ? TC_DARK_GREEN : (change_bp < 0 ? TC_RED : TC_BLACK)},
	};
	for (int i = 0; i < 4; i++) {
		const int x = card.left + pad + i * col_w;
		if (stats_top + hn + hl + 2 > card.bottom) break;
		DrawString(x, x + col_w - ScaleGUITrad(4), stats_top, stats[i].caption, TC_BLACK, SA_LEFT);
		DrawString(x, x + col_w - ScaleGUITrad(4), stats_top + hn + 2, stats[i].value, stats[i].colour, SA_LEFT, false, FS_LARGE);
	}

	/* The plot: a little room above and below the extremes. */
	const int64_t span = hi - lo;
	const int64_t margin = std::max<int64_t>(span / 10, lo / 200 + 1);
	const int64_t y_lo = std::max<int64_t>(0, lo - margin);
	const int64_t y_hi = hi + margin;
	const int64_t y_range = std::max<int64_t>(1, y_hi - y_lo);

	const std::string top_label = GpMoney(y_hi / 100);
	const std::string bottom_label = GpMoney(y_lo / 100);
	const int label_w = std::max(static_cast<int>(GetStringBoundingBox(top_label).width), static_cast<int>(GetStringBoundingBox(bottom_label).width)) + ScaleGUITrad(8);
	const Rect plot{card.left + pad + label_w, stats_top + hn + hl + ScaleGUITrad(16), card.right - pad, card.bottom - pad - hn - ScaleGUITrad(6)};
	const int pw = plot.right - plot.left + 1;
	const int ph = plot.bottom - plot.top + 1;
	if (pw < 40 || ph < 30) return;

	/* Grid and the scale on the left. */
	for (int i = 0; i <= 4; i++) {
		const int gy = plot.top + i * (ph - 1) / 4;
		GfxDrawLine(plot.left, gy, plot.right, gy, c.grid);
		const int64_t value = y_hi - y_range * i / 4;
		DrawString(card.left + pad, plot.left - ScaleGUITrad(6), gy - hn / 2, GpMoney(value / 100), TC_BLACK, SA_RIGHT);
	}
	GfxDrawLine(plot.left, plot.top, plot.left, plot.bottom, c.border);
	GfxDrawLine(plot.left, plot.bottom, plot.right, plot.bottom, c.border);

	if (n < 2) {
		DrawString(plot.left, plot.right, CentreBounds(plot.top, plot.bottom + 1, hn), GpText("กำลังเก็บข้อมูลราคา: เพิ่มหนึ่งจุดทุกวันในเกม", "Collecting price data: a point is added every game day"), TC_BLACK, SA_HOR_CENTER);
		return;
	}

	/* One point per pixel column, with the price between two days worked out in a straight line. */
	std::vector<int> ys(static_cast<size_t>(pw));
	for (int i = 0; i < pw; i++) {
		const int64_t num = static_cast<int64_t>(i) * static_cast<int64_t>(n - 1);
		const size_t idx = static_cast<size_t>(num / (pw - 1));
		const int64_t frac = num % (pw - 1);
		const int64_t a = hist[first + idx];
		const int64_t b = hist[first + std::min(idx + 1, n - 1)];
		const int64_t v = a + (b - a) * frac / (pw - 1);
		ys[static_cast<size_t>(i)] = plot.bottom - static_cast<int>((v - y_lo) * (ph - 1) / y_range);
	}
	for (int i = 0; i < pw; i++) GfxFillRect(plot.left + i, ys[static_cast<size_t>(i)], plot.left + i, plot.bottom - 1, c.area);
	for (int i = 1; i < pw; i++) {
		const int y0 = ys[static_cast<size_t>(i - 1)];
		const int y1 = ys[static_cast<size_t>(i)];
		GfxDrawLine(plot.left + i - 1, y0, plot.left + i, y1, c.line);
		GfxDrawLine(plot.left + i - 1, y0 + 1, plot.left + i, y1 + 1, c.line);
	}
	const int ex = plot.right - 1;
	const int ey = ys[static_cast<size_t>(pw - 1)];
	GfxFillRect(ex - 3, ey - 3, ex + 3, ey + 3, c.ink);
	GfxFillRect(ex - 2, ey - 2, ex + 2, ey + 2, GetColourGradient(COLOUR_YELLOW, SHADE_NORMAL));

	/* The time axis. */
	const int ay = plot.bottom + ScaleGUITrad(4);
	DrawString(plot.left, plot.right, ay, fmt::format("{} {}", n - 1, GpText("วันที่แล้ว", "days ago")), TC_BLACK, SA_LEFT);
	DrawString(plot.left, plot.right, ay, GpText("ตอนนี้", "Now"), TC_BLACK, SA_RIGHT);
}

/** The info area: the trading panel (cards) or the price chart. */
void GoldPanel::DrawInfo(const Rect &r) const
{
	const GpColours c = GetGpColours();
	GfxFillRect(r.left, r.top, r.right, r.bottom, c.paper);
	if (r.right <= r.left || r.bottom <= r.top) return;
	const GpLayout l = GpMakeLayout(r.right - r.left + 1, r.bottom - r.top + 1);
	const int dx = r.left;
	const int dy = r.top;
	auto at = [&](const Rect &x) { return GpOffset(x, dx, dy); };

	DrawHeader(l, dx, dy, this->show_chart, c);

	if (_gold_reserve.GetPriceCents() <= 0) {
		const Rect card = at(l.spot);
		DrawStringMultiLine(card.left, card.right, card.top, r.bottom - ScaleGUITrad(4), GpText("ตลาดทองคำยังไม่เปิด", "The gold market has not started yet."), TC_BLACK, SA_TOP | SA_LEFT);
		return;
	}

	if (this->show_chart) {
		const Rect ranges[4] = {at(l.ranges[0]), at(l.ranges[1]), at(l.ranges[2]), at(l.ranges[3])};
		DrawGoldChart(at(l.chart), ranges, this->chart_days, c);
		return;
	}

	const int64_t price = _gold_reserve.GetPriceCents();
	const int64_t backing = _gold_reserve.GetBackingBp();
	const CreditTier tier = _gold_reserve.GetCreditTier();
	auto ounces = [](int64_t v) { return fmt::format("{} oz", v); };

	/* Card 1: the spot prices (large) and the buy / sell price. */
	{
		const Rect card = at(l.spot);
		DrawCard(card, c.card, c);
		const int pad = ScaleGUITrad(12);
		const int left = card.left + pad;
		const int right = card.right - pad;
		const int hn = GetCharacterHeight(FS_NORMAL);
		const int hl = GetCharacterHeight(FS_LARGE);
		auto big_row = [&](int top, const std::string &caption, const std::string &value) {
			const int value_w = std::min(static_cast<int>(GetStringBoundingBox(value, FS_LARGE).width), right - left);
			DrawString(std::max(left, right - value_w), right, CentreBounds(top, top + l.big_h, hl), value, TC_BROWN, SA_RIGHT, false, FS_LARGE);
			const int caption_right = right - value_w - ScaleGUITrad(8);
			if (caption_right > left) DrawString(left, caption_right, CentreBounds(top, top + l.big_h, hn), caption, TC_BLACK, SA_LEFT);
		};
		const int top = card.top + ScaleGUITrad(2);
		big_row(top, GpText("ราคาทองต่อออนซ์", "Spot price per oz"), GpMoney(price / 100));
		big_row(top + l.big_h, GpText("ราคาทองต่อตัน", "Spot price per ton"), GpMoney(price * GoldReserveManager::OUNCES_PER_TON / 100));
		const int sub = top + 2 * l.big_h;
		const std::string quote = GpMoney(_gold_reserve.GetAskCents() / 100) + " / " + GpMoney(_gold_reserve.GetBidCents() / 100);
		const int quote_w = std::min(static_cast<int>(GetStringBoundingBox(quote).width), right - left);
		DrawString(std::max(left, right - quote_w), right, CentreBounds(sub, sub + l.row_h, hn), quote, TC_BLACK, SA_RIGHT);
		const int caption_right = right - quote_w - ScaleGUITrad(8);
		if (caption_right > left) DrawString(left, caption_right, CentreBounds(sub, sub + l.row_h, hn), GpText("ราคาซื้อ / ขายต่อออนซ์", "Buy / sell price per oz"), TC_BLACK, SA_LEFT);
	}

	/* Card 2: the gold reserve of the money system. */
	{
		const Rect card = at(l.table);
		DrawCard(card, c.paper, c);
		DrawTableRow(card, 0, l.row_h, GpText("ทองสำรองทั้งหมด", "Gold reserves (all)"), GpMoney(_gold_reserve.GetReserveValue()) + " (" + ounces(_gold_reserve.GetTotalReserveOunces()) + ")", TC_BLACK, c);
		DrawTableRow(card, 1, l.row_h, GpText("ทองในตลาดที่ขาย", "Market stock for sale"), ounces(_gold_reserve.market_stock_oz), TC_BLACK, c);
		DrawTableRow(card, 2, l.row_h, GpText("เงินสดหมุนเวียน", "Circulating cash"), GpMoney(_gold_reserve.GetCirculatingCash()), TC_BLACK, c);
		DrawTableRow(card, 3, l.row_h, GpText("อัตราหนุนหลัง (ทองสำรอง / เงินสด)", "Backing ratio (reserves / cash)"), fmt::format("{}.{:02}%", backing / 100, backing % 100), TC_BLACK, c);
		DrawTierRow(card, 4, l.row_h, GpText("ระดับเครดิต", "Credit standing"), tier, c);
	}

	if (!Company::IsValidID(_local_company)) return;

	/* Card 3: the gold and the cash of the company. */
	{
		const Rect card = at(l.mine);
		DrawCard(card, c.paper, c);
		const int64_t held = _gold_reserve.GetHoldings(_local_company);
		DrawTableRow(card, 0, l.row_h, GpText("ทองในคลังของคุณ", "Your gold reserve"), ounces(held) + " (" + GpMoney(_gold_reserve.GetSellNet(held)) + ")", TC_BLACK, c);
		DrawTableRow(card, 1, l.row_h, GpText("เงินสดคล่องตัวที่ไม่ได้กู้", "Unborrowed liquid cash"), GpMoney(GetUnborrowedLiquidCash(_local_company)), TC_BLACK, c);
	}

	/* The alert: why Buy Gold is switched off. */
	std::string alert;
	switch (GetGoldBuyBlock(this->buy_qty)) {
		case GoldBuyBlock::Borrowed: alert = GpText("เตือน: เงินสำหรับคำสั่งนี้เป็นเงินกู้จากธนาคารกลาง ซื้อทองได้เฉพาะด้วยเงินสดที่ไม่ได้กู้", "Alert: the cash for this order is borrowed from the central bank. Gold can only be bought with unborrowed liquid cash."); break;
		case GoldBuyBlock::NoStock:  alert = GpText("เตือน: ตลาดมีทองขายไม่พอ", "Alert: the market does not have that much gold for sale."); break;
		case GoldBuyBlock::NoCash:   alert = GpText("เตือน: เงินสดที่ไม่ได้กู้ไม่พอสำหรับคำสั่งนี้", "Alert: not enough unborrowed liquid cash for this order."); break;
		default: break;
	}
	if (!alert.empty()) {
		const Rect a = at(l.alert);
		if (a.bottom - a.top >= GetCharacterHeight(FS_NORMAL)) DrawStringMultiLine(a.left, a.right, a.top, a.bottom, alert, TC_RED, SA_TOP | SA_LEFT);
	}
}

/** The GOLD button, the NEWS button and the range buttons of the chart. @return true when the click was on one of them (the area has to be redrawn). */
bool GoldPanel::ClickInfo(const Rect &panel, Point pt)
{
	const GpLayout l = GpMakeLayout(panel.right - panel.left + 1, panel.bottom - panel.top + 1);
	const int x = pt.x - panel.left;
	const int y = pt.y - panel.top;
	if (GpHit(l.badge, x, y)) {
		this->show_chart = !this->show_chart;
		return true;
	}
	if (GpHit(l.news, x, y)) {
		if (IsPayrollNewsWindowOpen()) CloseWindowById(WC_CENTRAL_BANK, 5); else ShowPayrollNewsWindow();
		return true;
	}
	if (this->show_chart) {
		for (int i = 0; i < 4; i++) {
			if (GpHit(l.ranges[i], x, y)) {
				this->chart_days = GP_RANGE_DAYS[i];
				return true;
			}
		}
	}
	return false;
}

/** The quantity field under the panel: the ounces, and what they cost (buy) or bring (sell). Click to type another number. */
void GoldPanel::DrawQuantity(const Rect &r, bool buy) const
{
	const GpColours c = GetGpColours();
	GfxFillRect(r.left, r.top, r.right, r.bottom, c.paper);
	const int left = r.left + ScaleGUITrad(10);
	const int right = r.right - ScaleGUITrad(4);
	if (right <= left) return;
	const int64_t qty = buy ? this->buy_qty : this->sell_qty;
	std::string text = fmt::format("{}: {} oz", buy ? GpText("ซื้อ", "Buy") : GpText("ขาย", "Sell"), qty);
	if (_gold_reserve.GetPriceCents() > 0) {
		const int64_t value = buy ? _gold_reserve.GetBuyTotal(qty) : _gold_reserve.GetSellNet(qty);
		text += fmt::format(" ~ {}", GpMoney(value));
	}
	DrawString(left, right, CentreBounds(r.top, r.bottom + 1, GetCharacterHeight(FS_NORMAL)), text, TC_BLACK, SA_LEFT);
}

/** The thin vertical line between the Buy and the Sell side. */
void GoldPanel::DrawDivider(const Rect &r) const
{
	const GpColours c = GetGpColours();
	GfxFillRect(r.left, r.top, r.right, r.bottom, c.paper);
	const int x = (r.left + r.right) / 2;
	GfxFillRect(x, r.top + ScaleGUITrad(6), x, r.bottom - ScaleGUITrad(6), c.border);
}
