/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file game_calendar.cpp The weekdays of the game calendar and the clock panel of the status bar. */

#include "stdafx.h"
#include "3rdparty/fmt/format.h"
#include "game_calendar.h"
#include "game_time/game_time_manager.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "zoom_func.h"
#include <algorithm>

#include "safeguards.h"

/** True: the weekday names are Thai ("วันจันทร์"). False: English ("Mon"). Change it here and build again (like USE_THAI in central_bank_gui.cpp). */
static constexpr bool GAME_CALENDAR_USE_THAI = true;

GameWeekday GetCurrentGameWeekday()
{
	return GetGameWeekday(TimerGameCalendar::date);
}

std::string GetGameWeekdayName(GameWeekday day)
{
	static constexpr const char *THAI[GAME_DAYS_PER_WEEK] = {
		"วันจันทร์", "วันอังคาร", "วันพุธ", "วันพฤหัสบดี", "วันศุกร์", "วันเสาร์", "วันอาทิตย์",
	};
	static constexpr const char *ENGLISH[GAME_DAYS_PER_WEEK] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
	const size_t index = std::min<size_t>(static_cast<size_t>(day), GAME_DAYS_PER_WEEK - 1);
	return GAME_CALENDAR_USE_THAI ? THAI[index] : ENGLISH[index];
}

/** The width of the badge: the widest weekday name and some space at both sides, so the panel does not jump from day to day. */
static int GameClockBadgeWidth()
{
	int widest = 0;
	for (int i = 0; i < GAME_DAYS_PER_WEEK; i++) {
		widest = std::max<int>(widest, GetStringBoundingBox(GetGameWeekdayName(static_cast<GameWeekday>(i))).width);
	}
	return widest + 2 * ScaleGUITrad(5);
}

int GameClockExtraWidth()
{
	const int time_width = GetStringBoundingBox(std::string("00:00")).width;
	return GameClockBadgeWidth() + time_width + 2 * ScaleGUITrad(8);
}

void DrawGameClock(const Rect &r, const std::string &date_text)
{
	const GameTimeManager &clock = GameTimeManager::Get();
	const GameWeekday day = GetCurrentGameWeekday();
	const std::string time_text = fmt::format("{:02}:{:02}", clock.GetCurrentGameHour(), clock.GetCurrentGameMinute());

	const int gap = ScaleGUITrad(8);
	const int badge_width = GameClockBadgeWidth();
	const int time_width = GetStringBoundingBox(std::string("00:00")).width;
	const int date_width = GetStringBoundingBox(date_text).width;
	const int total = badge_width + gap + time_width + gap + date_width;

	const int panel_width = r.right - r.left + 1;
	const int x = r.left + std::max(0, (panel_width - total) / 2);
	const int text_height = GetCharacterHeight(FS_NORMAL);

	/* The weekday badge: dark blue on a working day, orange on Saturday, red on Sunday. */
	PixelColour badge_colour = GetColourGradient(COLOUR_DARK_BLUE, SHADE_NORMAL);
	TextColour badge_text = TC_WHITE;
	if (day == GameWeekday::Saturday) {
		badge_colour = PC_ORANGE;
		badge_text = TC_BLACK;
	} else if (day == GameWeekday::Sunday) {
		badge_colour = PC_RED;
	}
	GfxFillRect(x, r.top - ScaleGUITrad(1), x + badge_width - 1, r.top + text_height, badge_colour);
	DrawString(x, x + badge_width - 1, r.top, GetGameWeekdayName(day), badge_text, SA_HOR_CENTER);

	const int time_x = x + badge_width + gap;
	DrawString(time_x, r.right, r.top, time_text, TC_WHITE, SA_LEFT);
	DrawString(time_x + time_width + gap, r.right, r.top, date_text, TC_WHITE, SA_LEFT);
}
