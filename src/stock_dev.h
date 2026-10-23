/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_dev.h Test tools of the mod: the switch that is set by the console command and the time skip. Not for release builds. */

#ifndef STOCK_DEV_H
#define STOCK_DEV_H

#include <cstdint>

/** True while the test tools are on ('stock_dev on' in the console): the time skip buttons of the Central Bank window are shown and 'stock_skip' works. Off at every start. */
extern bool _stock_dev_tools;

void SkipGameMinutes(uint64_t minutes);

#endif /* STOCK_DEV_H */
