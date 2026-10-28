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
 *  - Fuel cost:      not a tax: every game day, per vehicle that runs, base cost x the part of the fuel index above 1.0 (news FuelSpike / FuelDrop, see
 *                    CargoMarketMath.h). Booked as LedgerItem::FuelCost, NOT paid to the central bank. 0 while fuel is at or below its plain price.
 *  - VAT:            7% of every income from cargo delivery, taken when the income is paid.
 *  - Capital gains:  15% of every profit of a closed trade and of a gold sale, taken when the profit is paid.
 *  - Dividend tax:   10% of dividends that a company receives.
 *  - Income tax:     progressive tax on the profit of the quarter (income - expenses of the quarter that has just closed), every 90 game days:
 *                    15% up to TAX_INCOME_BRACKET1, 28% up to TAX_INCOME_BRACKET2, 40% above (only the part inside each step is taxed that way).
 *                    The steps are multiplied by the price index, so inflation does not push a company into a higher step by itself.
 *  - IORB tax:       BANK_IORB_TAX_PERCENT (central_bank.h) of the interest that the bank pays on idle cash, withheld at the source in CentralBank::PayIorb
 *                    (LedgerItem::TaxInterest). The withheld part is never created as money.
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
static constexpr int64_t TAX_INCOME_QUARTER_PERCENT = 15; ///< Income tax rate of the first step (profit up to TAX_INCOME_BRACKET1).
static constexpr int64_t TAX_INCOME_BRACKET1 = 5'000'000;  ///< End of the first step, quarter profit at price index 1.0.
static constexpr int64_t TAX_INCOME_BRACKET2 = 20'000'000; ///< End of the second step, quarter profit at price index 1.0.
static constexpr int64_t TAX_INCOME_PERCENT_2 = 28;        ///< Income tax rate between the two steps.
static constexpr int64_t TAX_INCOME_PERCENT_3 = 40;        ///< Income tax rate above the second step.

/**
 * Income tax of one quarter, by steps.
 * @param profit Profit of the quarter (a loss or 0 gives 0).
 * @param index_ppm Price index in ppm (GetPriceIndexPpm), 1,000,000 = 1.0; the steps are scaled by it.
 * @return The tax, integer only.
 */
inline int64_t IncomeTaxOfQuarter(int64_t profit, int64_t index_ppm)
{
	if (profit <= 0) return 0;
	const int64_t b1 = TAX_INCOME_BRACKET1 * index_ppm / 1'000'000;
	const int64_t b2 = TAX_INCOME_BRACKET2 * index_ppm / 1'000'000;
	int64_t tax = std::min(profit, b1) * TAX_INCOME_QUARTER_PERCENT / 100;
	if (profit > b1) tax += (std::min(profit, b2) - b1) * TAX_INCOME_PERCENT_2 / 100;
	if (profit > b2) tax += (profit - b2) * TAX_INCOME_PERCENT_3 / 100;
	return tax;
}

/** VAT on an income of cargo delivery: taken from the company cash now. */
void ChargeVat(CompanyID company, int64_t income);

/** Exchange fee of a delivery across a zone border (zone_market.cpp): taken from the company cash now, booked as LedgerItem::ZoneFee, paid to the bank. */
void ChargeZoneFee(CompanyID company, int64_t amount);

/**
 * Tax on a profit that is about to be paid to a wallet (closed trade, dividend). It is booked in the ledger and goes to the bank,
 * but the cash is NOT touched: the caller pays the profit minus the returned tax.
 * @param item LedgerItem::RealizedProfit or LedgerItem::Dividends; other items have no tax.
 * @return The tax (0 for a loss or an item without tax).
 */
int64_t TakeInvestorTax(CompanyID company, LedgerItem item, int64_t profit);

#endif /* TAX_H */
