/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file game_time_driver.cpp Connects the clock of the mod to the game ticks. */

#include "../stdafx.h"
#include "game_time_manager.h"
#include "../gfx_type.h"
#include "../timer/timer.h"
#include "../timer/timer_game_tick.h"

#include "../safeguards.h"

/**
 * Every game tick adds MILLISECONDS_PER_TICK milliseconds to the clock. The game ticks are the same on all clients of a
 * network game, they stop while the game is paused and they run faster while the game is fast-forwarded. The wall clock
 * does none of this, and it would make the clients disagree about the time, so it is not used.
 * One game minute is 1000 ms, so at normal speed it takes about 37 ticks.
 */
static const IntervalTimer<TimerGameTick> _game_time_tick({TimerGameTick::Priority::NONE, 1}, [](auto count)
{
	GameTimeManager::Get().AdvanceMilliseconds(static_cast<uint64_t>(count) * MILLISECONDS_PER_TICK);
});
