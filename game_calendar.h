/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file game_calendar.h The weekdays (Monday .. Sunday) of the game calendar and the clock panel of the status bar. */

#ifndef GAME_CALENDAR_H
#define GAME_CALENDAR_H

#include "core/geometry_type.hpp"
#include "timer/timer_game_calendar.h"
#include <cstdint>
#include <string>

/*
 * ---- Weekdays ---------------------------------------------------------------------------------------------------------------
 * The week has 7 days, Monday first. The weekday is worked out from the calendar date, so it always belongs to the date that the status bar
 * shows (the calendar moves exactly at 00:00 of the game clock, see GAME_TIME_SYNC_CALENDAR). It follows the real Gregorian calendar:
 * day 0 of OpenTTD (1 January of year 0) is a Saturday, so 1 January 1950 is a Sunday and 12 April 1950 is a Wednesday.
 * Integer only and a pure function of the date, so every client of a multiplayer game shows the same day.
 */
static constexpr int GAME_DAYS_PER_WEEK = 7;                ///< Days in a week.
static constexpr int GAME_WEEKDAY_OF_DAY_ZERO = 5;          ///< Day 0 of the calendar is a Saturday: its number counted from Monday = 0.

enum class GameWeekday : uint8_t {
	Monday,
	Tuesday,
	Wednesday,
	Thursday,
	Friday,
	Saturday,
	Sunday,
};

/** The weekday of a calendar date. */
inline GameWeekday GetGameWeekday(TimerGameCalendar::Date date)
{
	const int64_t days = static_cast<int64_t>(date.base()) + GAME_WEEKDAY_OF_DAY_ZERO;
	return static_cast<GameWeekday>(((days % GAME_DAYS_PER_WEEK) + GAME_DAYS_PER_WEEK) % GAME_DAYS_PER_WEEK);
}

/** True for Saturday and Sunday. */
inline bool IsGameWeekend(GameWeekday day) { return day == GameWeekday::Saturday || day == GameWeekday::Sunday; }

/** The weekday of today in the game. */
GameWeekday GetCurrentGameWeekday();

/** The name of a weekday, in Thai ("วันจันทร์") or in English ("Mon"), see GAME_CALENDAR_USE_THAI in game_calendar.cpp. */
std::string GetGameWeekdayName(GameWeekday day);

/** The width in pixels that the weekday badge, HH:MM and the gaps need in front of the date in the status bar. */
int GameClockExtraWidth();

/**
 * Draw the clock panel of the status bar, centred in a rectangle:  [ weekday ]  HH:MM  date.
 * The weekday is in a badge: dark blue Monday to Friday, orange on Saturday, red on Sunday.
 * @param r The rectangle of the text of the panel.
 * @param date_text The date as text (for example "12 เมษายน 1950").
 */
void DrawGameClock(const Rect &r, const std::string &date_text);

#endif /* GAME_CALENDAR_H */
