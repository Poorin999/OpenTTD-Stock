/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_finance_gui.h The finance window of one town: its treasury and the four groups of citizens (see town_treasury.h). */

#ifndef TOWN_FINANCE_GUI_H
#define TOWN_FINANCE_GUI_H

#include <cstdint>

/** Open (or bring to the front) the finance window of a town. @param town The index of the town (the window number of the town window). */
void ShowTownFinanceWindow(uint32_t town);

#endif /* TOWN_FINANCE_GUI_H */
