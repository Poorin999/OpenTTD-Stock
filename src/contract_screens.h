/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file contract_screens.h The two contract windows of the design: Negotiate Contract and My Contracts (contract_screens.cpp). */

#ifndef CONTRACT_SCREENS_H
#define CONTRACT_SCREENS_H

#include <cstdint>

/** Open (or bring to the front) the My Contracts window: the table of your offers and contracts. */
void ShowMyContractsWindow();

/** Open (or bring to the front) the Negotiate Contract window.
 * @param industry Pool index of the industry to talk about, 0xFFFF = the first industry that can be offered to (or the open contract of the company).
 * @param qty Terms to put on the form (units a month); 0 = the usual start terms (a new offer) or the terms on the table (an open contract).
 * @param price_cents Price of a unit in 1/100 of a money unit (used when qty > 0).
 * @param months Length in months (used when qty > 0). */
void ShowNegotiateWindow(uint16_t industry = 0xFFFF, int64_t qty = 0, int64_t price_cents = 0, int64_t months = 0);

/** True when the Negotiate Contract window is open. */
bool IsNegotiateWindowOpen();

#endif /* CONTRACT_SCREENS_H */
