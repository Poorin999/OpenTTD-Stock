/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file market_news_gui.h The news panel: the lead story written by the news writer, and the list of the latest events. It takes the place of the profit graph in the gold window, and it is the window of the menu entry Market News (the old income graph). */

#ifndef MARKET_NEWS_GUI_H
#define MARKET_NEWS_GUI_H

#include <cstdint>
#include "core/geometry_type.hpp"

/** State of the news panel (which story is open). One per window. */
struct MarketNewsPanel {
	uint32_t selected_id = 0; ///< Id of the story that is open; 0 or an id that is gone = the newest.

	/** Draw the panel in a rectangle. */
	void Draw(const Rect &r) const;
	/** A click in the panel (pt in window coordinates). @return True when the open story changed (redraw). */
	bool Click(const Rect &r, Point pt);
};

/** Open (or bring to the front) the market news window: it is the menu entry that used to be the income graph. */
void ShowMarketNewsWindow();
/** Whether the market news window is open. */
bool IsMarketNewsWindowOpen();

#endif /* MARKET_NEWS_GUI_H */
