/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file contract.cpp Contracts (step 3): the table, the output of an industry, the sweep of gone companies and industries, the daily expiry. Integer maths only, the same on every client.
 * The rules are in contract_math.h; the commands are in contract_cmd.cpp. */

#include "stdafx.h"
#include "contract.h"
#include "contract_math.h"
#include "company_role.h"
#include "industry_owner.h"
#include "central_bank.h"
#include "market_events.h"
#include "news_func.h"
#include "cargotype.h"
#include "cargopacket.h"
#include "strings_func.h"
#include "core/format.hpp"
#include "company_base.h"
#include "company_func.h"
#include "industry.h"
#include "window_func.h"
#include "core/backup_type.hpp"
#include "game_time/game_time_manager.h"
#include <algorithm>
#include <set>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

contract::Table _contracts;

static int64_t Today()
{
	return static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
}

int64_t ContractMonthlyOutput(const Industry *i, CargoType cargo)
{
	if (i == nullptr || !IsValidCargoType(cargo)) return 0;
	for (const auto &p : i->produced) {
		if (p.cargo != cargo) continue;
		return std::max<int64_t>(p.history[THIS_MONTH].production, p.history[LAST_MONTH].production);
	}
	return 0;
}

bool ContractIndustryIdent(const Industry *i, int64_t &ident, int32_t &built)
{
	if (i == nullptr) return false;
	ident = static_cast<int64_t>(i->location.tile.base()) | (static_cast<int64_t>(i->type) << 32);
	built = static_cast<int32_t>(i->construction_date.base());
	return true;
}

void ContractCurrentSeller(const contract::Contract &c, uint8_t &seller, int32_t &seller_year)
{
	seller = c.seller;
	seller_year = c.seller_year;
	if (!contract::IsOpen(c.status)) return; // An active contract keeps its seller.
	const IndustryID iid{c.industry};
	if (!Industry::IsValidID(iid)) return;
	const CompanyID owner = GetIndustryOwner(iid);
	if (owner == CompanyID::Invalid() || owner.base() >= contract::MAX_COMPANIES_N || !Company::IsValidID(owner)) {
		seller = contract::SYSTEM;
		seller_year = 0;
	} else {
		seller = owner.base();
		seller_year = CompanyRoleYear(owner);
	}
}

/** Is the company that this number and founding year name still there? */
static bool CompanyIsThere(uint8_t company, int32_t year)
{
	if (company >= MAX_COMPANIES) return false;
	const CompanyID id{company};
	return Company::IsValidID(id) && CompanyRoleYear(id) == year;
}

/** Give a company money (or take it with a negative amount) and book it on a line of the account statement. */
static void PayWithLedger(uint8_t company, int64_t amount, LedgerItem item)
{
	if (amount == 0 || company >= MAX_COMPANIES || !Company::IsValidID(CompanyID{company})) return;
	{
		AutoRestoreBackup cur_company(_current_company, CompanyID{company});
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(-amount)));
	}
	RecordLedger(CompanyID{company}, item, amount); // The account statement (step 6: every move of money is booked).
}

/** The deposit and its return are both 'other expenses', so they cancel in the finances window. */
void ContractPay(uint8_t company, int64_t amount)
{
	PayWithLedger(company, amount, LedgerItem::ContractEscrow);
}

void ContractPayFine(uint8_t company, int64_t amount)
{
	PayWithLedger(company, amount, LedgerItem::ContractFine);
}

int ContractSweep()
{
	if (_contracts.rows.empty()) return 0;

	std::vector<contract::Payout> payouts;
	const int changed = _contracts.Sweep([](const contract::Contract &c) {
		/* Order matters: a bankrupt buyer ends the contract whatever else happened. */
		if (!CompanyIsThere(c.buyer, c.buyer_year)) return contract::Verdict::BuyerGone;

		int64_t ident = 0;
		int32_t built = 0;
		const IndustryID iid{c.industry};
		if (!Industry::IsValidID(iid) || !ContractIndustryIdent(Industry::Get(iid), ident, built) || ident != c.ident || built != c.built) return contract::Verdict::IndustryGone;

		if (c.seller != contract::SYSTEM && !CompanyIsThere(c.seller, c.seller_year)) return contract::Verdict::SellerGone;
		return contract::Verdict::Keep;
	}, Today(), payouts);

	for (const contract::Payout &p : payouts) ContractPay(p.company, p.amount);
	if (changed > 0) InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	return changed;
}

/** The industry of the contract, if it is still the same industry. */
static const Industry *IndustryOfContract(const contract::Contract &c)
{
	const IndustryID iid{c.industry};
	if (!Industry::IsValidID(iid)) return nullptr;
	const Industry *i = Industry::Get(iid);
	int64_t ident = 0;
	int32_t built = 0;
	if (!ContractIndustryIdent(i, ident, built) || ident != c.ident || built != c.built) return nullptr;
	return i;
}

/** Spec step 4: an offer, a signed contract and a refusal go into the news system of the market (market_events). Same on every client: it is game state (the saved log). */
static void PublishContractEvent(const contract::Contract &c, ContractEvent ev)
{
	MarketEventKind kind;
	switch (ev) {
		case ContractEvent::Offered:  kind = MarketEventKind::ContractOffered; break;
		case ContractEvent::Accepted: kind = MarketEventKind::ContractSigned; break;
		case ContractEvent::Refused:  kind = MarketEventKind::ContractRefused; break;
		default: return;
	}
	if (_game_mode != GM_NORMAL) return;
	const int64_t minute = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());
	MarketEvent e = _market_events.NewEvent(kind, minute, 1, c.id);
	e.target = c.buyer; // {C} of the story = the buyer
	e.fact_ppm = c.terms.qty;
	e.fact2 = std::min<int64_t>(c.terms.price_cents, 0x7FFFFFFF);
	PublishMarketEvent(e);
}

void ContractNews(const contract::Contract &c, ContractEvent ev, bool by_buyer)
{
	if (ev == ContractEvent::Offered || ev == ContractEvent::Accepted || ev == ContractEvent::Refused) {
		PublishContractEvent(c, ev); // The news of these three is the market news (every company sees it).
		return;
	}
	if (!Company::IsValidID(_local_company)) return; // A spectator or the server of a dedicated game sees no contract news.
	const uint8_t me = _local_company.base();
	const bool is_buyer = c.buyer == me && c.buyer_year == CompanyRoleYear(_local_company);
	const bool is_seller = c.seller != contract::SYSTEM && c.seller == me && c.seller_year == CompanyRoleYear(_local_company);
	if (!is_buyer && !is_seller) return;
	if (ev != ContractEvent::Expired) {
		if (by_buyer && !is_seller) return; // The buyer did it: only the seller is told.
		if (!by_buyer && !is_buyer) return; // The seller (or the NPC) did it: only the buyer is told.
	}

	const std::string industry = Industry::IsValidID(IndustryID{c.industry}) ? GetString(STR_INDUSTRY_NAME, IndustryID{c.industry}) : std::string("?");
	const std::string cargo = IsValidCargoType(c.cargo) ? GetString(CargoSpec::Get(c.cargo)->name) : std::string("?");
	const std::string terms = fmt::format("{} {} / month, {} each, {} months", c.terms.qty, cargo, GetString(STR_JUST_CURRENCY_LONG, c.terms.price_cents / contract::PRICE_UNITS), c.terms.months);
	const std::string who = by_buyer ? "The buyer" : (c.seller == contract::SYSTEM ? "The industry" : "The owner of the industry");

	std::string text;
	switch (ev) {
		case ContractEvent::Countered:
			text = fmt::format("Contract #{} with {}: {} made a counter-offer (round {} of {}): {}. Your answer is needed.", c.id, industry, who, c.round, contract::MAX_ROUNDS, terms);
			break;
		case ContractEvent::Accepted:
		case ContractEvent::Refused:
		case ContractEvent::Offered:
			return; // Done above (market news).
		case ContractEvent::Cancelled:
			text = fmt::format("Contract #{} with {} was ended by {}.", c.id, industry, who);
			break;
		case ContractEvent::Expired:
			text = fmt::format("Offer #{} for {} expired: nobody answered in {} days.", c.id, industry, contract::OFFER_EXPIRY_DAYS);
			break;
	}
	AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, text), NewsType::General, NewsStyle::Small, {});
}

/** What ContractOnDelivery counted for the delivery that is being paid now (read and cleared by ContractAdjustProfit, the next call in PayFinalDelivery). */
static int64_t _delivery_units = 0;
static int64_t _delivery_value_cents = 0;

void ContractOnDelivery(CompanyID payer, const CargoPacket *cp, CargoType cargo, uint count)
{
	_delivery_units = 0;
	_delivery_value_cents = 0;
	if (cp == nullptr || count == 0 || _contracts.rows.empty() || !Company::IsValidID(payer) || payer.base() >= contract::MAX_COMPANIES_N) return;
	if (cp->GetSource().type != SourceType::Industry) return; // Only cargo that was loaded from an industry (not from a town or a transfer chain of town cargo).
	const IndustryID iid = static_cast<IndustryID>(cp->GetSource().id);
	if (!Industry::IsValidID(iid) || !IsValidCargoType(cargo)) return;
	int64_t ident = 0;
	int32_t built = 0;
	if (!ContractIndustryIdent(Industry::Get(iid), ident, built)) return;
	/* Deterministic: the delivery runs in the game loop on every client. */
	int64_t value = 0;
	const int64_t units = _contracts.AddDelivery(payer.base(), CompanyRoleYear(payer), iid.base(), ident, built, static_cast<uint8_t>(cargo), static_cast<int64_t>(count), &value);
	if (units > 0) {
		_delivery_units = units;
		_delivery_value_cents = value;
		InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	}
}

Money ContractAdjustProfit(CompanyID payer, const CargoPacket *cp, CargoType cargo, uint count, Money normal)
{
	const int64_t units = _delivery_units;
	const int64_t value_cents = _delivery_value_cents;
	_delivery_units = 0;
	_delivery_value_cents = 0;
	(void)cargo;
	if (cp == nullptr || count == 0 || !Company::IsValidID(payer) || payer.base() >= contract::MAX_COMPANIES_N) return normal;
	if (cp->GetSource().type != SourceType::Industry) return normal;
	const IndustryID iid = static_cast<IndustryID>(cp->GetSource().id);
	if (!Industry::IsValidID(iid)) return normal;

	/* Has the industry an owner that is another company? (The owner hauling its own cargo is not cut to 70%.) */
	const CompanyID owner = GetIndustryOwner(iid);
	const bool owned = owner != CompanyID::Invalid() && owner.base() < contract::MAX_COMPANIES_N && Company::IsValidID(owner) && owner != payer;

	const int64_t profit = contract::DeliveryProfit(static_cast<int64_t>(normal), static_cast<int64_t>(count), units, value_cents / contract::PRICE_UNITS, owned);
	if (profit == static_cast<int64_t>(normal)) return normal;
	if (Company *c = Company::GetIfValid(payer)) c->cur_economy.income += Money(profit - static_cast<int64_t>(normal)); // DeliverGoods booked `normal`.
	return Money(profit);
}

int ContractAutoAnswer()
{
	if (_contracts.rows.empty()) return 0;
	const int64_t today = Today();

	/* First collect the ids (the walk must not change the map), then answer one by one: an accepted contract changes what the next one may book. */
	std::vector<uint32_t> due;
	for (auto &[id, c] : _contracts.rows) {
		if (!contract::IsOpen(c.status)) continue;
		uint8_t seller = contract::SYSTEM;
		int32_t seller_year = 0;
		ContractCurrentSeller(c, seller, seller_year);
		if (contract::AutoDue(c, seller, today)) due.push_back(id);
	}

	int answered = 0;
	for (const uint32_t id : due) {
		contract::Contract *c = _contracts.Get(id);
		if (c == nullptr) continue;
		const Industry *i = IndustryOfContract(*c);
		if (i == nullptr) continue; // The sweep closes it.
		ContractCurrentSeller(*c, c->seller, c->seller_year); // The open contract follows the industry.

		const int64_t output = ContractMonthlyOutput(i, c->cargo);
		const int64_t cash = (c->buyer < MAX_COMPANIES && Company::IsValidID(CompanyID{c->buyer})) ? static_cast<int64_t>(GetUnborrowedLiquidCash(CompanyID{c->buyer})) : 0;
		const contract::AutoAnswer a = contract::AutoDecide(*c, GetCargoUnitPriceCents(c->cargo));

		bool refuse = a.kind == contract::AutoKind::Refuse;
		if (a.kind == contract::AutoKind::Accept) {
			if (_contracts.Accept(id, contract::Side::Seller, output, cash, today) == contract::Result::Ok) {
				ContractPay(c->buyer, -c->escrow); // The escrow leaves the cash of the buyer.
				ContractNews(*c, ContractEvent::Accepted, false);
			} else {
				refuse = true; // The deal does not fit any more (output fell, share used up, buyer without cash): the NPC says no.
			}
		} else if (a.kind == contract::AutoKind::Counter) {
			if (_contracts.Counter(id, contract::Side::Seller, a.terms, output, cash, today) == contract::Result::Ok) {
				ContractNews(*c, ContractEvent::Countered, false);
			} else {
				refuse = true;
			}
		}
		if (refuse && _contracts.Refuse(id, contract::Side::Seller, today) == contract::Result::Ok) ContractNews(*c, ContractEvent::Refused, false);
		if (a.kind != contract::AutoKind::Wait) answered++;
	}
	if (answered > 0) InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	return answered;
}

/** Contracts that were already warned about a low output this month (not saved: after a load the warning may come once more). */
static std::set<uint32_t> _warned_output;

/** A news item for the local company when it is the buyer or the seller of the contract. Local only, no game state. */
static void LocalContractText(const contract::Contract &c, const std::string &text, bool to_buyer = true, bool to_seller = true)
{
	if (!Company::IsValidID(_local_company)) return;
	const uint8_t me = _local_company.base();
	const bool is_buyer = c.buyer == me && c.buyer_year == CompanyRoleYear(_local_company);
	const bool is_seller = c.seller != contract::SYSTEM && c.seller == me && c.seller_year == CompanyRoleYear(_local_company);
	if (!((is_buyer && to_buyer) || (is_seller && to_seller))) return;
	AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, text), NewsType::General, NewsStyle::Small, {});
}

static std::string IndustryNameOf(const contract::Contract &c)
{
	return Industry::IsValidID(IndustryID{c.industry}) ? GetString(STR_INDUSTRY_NAME, IndustryID{c.industry}) : std::string("?");
}

/** Spec step 6, warning: the output of an industry fell below the amount of an active contract. Once a month per contract, to the owner and the buyer. */
static void ContractOutputWarnings()
{
	for (const auto &[id, c] : _contracts.rows) {
		if (c.status != contract::Status::Active || _warned_output.count(id) != 0) continue;
		const Industry *i = IndustryOfContract(c);
		if (i == nullptr) continue;
		const int64_t output = ContractMonthlyOutput(i, c.cargo);
		if (output <= 0 || output >= c.terms.qty) continue; // 0 = no figure yet (a new industry): no warning
		_warned_output.insert(id);
		LocalContractText(c, fmt::format("Warning, contract #{} with {}: the industry produces {} a month but the contract needs {}. A shortfall costs the owner a fine of {}% of the missing units.",
				c.id, IndustryNameOf(c), output, c.terms.qty, contract::PENALTY_PERCENT));
	}
}

/** The result of one contract at the end of the month, as news of the local company (the buyer or the owner of the industry). */
static void ContractMonthNews(const contract::MonthResult &r)
{
	const contract::Contract *c = _contracts.Get(r.id);
	if (c == nullptr) return;
	std::string text = fmt::format("Contract #{} with {}: month closed, {} of {} delivered", c->id, IndustryNameOf(*c), r.delivered, r.qty);
	if (r.missing > 0) {
		text += fmt::format(". Short by {}: fine {} (paid {}", r.missing, GetString(STR_JUST_CURRENCY_LONG, r.fine), GetString(STR_JUST_CURRENCY_LONG, r.fine_paid));
		if (r.debt_after > 0) text += fmt::format(", still owed {}", GetString(STR_JUST_CURRENCY_LONG, r.debt_after));
		text += ")";
		if (r.free_cancel) text += ". The buyer may now end the contract without a fine";
	}
	if (r.completed) text += fmt::format(". Contract completed, escrow of {} returned", GetString(STR_JUST_CURRENCY_LONG, r.escrow_back));
	LocalContractText(*c, text + ".");
}

int ContractMonthly(int64_t today)
{
	const int64_t month = today / contract::DAYS_PER_MONTH;
	if (_contracts.last_month < 0 || month < _contracts.last_month) {
		_contracts.last_month = month; // A new game or an old savegame (or a clock that went back): learn the month, settle nothing.
		_warned_output.clear();
		return 0;
	}
	if (month == _contracts.last_month) return 0;
	_contracts.last_month = month;
	_warned_output.clear();
	if (_contracts.rows.empty()) return 0;

	/* Unborrowed cash of an owner: a fine never pushes it into the red (the rest becomes a debt of the contract). */
	const std::vector<contract::MonthResult> results = _contracts.SettleMonth([](uint8_t company) -> int64_t {
		if (company >= MAX_COMPANIES || !Company::IsValidID(CompanyID{company})) return 0;
		return static_cast<int64_t>(GetUnborrowedLiquidCash(CompanyID{company}));
	}, today);

	for (const contract::MonthResult &r : results) {
		if (r.fine_paid > 0) {
			ContractPayFine(r.seller, -r.fine_paid); // the owner of the industry pays ...
			ContractPayFine(r.buyer, r.fine_paid);   // ... the buyer gets it
		}
		if (r.escrow_back > 0) ContractPay(r.buyer, r.escrow_back);
		ContractMonthNews(r);
	}
	if (!results.empty()) InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	return static_cast<int>(results.size());
}

int ContractDaily()
{
	const int64_t today = Today();
	ContractMonthly(today); // Learns the month even when there are no contracts (so that the first contract is settled at the right time).
	if (_contracts.rows.empty()) return 0;
	ContractAutoAnswer(); // Before the expiry: an answer on the last day still counts.
	std::vector<uint32_t> closed;
	const int expired = _contracts.ExpireOpen(today, &closed);
	for (const uint32_t id : closed) {
		if (const contract::Contract *c = _contracts.Get(id)) ContractNews(*c, ContractEvent::Expired, false);
	}
	ContractOutputWarnings();
	const int purged = _contracts.PurgeClosed(today);
	if (expired > 0 || purged > 0) InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	return expired;
}

static const GameTimeSubscription _contract_hourly(GameTimeEvent::Hour, [](const GameTimeSnapshot &) { ContractSweep(); }, 24);
static const GameTimeSubscription _contract_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &) { ContractDaily(); }, 24);
