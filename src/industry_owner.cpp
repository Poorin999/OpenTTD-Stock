/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file industry_owner.cpp Owners of industries (step 1). Integer maths only, the same on every client. The rules and the table are in industry_owner_math.h. */

#include "stdafx.h"
#include "industry_owner.h"
#include "industry_owner_math.h"
#include "cargo_market.h"
#include "central_bank.h"
#include "zone_market.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "industry.h"
#include "window_func.h"
#include "window_gui.h"
#include "textbuf_gui.h"
#include "strings_func.h"
#include "game_time/game_time_manager.h"

#include "table/strings.h"

#include "safeguards.h"

indowner::Table _industry_owners;

/** The distance (tiles) and the number of units that the price of one unit is measured on: the pay of the game for 100 units over 20 tiles, no time in transit, divided by 100. */
static constexpr uint PRICE_REF_TILES = 20;
static constexpr uint PRICE_REF_UNITS = 100;

indowner::Identity IndustryIdentity(const Industry *industry)
{
	indowner::Identity id;
	id.type = static_cast<uint8_t>(industry->type);
	id.tile = static_cast<uint32_t>(industry->location.tile.base());
	id.built = static_cast<int32_t>(industry->construction_date.base());
	return id;
}

/** The market price of one unit of a cargo now (money units): the pay of the game with the price and the freight rate of the cargo prices window. */
static int64_t UnitPrice(CargoType cargo)
{
	const int64_t base = static_cast<int64_t>(GetTransportedGoodsIncome(PRICE_REF_UNITS, PRICE_REF_TILES, 0, cargo)) / static_cast<int64_t>(PRICE_REF_UNITS);
	return std::max<int64_t>(0, static_cast<int64_t>(CargoMarketAdjustIncome(cargo, Money(base))));
}

int64_t GetCargoUnitPriceCents(CargoType cargo)
{
	if (!IsValidCargoType(cargo)) return 0;
	/* The pay of the game for PRICE_REF_UNITS units is, in money units, the same number as the price of one unit in 1/100 (units = 100). */
	static_assert(PRICE_REF_UNITS == 100, "cents of one unit = money units of 100 units");
	const Money pay = GetTransportedGoodsIncome(PRICE_REF_UNITS, PRICE_REF_TILES, 0, cargo);
	return std::max<int64_t>(0, static_cast<int64_t>(CargoMarketAdjustIncome(cargo, pay)));
}

int64_t GetIndustryOwnershipPrice(const Industry *industry)
{
	if (industry == nullptr) return 0;
	bool produces = false;
	int64_t monthly_value = 0;
	for (const Industry::ProducedCargo &p : industry->produced) {
		if (!IsValidCargoType(p.cargo)) continue;
		produces = true;
		/* The output of a month: the larger of this month so far and last month, so that a new industry is not priced at nothing. */
		const int64_t units = std::max<int64_t>(p.history[THIS_MONTH].production, p.history[LAST_MONTH].production);
		monthly_value += indowner::SaturatingMul(units, UnitPrice(p.cargo));
	}
	if (!produces) return 0;
	return indowner::OwnershipPrice(monthly_value);
}

CompanyID GetIndustryOwner(IndustryID industry)
{
	if (!Industry::IsValidID(industry)) return CompanyID::Invalid();
	const uint8_t owner = _industry_owners.OwnerOf(industry.base(), IndustryIdentity(Industry::Get(industry)));
	if (owner == indowner::SYSTEM || owner >= MAX_COMPANIES) return CompanyID::Invalid();
	return CompanyID{owner};
}

bool IsIndustryOwnedBy(IndustryID industry, CompanyID company)
{
	return Company::IsValidID(company) && GetIndustryOwner(industry) == company;
}

int GetIndustryLevel(IndustryID industry)
{
	if (!Industry::IsValidID(industry)) return 0;
	return _industry_owners.Level(industry.base(), IndustryIdentity(Industry::Get(industry)));
}

static int64_t Today()
{
	return static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
}

/**
 * Buy the right of ownership of an industry from the system.
 * @param flags Type of operation.
 * @param industry The industry.
 * @return The price (booked inside the command, not through the cost), or an error.
 */
CommandCost CmdBuyIndustryOwnership(DoCommandFlags flags, IndustryID industry)
{
	if (!Company::IsValidID(_current_company) || _current_company.base() >= indowner::MAX_COMPANIES_N) return CMD_ERROR;
	if (!Industry::IsValidID(industry)) return CMD_ERROR;
	const Industry *ind = Industry::Get(industry);

	const int64_t price = GetIndustryOwnershipPrice(ind);
	const indowner::Identity id = IndustryIdentity(ind);
	const uint8_t buyer = _current_company.base();
	const int64_t cash = static_cast<int64_t>(GetUnborrowedLiquidCash(_current_company)); // Nothing is bought with borrowed money.

	switch (_industry_owners.CanBuy(industry.base(), id, buyer, cash, price, price > 0)) {
		case indowner::BuyResult::Ok: break;
		case indowner::BuyResult::NotForSale: return CommandCost(STR_INDUSTRY_OWNER_ERROR_NOT_FOR_SALE);
		case indowner::BuyResult::AlreadyYours: return CommandCost(STR_INDUSTRY_OWNER_ERROR_YOURS);
		case indowner::BuyResult::OwnedByOther: return CommandCost(STR_INDUSTRY_OWNER_ERROR_OWNED);
		case indowner::BuyResult::NoCash: return CommandCost(STR_INDUSTRY_OWNER_ERROR_NO_CASH);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		const Company *c = Company::Get(_current_company);
		_industry_owners.Buy(industry.base(), id, buyer, static_cast<int32_t>(c->inaugurated_year.base()), cash, price, true, Today());
		/* Like the permit fee, the price is taken here and not through the cost that the command returns. */
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(price)));
		RecordLedger(_current_company, LedgerItem::IndustryBuy, -price); // The account statement.
		/* Paid to the system: the central bank of the zone of the industry (of the company when the industry stands outside every zone). */
		const int zone = _zone_market.count > 0 ? _zone_market.ZoneOfTile(ind->location.tile) : -1;
		(zone >= 0 ? BankOfZone(zone) : BankOfCompany(_current_company)).trading_revenue_cents += price * 100;
		InvalidateWindowClassesData(WC_INDUSTRY_VIEW, 0);
	}
	return CommandCost(EXPENSES_OTHER);
}

int IndustryOwnerSweep()
{
	if (_industry_owners.rows.empty()) return 0;
	const indowner::SweepResult r = _industry_owners.Sweep([](uint32_t index, const indowner::Row &row) {
		/* The industry: still there, and the same one that the row was made for (the game re-uses an index). */
		if (index >= 0xFFFF || !Industry::IsValidID(IndustryID{static_cast<uint16_t>(index)})) return indowner::Verdict::IndustryGone;
		if (IndustryIdentity(Industry::Get(IndustryID{static_cast<uint16_t>(index)})) != row.id) return indowner::Verdict::IndustryGone;
		/* The owner: still there, and the same company (the game re-uses a company slot). */
		if (row.owner >= MAX_COMPANIES || !Company::IsValidID(CompanyID{row.owner})) return indowner::Verdict::OwnerGone;
		if (static_cast<int32_t>(Company::Get(CompanyID{row.owner})->inaugurated_year.base()) != row.owner_year) return indowner::Verdict::OwnerGone;
		return indowner::Verdict::Keep;
	});
	return r.Total();
}

static const GameTimeSubscription _industry_owner_hourly(GameTimeEvent::Hour, [](const GameTimeSnapshot &) { IndustryOwnerSweep(); }, 22);

/** The answer of the confirmation window: buy when the player said yes. The command checks everything again and shows the reason when it refuses. */
static void BuyIndustryAnswer(Window *parent, bool confirmed)
{
	if (!confirmed || parent == nullptr) return;
	const IndustryID iid{static_cast<uint16_t>(parent->window_number)};
	if (!Industry::IsValidID(iid)) return;
	Command<CMD_BUY_INDUSTRY_OWNERSHIP>::Post(STR_INDUSTRY_OWNER_ERROR_CAPTION, iid);
}

void ShowIndustryBuyPrompt(Window *parent, IndustryID industry)
{
	if (parent == nullptr || !Industry::IsValidID(industry)) return;
	const Industry *i = Industry::Get(industry);
	const CompanyID owner = GetIndustryOwner(industry);
	const int64_t price = GetIndustryOwnershipPrice(i);
	std::string text;
	if (owner != CompanyID::Invalid()) {
		text = fmt::format("เจ้าของโรงงานนี้คือบริษัทที่ {} ซื้อไม่ได้ / Owned by company {}.", owner.base() + 1, owner.base() + 1);
	} else if (price <= 0) {
		text = "โรงงานนี้ไม่มีผลผลิต จึงไม่ขาย / This industry produces nothing: not for sale.";
	} else {
		text = fmt::format("ซื้อสิทธิ์เป็นเจ้าของโรงงานนี้ ราคา {} (จ่ายจากเงินสดที่ไม่ได้กู้) / Buy ownership for {} (unborrowed cash).",
			GetString(STR_JUST_CURRENCY_LONG, price), GetString(STR_JUST_CURRENCY_LONG, price));
	}
	ShowQuery(GetEncodedString(STR_INDUSTRY_OWNER_QUERY_CAPTION), GetEncodedString(STR_JUST_RAW_STRING, text), parent, BuyIndustryAnswer);
}
