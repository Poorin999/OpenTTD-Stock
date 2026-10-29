/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file town_wallet.h City wallet: the town pays for consumer cargo from its treasury, takes a business tax, borrows (bonds, central bank) and gets a yearly grant. See town_wallet_math.h. */

#ifndef TOWN_WALLET_H
#define TOWN_WALLET_H

#include "stdafx.h"
#include "station_type.h"
#include "cargo_type.h"

/**
 * Called for every final delivery (economy.cpp, hook TOWN_WALLET_PATCH) before the income is booked.
 * Goods / Food / Water: the town of the destination station pays from its treasury (it first sells bonds or borrows from the bank when the treasury is short);
 * the company gets what the town could pay. Other industrial cargo: the town takes a business tax. Passengers are paid from the cash of the citizens of the town of ORIGIN (source station), mail from its treasury, at most 25% of the purse per payment. Gold is not touched.
 * For passengers, mail and consumer cargo WAGE_SHARE_PERCENT (town_wallet_math.h) of the payment goes back into the cash of the citizens of the town that paid.
 * @return What the company is paid.
 */
int64_t TownPayForDelivery(CargoType cargo, StationID source, StationID dest, int64_t income);

#endif /* TOWN_WALLET_H */
