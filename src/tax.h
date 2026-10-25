/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file tax.h The taxes of the companies. Every tax is paid to the Central Bank (it is income of the own money of the bank,
 * CentralBank::trading_revenue_cents) and is a line of the account statement (LedgerItem::Tax*).
 *
 *  - Land tax:       every game day, per tile that the company owns (stations, rail, road, bridges, water, objects, bought land).
 *                    A station tile counts 4, an object 3, other tiles 1; a tile near a town is worth more (up to +500% for a big town).
 *  - Transport tax:  every game day, per vehicle that is not stopped in a depot (train, road vehicle, ship, aircraft each have a rate).
 *  - VAT:            7% of every income from cargo delivery, taken when the income is paid.
 *  - Capital gains:  15% of every profit of a closed trade and of a gold sale, taken when the profit is paid.
 *  - Dividend tax:   10% of dividends that a company receives.
 *  - Income tax:     15% of the profit of the quarter (income - expenses of the quarter that has just closed), every 90 game days.
 * The fixed amounts (land, transport) are multiplied by the price index of the game, so they follow inflation.
 * All maths is integer; everything runs from the clock of the mod or from game events, so it is the same on every client.
 */

#ifndef TAX_H
#define TAX_H

#include "stdafx.h"
#include "company_type.h"
#include "central_bank.h"

/* Rates. Change here and build again. Money amounts are in the base currency at price index 1.0. */
static constexpr int64_t TAX_LAND_PER_UNIT_DAY = 6;       ///< Land tax per tile weight 1 (100 units) per game day.
static constexpr int64_t TAX_TRAIN_DAY = 150;             ///< Transport tax per train (a whole consist) per game day.
static constexpr int64_t TAX_ROADVEH_DAY = 50;            ///< Transport tax per road vehicle per game day.
static constexpr int64_t TAX_SHIP_DAY = 100;              ///< Transport tax per ship per game day.
static constexpr int64_t TAX_AIRCRAFT_DAY = 200;          ///< Transport tax per aircraft per game day.
static constexpr int64_t TAX_VAT_PERCENT = 7;             ///< VAT on the income of cargo delivery.
static constexpr int64_t TAX_CAPITAL_GAINS_PERCENT = 15;  ///< Tax on the profit of closed trades and gold sales.
static constexpr int64_t TAX_DIVIDEND_PERCENT = 10;       ///< Tax on dividends received.
static constexpr int64_t TAX_INCOME_QUARTER_PERCENT = 15; ///< Tax on the profit of a quarter.

/** VAT on an income of cargo delivery: taken from the company cash now. */
void ChargeVat(CompanyID company, int64_t income);

/**
 * Tax on a profit that is about to be paid to a wallet (closed trade, dividend). It is booked in the ledger and goes to the bank,
 * but the cash is NOT touched: the caller pays the profit minus the returned tax.
 * @param item LedgerItem::RealizedProfit or LedgerItem::Dividends; other items have no tax.
 * @return The tax (0 for a loss or an item without tax).
 */
int64_t TakeInvestorTax(CompanyID company, LedgerItem item, int64_t profit);

#endif /* TAX_H */
