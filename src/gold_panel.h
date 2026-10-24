/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gold_panel.h The gold trading panel: drawing and the actions of its buttons. Used by the operating profit graph window. */

#ifndef GOLD_PANEL_H
#define GOLD_PANEL_H

#include "window_gui.h"
#include <cstdint>
#include <optional>
#include <string>

/** State and actions of one gold trading panel. The window that shows it owns one, and calls these from its draw and click handlers. */
struct GoldPanel {
	int64_t buy_qty = 10;   ///< Ounces in the buy field.
	int64_t sell_qty = 10;  ///< Ounces in the sell field.
	bool query_buy = true;  ///< Whether the open query window is for the buy field.
	bool show_chart = false; ///< Whether the info area shows the gold price chart (true) or the trading panel (false).
	int chart_days = 90;    ///< Days of price history in the chart; 0 = all that is kept.

	bool CanBuy() const;
	bool CanSell() const;
	bool CanOpenBuyWindow() const;
	void Buy() const;
	void Sell() const;
	void AskQuantity(Window *parent, bool buy);
	void QueryFinished(const std::optional<std::string> &text);
	void DrawInfo(const Rect &r) const;
	void DrawQuantity(const Rect &r, bool buy) const;
	void DrawDivider(const Rect &r) const;
	bool ClickInfo(const Rect &panel, Point pt);
};

/** Open the Buy gold window of the company vault (gold_buy_gui.cpp). start_qty is the number of ounces in its field. */
void ShowGoldBuyWindow(int64_t start_qty);

#endif /* GOLD_PANEL_H */
