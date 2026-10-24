/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gold_panel.cpp The gold trading panel: spot price, backing, credit standing, holdings, and the Buy / Sell actions. */

#include "stdafx.h"
#include "3rdparty/fmt/format.h"
#include "gold_panel.h"
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
#include "zoom_func.h"

#include "table/strings.h"

#include "safeguards.h"

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

/** One line of the panel: the caption on the left, the value on the right. Everything stays inside [left, right] x [.., bottom). */
static void DrawGoldLine(int left, int right, int &y, int bottom, int row_h, const std::string &caption, const std::string &value, TextColour colour = TC_WHITE)
{
	if (right <= left || y + row_h > bottom) return; // A line that does not fit completely is not drawn at all.
	const int value_w = std::min(GetStringBoundingBox(value).width, static_cast<uint>(std::max(0, right - left)));
	DrawString(std::max(left, right - value_w), right, y, value, colour, SA_RIGHT);
	const int caption_right = right - value_w - ScaleGUITrad(8);
	if (caption_right > left) DrawString(left, caption_right, y, caption, TC_SILVER, SA_LEFT);
	y += row_h;
}

/** The panel: spot price, backing, credit standing, holdings, and the alert when the gold can not be bought. */
void GoldPanel::DrawInfo(const Rect &r) const
{
	const int pad = ScaleGUITrad(6);
	const int left = r.left + pad;
	const int right = r.right - pad;
	const int bottom = r.bottom - ScaleGUITrad(2);
	const int row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
	int y = r.top + ScaleGUITrad(4);
	if (right <= left) return;

	auto money = [](int64_t v) { return GetString(STR_JUST_CURRENCY_LONG, v); };
	auto ounces = [](int64_t v) { return fmt::format("{} oz", v); };

	if (_gold_reserve.GetPriceCents() <= 0) {
		DrawStringMultiLine(left, right, y, bottom, std::string("The gold market has not started yet."), TC_GREY, SA_TOP | SA_LEFT);
		return;
	}
	const int64_t price = _gold_reserve.GetPriceCents();
	const int64_t backing = _gold_reserve.GetBackingBp();
	const CreditTier tier = _gold_reserve.GetCreditTier();

	DrawGoldLine(left, right, y, bottom, row_h, "GOLD TRADING PANEL", "", TC_LIGHT_BLUE);
	DrawGoldLine(left, right, y, bottom, row_h, "Spot price per oz", money(price / 100), TC_GOLD);
	DrawGoldLine(left, right, y, bottom, row_h, "Buy / sell price per oz", money(_gold_reserve.GetAskCents() / 100) + " / " + money(_gold_reserve.GetBidCents() / 100));
	DrawGoldLine(left, right, y, bottom, row_h, "Spot price per ton", money(price * GoldReserveManager::OUNCES_PER_TON / 100), TC_GOLD);
	DrawGoldLine(left, right, y, bottom, row_h, "Gold reserves (all)", money(_gold_reserve.GetReserveValue()) + "  (" + ounces(_gold_reserve.GetTotalReserveOunces()) + ")");
	DrawGoldLine(left, right, y, bottom, row_h, "Market stock for sale", ounces(_gold_reserve.market_stock_oz));
	DrawGoldLine(left, right, y, bottom, row_h, "Circulating cash", money(_gold_reserve.GetCirculatingCash()));
	DrawGoldLine(left, right, y, bottom, row_h, "Backing ratio (reserves / cash)", fmt::format("{}.{:02}%", backing / 100, backing % 100));
	DrawGoldLine(left, right, y, bottom, row_h, "Credit standing", std::string(GetCreditTierName(tier)),
			tier >= CreditTier::A ? TC_GREEN : (tier == CreditTier::B ? TC_YELLOW : TC_RED));

	if (!Company::IsValidID(_local_company)) return;
	y += ScaleGUITrad(4);
	const int64_t held = _gold_reserve.GetHoldings(_local_company);
	DrawGoldLine(left, right, y, bottom, row_h, "Your gold reserve", ounces(held) + "  (" + money(_gold_reserve.GetSellNet(held)) + ")");
	DrawGoldLine(left, right, y, bottom, row_h, "Unborrowed liquid cash", money(GetUnborrowedLiquidCash(_local_company)));

	/* The alert: why Buy Gold is switched off. */
	const char *alert = nullptr;
	switch (GetGoldBuyBlock(this->buy_qty)) {
		case GoldBuyBlock::Borrowed: alert = "Alert: the cash for this order is borrowed from the central bank. Gold can only be bought with unborrowed liquid cash."; break;
		case GoldBuyBlock::NoStock:  alert = "Alert: the market does not have that much gold for sale."; break;
		case GoldBuyBlock::NoCash:   alert = "Alert: not enough unborrowed liquid cash for this order."; break;
		default: break;
	}
	if (alert != nullptr && y < bottom) DrawStringMultiLine(left, right, y + ScaleGUITrad(2), bottom, std::string(alert), TC_RED, SA_TOP | SA_LEFT);
}

/** The quantity field: the ounces, and what they cost (buy) or bring (sell). */
void GoldPanel::DrawQuantity(const Rect &r, bool buy) const
{
	const int left = r.left + ScaleGUITrad(4);
	const int right = r.right - ScaleGUITrad(4);
	if (right <= left) return;
	const int64_t qty = buy ? this->buy_qty : this->sell_qty;
	std::string text = fmt::format("{}: {} oz", buy ? "Buy" : "Sell", qty);
	if (_gold_reserve.GetPriceCents() > 0) {
		const int64_t value = buy ? _gold_reserve.GetBuyTotal(qty) : _gold_reserve.GetSellNet(qty);
		text += fmt::format("  ~ {}", GetString(STR_JUST_CURRENCY_LONG, value));
	}
	DrawString(left, right, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), text, TC_BLACK, SA_LEFT);
}
