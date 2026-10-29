/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargo_market_gui.h The cargo prices window: what each cargo pays now, and which towns pay most and least. Opened by the menu entry Graphs > Cargo Prices (the old cargo payment rates graph, see patch_cargo_rates_to_prices) and by a button of the market news window. */

#ifndef CARGO_MARKET_GUI_H
#define CARGO_MARKET_GUI_H

/** Open (or bring to the front) the cargo prices window. */
void ShowCargoPricesWindow();

/** Whether the cargo prices window is open. */
bool IsCargoPricesWindowOpen();

#endif /* CARGO_MARKET_GUI_H */
