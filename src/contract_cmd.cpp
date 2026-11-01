/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file contract_cmd.cpp The commands of the contracts (step 3). Every command checks rights and money first (the test run uses a COPY of the table), changes the table only
 * when it is executed, and gives the same result on every client. The rules are in contract_math.h. */

#include "stdafx.h"
#include "contract.h"
#include "contract_math.h"
#include "central_bank.h"
#include "company_role.h"
#include "industry_owner.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "industry.h"
#include "window_func.h"
#include "game_time/game_time_manager.h"

#include "table/strings.h"

#include "safeguards.h"

static int64_t Today()
{
	return static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
}

/** The text of an answer of the rules. */
CommandCost ContractErrorOf(contract::Result r)
{
	switch (r) {
		case contract::Result::BadQty: return CommandCost(STR_CONTRACT_ERROR_BAD_QTY);
		case contract::Result::BadPrice: return CommandCost(STR_CONTRACT_ERROR_BAD_PRICE);
		case contract::Result::BadMonths: return CommandCost(STR_CONTRACT_ERROR_BAD_MONTHS);
		case contract::Result::NoSuchContract: return CommandCost(STR_CONTRACT_ERROR_NO_SUCH);
		case contract::Result::SelfDeal: return CommandCost(STR_CONTRACT_ERROR_SELF_DEAL);
		case contract::Result::NotProducing: return CommandCost(STR_CONTRACT_ERROR_NOT_PRODUCING);
		case contract::Result::TooMany: return CommandCost(STR_CONTRACT_ERROR_TOO_MANY);
		case contract::Result::DuplicateOpen: return CommandCost(STR_CONTRACT_ERROR_DUPLICATE);
		case contract::Result::ExceedsFree: return CommandCost(STR_CONTRACT_ERROR_EXCEEDS_FREE);
		case contract::Result::ExceedsShare: return CommandCost(STR_CONTRACT_ERROR_EXCEEDS_SHARE);
		case contract::Result::NoCash: return CommandCost(STR_CONTRACT_ERROR_NO_CASH);
		case contract::Result::NotOpen: return CommandCost(STR_CONTRACT_ERROR_NOT_OPEN);
		case contract::Result::NotYourTurn: return CommandCost(STR_CONTRACT_ERROR_NOT_YOUR_TURN);
		case contract::Result::TimedOut: return CommandCost(STR_CONTRACT_ERROR_TIMED_OUT);
		case contract::Result::TooManyRounds: return CommandCost(STR_CONTRACT_ERROR_TOO_MANY_ROUNDS);
		case contract::Result::SameTerms: return CommandCost(STR_CONTRACT_ERROR_SAME_TERMS);
		case contract::Result::NotAllowed: return CommandCost(STR_CONTRACT_ERROR_NOT_ALLOWED);
		case contract::Result::AlreadyClosed: return CommandCost(STR_CONTRACT_ERROR_CLOSED);
		case contract::Result::Ok: return CommandCost(EXPENSES_OTHER);
	}
	return CMD_ERROR;
}

/** Short name for the commands of this file. */
static inline CommandCost ErrorOf(contract::Result r) { return ContractErrorOf(r); }

/** The table that a command works on: the real one when it is executed, a COPY in the test run (so that the test run changes nothing and both runs give the same answer). */
static contract::Table &WorkTable(DoCommandFlags flags, contract::Table &scratch)
{
	if (flags.Test(DoCommandFlag::Execute)) return _contracts;
	scratch = _contracts;
	return scratch;
}

/** Is the command given by a company that exists? Contracts are for the companies of the game (not for spectators or the deity). */
static bool IsContractCompany(CompanyID company)
{
	return Company::IsValidID(company) && company.base() < contract::MAX_COMPANIES_N;
}

/** Unborrowed cash of the buyer (0 when the buyer is gone). */
static int64_t BuyerCash(const contract::Contract &c)
{
	if (c.buyer >= MAX_COMPANIES || !Company::IsValidID(CompanyID{c.buyer})) return 0;
	return static_cast<int64_t>(GetUnborrowedLiquidCash(CompanyID{c.buyer}));
}

/** Which side of this contract is the company? False when it is neither the buyer nor the seller (the SYSTEM has no company: it answers through ContractAutoAnswer, step 4). */
static bool SideOf(const contract::Contract &c, CompanyID company, contract::Side &side)
{
	const int32_t year = CompanyRoleYear(company);
	if (c.buyer == company.base() && c.buyer_year == year) {
		side = contract::Side::Buyer;
		return true;
	}
	uint8_t seller = contract::SYSTEM;
	int32_t seller_year = 0;
	ContractCurrentSeller(c, seller, seller_year);
	if (seller != contract::SYSTEM && seller == company.base() && seller_year == year) {
		side = contract::Side::Seller;
		return true;
	}
	return false;
}

/** The industry of the contract, if it is still the same industry. */
static const Industry *IndustryOf(const contract::Contract &c)
{
	const IndustryID iid{c.industry};
	if (!Industry::IsValidID(iid)) return nullptr;
	const Industry *i = Industry::Get(iid);
	int64_t ident = 0;
	int32_t built = 0;
	if (!ContractIndustryIdent(i, ident, built) || ident != c.ident || built != c.built) return nullptr;
	return i;
}

/** An open contract follows the industry: when somebody bought the industry after the offer, the new owner answers. Done on the work table in both runs. */
static void SyncSeller(contract::Contract &c)
{
	if (!contract::IsOpen(c.status)) return;
	ContractCurrentSeller(c, c.seller, c.seller_year);
}

CommandCost CmdContractOffer(DoCommandFlags flags, IndustryID industry, uint8_t cargo, uint32_t qty, uint32_t price_cents, uint8_t months)
{
	if (!IsContractCompany(_current_company)) return CMD_ERROR;
	if (!Industry::IsValidID(industry)) return CommandCost(STR_CONTRACT_ERROR_NO_INDUSTRY);
	if (cargo >= NUM_CARGO || !IsValidCargoType(cargo)) return CommandCost(STR_CONTRACT_ERROR_NOT_PRODUCING);

	const Industry *i = Industry::Get(industry);
	contract::Deal d;
	d.buyer = _current_company.base();
	d.buyer_year = CompanyRoleYear(_current_company);
	d.industry = industry.base();
	d.cargo = cargo;
	if (!ContractIndustryIdent(i, d.ident, d.built)) return CMD_ERROR;
	const CompanyID owner = GetIndustryOwner(industry);
	if (owner == CompanyID::Invalid() || owner.base() >= contract::MAX_COMPANIES_N || !Company::IsValidID(owner)) {
		d.seller = contract::SYSTEM;
		d.seller_year = 0;
	} else {
		d.seller = owner.base();
		d.seller_year = CompanyRoleYear(owner);
	}

	contract::Table scratch;
	contract::Table &table = WorkTable(flags, scratch);
	uint32_t id = 0;
	const contract::Terms terms{static_cast<int64_t>(qty), static_cast<int64_t>(price_cents), static_cast<int64_t>(months)};
	const contract::Result r = table.Offer(d, terms, ContractMonthlyOutput(i, cargo), static_cast<int64_t>(GetUnborrowedLiquidCash(_current_company)), Today(), id);
	if (r != contract::Result::Ok) return ErrorOf(r);

	if (flags.Test(DoCommandFlag::Execute)) {
		if (const contract::Contract *made = table.Get(id)) ContractNews(*made, ContractEvent::Offered, true); // Market news (spec step 4).
		InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	}
	return CommandCost(EXPENSES_OTHER);
}

CommandCost CmdContractCounter(DoCommandFlags flags, uint32_t contract_id, uint32_t qty, uint32_t price_cents, uint8_t months)
{
	if (!IsContractCompany(_current_company)) return CMD_ERROR;

	contract::Table scratch;
	contract::Table &table = WorkTable(flags, scratch);
	contract::Contract *c = table.Get(contract_id);
	if (c == nullptr) return ErrorOf(contract::Result::NoSuchContract);
	SyncSeller(*c);

	contract::Side side;
	if (!SideOf(*c, _current_company, side)) return CommandCost(STR_CONTRACT_ERROR_NOT_YOURS);
	if (c->seller == c->buyer) return ErrorOf(contract::Result::SelfDeal);
	const Industry *i = IndustryOf(*c);
	if (i == nullptr) return CommandCost(STR_CONTRACT_ERROR_INDUSTRY_GONE);

	const contract::Terms terms{static_cast<int64_t>(qty), static_cast<int64_t>(price_cents), static_cast<int64_t>(months)};
	const contract::Result r = table.Counter(contract_id, side, terms, ContractMonthlyOutput(i, c->cargo), BuyerCash(*c), Today());
	if (r != contract::Result::Ok) return ErrorOf(r);

	if (flags.Test(DoCommandFlag::Execute)) {
		ContractNews(*c, ContractEvent::Countered, side == contract::Side::Buyer);
		InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	}
	return CommandCost(EXPENSES_OTHER);
}

CommandCost CmdContractAccept(DoCommandFlags flags, uint32_t contract_id)
{
	if (!IsContractCompany(_current_company)) return CMD_ERROR;

	contract::Table scratch;
	contract::Table &table = WorkTable(flags, scratch);
	contract::Contract *c = table.Get(contract_id);
	if (c == nullptr) return ErrorOf(contract::Result::NoSuchContract);
	SyncSeller(*c);

	contract::Side side;
	if (!SideOf(*c, _current_company, side)) return CommandCost(STR_CONTRACT_ERROR_NOT_YOURS);
	if (c->seller == c->buyer) return ErrorOf(contract::Result::SelfDeal);
	const Industry *i = IndustryOf(*c);
	if (i == nullptr) return CommandCost(STR_CONTRACT_ERROR_INDUSTRY_GONE);

	const contract::Result r = table.Accept(contract_id, side, ContractMonthlyOutput(i, c->cargo), BuyerCash(*c), Today());
	if (r != contract::Result::Ok) return ErrorOf(r);

	if (flags.Test(DoCommandFlag::Execute)) {
		/* The escrow leaves the cash of the BUYER (who may not be the company that gave this command). */
		ContractPay(c->buyer, -c->escrow);
		ContractNews(*c, ContractEvent::Accepted, side == contract::Side::Buyer);
		InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	}
	return CommandCost(EXPENSES_OTHER);
}

CommandCost CmdContractRefuse(DoCommandFlags flags, uint32_t contract_id)
{
	if (!IsContractCompany(_current_company)) return CMD_ERROR;

	contract::Table scratch;
	contract::Table &table = WorkTable(flags, scratch);
	contract::Contract *c = table.Get(contract_id);
	if (c == nullptr) return ErrorOf(contract::Result::NoSuchContract);
	SyncSeller(*c);

	contract::Side side;
	if (!SideOf(*c, _current_company, side)) return CommandCost(STR_CONTRACT_ERROR_NOT_YOURS);

	const contract::Result r = table.Refuse(contract_id, side, Today());
	if (r != contract::Result::Ok) return ErrorOf(r);

	if (flags.Test(DoCommandFlag::Execute)) {
		ContractNews(*c, ContractEvent::Refused, side == contract::Side::Buyer);
		InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	}
	return CommandCost(EXPENSES_OTHER);
}

CommandCost CmdContractCancel(DoCommandFlags flags, uint32_t contract_id)
{
	if (!IsContractCompany(_current_company)) return CMD_ERROR;

	contract::Table scratch;
	contract::Table &table = WorkTable(flags, scratch);
	contract::Contract *c = table.Get(contract_id);
	if (c == nullptr) return ErrorOf(contract::Result::NoSuchContract);
	SyncSeller(*c);

	contract::Side side;
	if (!SideOf(*c, _current_company, side)) return CommandCost(STR_CONTRACT_ERROR_NOT_YOURS);

	const uint8_t buyer = c->buyer;
	const uint8_t seller = c->seller;
	contract::Settlement settlement;
	const contract::Result r = table.Cancel(contract_id, side, Today(), settlement);
	if (r != contract::Result::Ok) return ErrorOf(r);

	if (flags.Test(DoCommandFlag::Execute)) {
		ContractPay(buyer, settlement.to_buyer);
		if (seller == contract::SYSTEM) {
			_contracts.system_income += settlement.to_seller; // The forfeited escrow that goes to the SYSTEM is only counted.
		} else {
			ContractPay(seller, settlement.to_seller);
		}
		ContractNews(*c, ContractEvent::Cancelled, side == contract::Side::Buyer);
		InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	}
	return CommandCost(EXPENSES_OTHER);
}
