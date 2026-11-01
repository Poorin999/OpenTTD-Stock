/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file contract_math.h Contracts between a buyer and an industry (step 3 of the contract system): integer maths only and no game headers, so that contract_test.cpp can test it alone.
 *
 * A contract says: the BUYER takes `qty` units a month of one cargo from one industry at `price_cents` (1/100 of a money unit) per unit, for `months` months.
 * The SELLER is the owner of the industry (a company, or SYSTEM = the NPC that owns every industry nobody bought).
 *
 * Life of a contract:
 *   Offered   the buyer made an offer (round 1); the seller must answer.
 *   Countered the side that was asked answered with new terms (round 2, round 3); the other side must answer.
 *   Active    somebody accepted: the buyer paid the ESCROW (one month of the contract) and the output is reserved.
 *   Closed:   Refused, Cancelled, Expired (nobody answered in 7 game days, or the 3 rounds are over), Completed (step 6).
 * Only the contracts that are not closed are saved. Nothing here changes money: every function that moves money tells the game what to pay (Settlement, Payout).
 *
 * DEFAULTS CHOSEN BY THE DESIGNER (the spec does not say; change the constants below): see the block "Decisions".
 */

#ifndef CONTRACT_MATH_H
#define CONTRACT_MATH_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace contract {

/* ---- The numbers of the spec (table 2 of the spec) ---- */
static constexpr int MAX_ROUNDS = 3;            ///< The negotiation has at most 3 rounds (offer, counter, counter).
static constexpr int OFFER_EXPIRY_DAYS = 7;     ///< An offer that is not answered in 7 game days is closed (every move starts the 7 days again).
static constexpr int BUYER_CAP_PERCENT = 60;    ///< One buyer may book at most 60% of the output of an industry.
static constexpr int ESCROW_MONTHS = 1;         ///< The escrow is the value of one month.
static constexpr int PENALTY_PERCENT = 5;       ///< Step 6: the fine of a shortfall (kept here with the other numbers).
static constexpr int FREE_CANCEL_MISS_STREAK = 2; ///< The buyer may cancel without a fine after 2 months in a row with a shortfall.
static constexpr int PRICE_UNITS = 100;         ///< `price_cents` is in 1/100 of a money unit.
static constexpr int DAYS_PER_MONTH = 30;       ///< A game month has 30 game days (the same as the game clock); the monthly settlement runs when `game day / 30` changes.

/* ---- Decisions of the designer (not in the spec) ---- */
static constexpr int MIN_MONTHS = 1;            ///< Shortest contract.
static constexpr int MAX_MONTHS = 36;           ///< Longest contract.
static constexpr int64_t MAX_QTY = 1000000;     ///< Sanity limit of units a month.
static constexpr int64_t MAX_PRICE_CENTS = 1000000000; ///< Sanity limit of the price of one unit (1/100 money unit).
static constexpr int MAX_LIVE_PER_BUYER = 20;   ///< Open offers + active contracts of one buyer (so that nobody floods the table).
static constexpr int CLOSED_KEEP_DAYS = 30;     ///< A closed contract stays in memory (for windows and news) this long; it is never saved.
static constexpr uint32_t MAX_ID = 0xFFFFFF;    ///< Ids are 24 bits (the saved record keeps the part number above them).
/** An early cancel by the buyer (not after a shortfall streak) gives the escrow to the seller as a fine. The seller can not cancel an active contract. */
static constexpr bool EARLY_CANCEL_FORFEITS_ESCROW = true;

static constexpr uint8_t SYSTEM = 255;          ///< The seller of an industry that no company owns.
static constexpr uint8_t MAX_COMPANIES_N = 15;  ///< Companies are 0 .. 14 (MAX_COMPANIES of OpenTTD).

enum class Side : uint8_t { Buyer = 0, Seller = 1 };
inline Side Other(Side s) { return s == Side::Buyer ? Side::Seller : Side::Buyer; }

enum class Status : uint8_t { Offered = 0, Countered = 1, Active = 2, Refused = 3, Cancelled = 4, Expired = 5, Completed = 6 };
static constexpr uint8_t STATUS_COUNT = 7;
inline bool IsValidStatus(uint8_t raw) { return raw < STATUS_COUNT; }
inline bool IsOpen(Status s) { return s == Status::Offered || s == Status::Countered; }
inline bool IsClosed(Status s) { return s == Status::Refused || s == Status::Cancelled || s == Status::Expired || s == Status::Completed; }

/** The terms of one move of the negotiation. */
struct Terms {
	int64_t qty = 0;         ///< Units a month.
	int64_t price_cents = 0; ///< Price of one unit in 1/100 of a money unit.
	int64_t months = 0;      ///< Length in months.
	bool operator==(const Terms &o) const { return qty == o.qty && price_cents == o.price_cents && months == o.months; }
};

/** What a command can answer. One list for all the commands (the game maps each value to a string). */
enum class Result : uint8_t {
	Ok,
	BadQty,          ///< Quantity is 0 or too large.
	BadPrice,        ///< Price is 0 or too large.
	BadMonths,       ///< Length is out of MIN_MONTHS .. MAX_MONTHS.
	NoSuchContract,
	SelfDeal,        ///< The buyer owns the industry.
	NotProducing,    ///< The industry produces none of this cargo.
	TooMany,         ///< The buyer has MAX_LIVE_PER_BUYER contracts, or the table is full.
	DuplicateOpen,   ///< The buyer has an open offer for this industry and cargo already.
	ExceedsFree,     ///< More than the free output (output of a month minus the active contracts).
	ExceedsShare,    ///< More than the share that one buyer may book (60% of the output).
	NoCash,          ///< The buyer has not enough unborrowed cash for the escrow.
	NotOpen,         ///< The contract is not Offered or Countered any more (or does not exist for this move).
	NotYourTurn,     ///< The other side has to answer.
	TimedOut,        ///< The offer is older than 7 game days.
	TooManyRounds,   ///< The 3 rounds are used: only accept or refuse.
	SameTerms,       ///< A counter-offer with the very same terms.
	NotAllowed,      ///< For example: the seller wants to cancel an active contract.
	AlreadyClosed,   ///< The contract is closed.
};

/** One contract. The industry is kept with its IDENTITY (tile, type, construction date) because the game re-uses industry numbers. */
struct Contract {
	uint32_t id = 0;
	uint8_t buyer = 0;
	int32_t buyer_year = 0;      ///< Founding year of the buyer (the game re-uses company slots).
	uint8_t seller = SYSTEM;     ///< Company number or SYSTEM.
	int32_t seller_year = 0;     ///< Founding year of the seller (0 for SYSTEM).
	uint16_t industry = 0;       ///< Pool index of the industry.
	int64_t ident = 0;           ///< tile | type << 32 of the industry.
	int32_t built = 0;           ///< Construction date of the industry.
	uint8_t cargo = 0;
	Terms terms{};               ///< The terms now on the table.
	uint8_t months_total = 0;    ///< Length when it was accepted (or offered).
	uint8_t months_left = 0;     ///< Active only.
	uint8_t round = 1;           ///< 1 .. MAX_ROUNDS.
	Side turn = Side::Seller;    ///< Who must answer (open contracts).
	Status status = Status::Offered;
	int64_t escrow = 0;          ///< Money held for the buyer (Active only).
	int64_t delivered = 0;       ///< Units delivered this month (step 5 fills it, step 6 sets it to 0 at the month end).
	uint8_t miss_streak = 0;     ///< Months in a row with a shortfall (step 6 fills it).
	int64_t fine_debt = 0;       ///< Step 6: fine that the seller owed the buyer and could not pay yet (money units). Paid at the next settlement as far as the seller has unborrowed cash.
	int64_t expires_day = 0;     ///< Last game day on which the open contract can be answered.
	int64_t closed_day = 0;      ///< Game day of the close (not saved).
};

/** Money that a command or a sweep tells the game to move. `company` is a company number, or SYSTEM (then it is only counted, nobody is paid). */
struct Payout {
	uint8_t company = 0;
	int64_t amount = 0;
	uint32_t contract = 0;
};

/** What a cancel does with the escrow. */
struct Settlement {
	int64_t to_buyer = 0;  ///< Back to the buyer.
	int64_t to_seller = 0; ///< To the seller as a fine (the game pays a company, or counts it for SYSTEM).
};

/** What the monthly settlement (step 6) did to one contract: for the news, the ledger and the tests. Money units. */
struct MonthResult {
	uint32_t id = 0;
	uint8_t buyer = 0;
	uint8_t seller = SYSTEM;
	int64_t qty = 0;               ///< Agreed units a month.
	int64_t delivered = 0;         ///< Units counted this month.
	int64_t missing = 0;           ///< qty - delivered, not below 0.
	int64_t price_cents = 0;
	int64_t paid_at_delivery = 0;  ///< Value of the delivered units at the contract price (already paid at the deliveries, step 5).
	int64_t fine = 0;              ///< Fine of this month (PENALTY_PERCENT of the value of the missing units; 0 for a SYSTEM seller).
	int64_t fine_paid = 0;         ///< Paid to the buyer now (this month's fine plus the old debt, as far as the seller had cash).
	int64_t debt_after = 0;        ///< Still owed to the buyer.
	int64_t escrow_back = 0;       ///< Returned to the buyer because the contract is Completed.
	int streak = 0;                ///< Months in a row with a shortfall, after this month.
	bool free_cancel = false;      ///< The buyer may now end the contract without a fine.
	bool completed = false;
};

/** The identity of the contract that is being made. */
struct Deal {
	uint8_t buyer = 0;
	int32_t buyer_year = 0;
	uint8_t seller = SYSTEM;
	int32_t seller_year = 0;
	uint16_t industry = 0;
	int64_t ident = 0;
	int32_t built = 0;
	uint8_t cargo = 0;
};

/* ---- Pure rules ---- */

/** The value of `months` months of the terms (1/100 units -> money units, rounded down). */
inline int64_t MonthValue(int64_t qty, int64_t price_cents) { return qty * price_cents / PRICE_UNITS; }

/** The escrow of the terms: ESCROW_MONTHS months. */
inline int64_t EscrowOf(const Terms &t) { return MonthValue(t.qty, t.price_cents) * ESCROW_MONTHS; }

/** Output of a month that is not booked by an active contract. */
inline int64_t FreeOf(int64_t output, int64_t reserved) { return output > reserved ? output - reserved : 0; }

/** The most that one buyer may book of an output (60%, rounded down). */
inline int64_t BuyerCapOf(int64_t output) { return output * BUYER_CAP_PERCENT / 100; }

inline Result CheckTerms(const Terms &t)
{
	if (t.qty <= 0 || t.qty > MAX_QTY) return Result::BadQty;
	if (t.price_cents <= 0 || t.price_cents > MAX_PRICE_CENTS) return Result::BadPrice;
	if (t.months < MIN_MONTHS || t.months > MAX_MONTHS) return Result::BadMonths;
	return Result::Ok;
}

/** May `qty` more units be booked? `reserved_all` = active contracts of the industry for this cargo, `reserved_buyer` = of them the ones of this buyer. */
inline Result CheckCapacity(int64_t qty, int64_t output, int64_t reserved_all, int64_t reserved_buyer)
{
	if (output <= 0) return Result::NotProducing;
	if (qty > FreeOf(output, reserved_all)) return Result::ExceedsFree;
	if (reserved_buyer + qty > BuyerCapOf(output)) return Result::ExceedsShare;
	return Result::Ok;
}

/** The fine of a shortfall (step 6): PENALTY_PERCENT of the value of the missing units, rounded down. */
inline int64_t FineOf(int64_t missing, int64_t price_cents) { return MonthValue(missing, price_cents) * PENALTY_PERCENT / 100; }

/** The buyer may cancel without a fine: 2 months in a row with a shortfall. */
inline bool CanCancelFree(const Contract &c) { return c.miss_streak >= FREE_CANCEL_MISS_STREAK; }

/** Why a sweep closes or changes a contract. */
enum class Verdict : uint8_t { Keep, BuyerGone, SellerGone, IndustryGone };

/** The saved form of one part of a contract (the game puts it in a StockRecord with kind SRK_CONTRACT). */
struct Record {
	int32_t ym = 0;  ///< -1 = totals; else id | part << 24 (part 0 = the contract, part 1 = identity of the industry).
	uint8_t a = 0;
	uint8_t b = 0;
	int64_t v1 = 0;
	int64_t v2 = 0;
	int64_t v3 = 0;
	int64_t v4 = 0;
	int64_t v5 = 0;
};

/** All contracts. Rows are kept in a std::map: the order of a walk is the same on every client. */
struct Table {
	std::map<uint32_t, Contract> rows;
	uint32_t next_id = 1;
	int64_t system_income = 0; ///< Money paid to the SYSTEM so far (a money sink for now).
	int64_t last_month = -1;   ///< Step 6: game month (game day / 30) of the last monthly settlement; -1 = not known yet (a new game or an old savegame: the first day only learns it).

	void Reset()
	{
		this->rows.clear();
		this->next_id = 1;
		this->system_income = 0;
		this->last_month = -1;
	}

	Contract *Get(uint32_t id)
	{
		const auto it = this->rows.find(id);
		return it == this->rows.end() ? nullptr : &it->second;
	}
	const Contract *Get(uint32_t id) const
	{
		const auto it = this->rows.find(id);
		return it == this->rows.end() ? nullptr : &it->second;
	}

	static bool SameIndustry(const Contract &c, uint16_t industry, int64_t ident, int32_t built)
	{
		return c.industry == industry && c.ident == ident && c.built == built;
	}

	/** Units a month booked by the ACTIVE contracts of this industry and cargo. If `buyer` is not SYSTEM, only the ones of that buyer. */
	int64_t Reserved(uint16_t industry, int64_t ident, int32_t built, uint8_t cargo, uint8_t buyer = SYSTEM, int32_t buyer_year = 0) const
	{
		int64_t sum = 0;
		for (const auto &[id, c] : this->rows) {
			if (c.status != Status::Active || c.cargo != cargo || !SameIndustry(c, industry, ident, built)) continue;
			if (buyer != SYSTEM && (c.buyer != buyer || c.buyer_year != buyer_year)) continue;
			sum += c.terms.qty;
		}
		return sum;
	}

	/**
	 * Step 5: `count` units of `cargo` that the BUYER hauled from this industry were delivered. They are counted on the ACTIVE contracts of that buyer for this industry
	 * and cargo, in id order, each up to its amount a month (units above the amount of all the contracts are not counted: they are hauled without a contract).
	 * @param value_cents_out If given: the pay of the counted units at the CONTRACT prices, in 1/100 money units (sum of units x price_cents).
	 * @return Units that were counted.
	 */
	int64_t AddDelivery(uint8_t buyer, int32_t buyer_year, uint16_t industry, int64_t ident, int32_t built, uint8_t cargo, int64_t count, int64_t *value_cents_out = nullptr)
	{
		int64_t left = count;
		int64_t value = 0;
		for (auto &[id, c] : this->rows) {
			if (left <= 0) break;
			if (c.status != Status::Active || c.cargo != cargo || c.buyer != buyer || c.buyer_year != buyer_year || !SameIndustry(c, industry, ident, built)) continue;
			const int64_t room = std::max<int64_t>(0, c.terms.qty - c.delivered);
			const int64_t take = std::min(room, left);
			c.delivered += take;
			value += take * c.terms.price_cents;
			left -= take;
		}
		if (value_cents_out != nullptr) *value_cents_out = value;
		return count - std::max<int64_t>(0, left);
	}

	/** Open offers + active contracts of a buyer. */
	int CountLive(uint8_t buyer, int32_t buyer_year) const
	{
		int n = 0;
		for (const auto &[id, c] : this->rows) {
			if (!IsClosed(c.status) && c.buyer == buyer && c.buyer_year == buyer_year) n++;
		}
		return n;
	}

	bool HasOpen(const Deal &d) const
	{
		for (const auto &[id, c] : this->rows) {
			if (IsOpen(c.status) && c.buyer == d.buyer && c.buyer_year == d.buyer_year && c.cargo == d.cargo && SameIndustry(c, d.industry, d.ident, d.built)) return true;
		}
		return false;
	}

	/** Free output of an industry and cargo, for a window. */
	int64_t FreeOutput(uint16_t industry, int64_t ident, int32_t built, uint8_t cargo, int64_t output) const
	{
		return FreeOf(output, this->Reserved(industry, ident, built, cargo));
	}

	/**
	 * Round 1: the buyer offers. Checks (in this order): terms, a buyer that is not the seller, the industry produces the cargo, room in the table,
	 * no open offer of this buyer for the same thing, free output and the share of one buyer, cash for the escrow. Nothing is changed unless it is Ok.
	 * @param output Output of a month of this cargo at the industry (the game measures it).
	 * @param buyer_cash Unborrowed cash of the buyer.
	 * @param id_out The id of the new contract.
	 */
	Result Offer(const Deal &d, const Terms &t, int64_t output, int64_t buyer_cash, int64_t today, uint32_t &id_out)
	{
		const Result tr = CheckTerms(t);
		if (tr != Result::Ok) return tr;
		if (d.seller == d.buyer) return Result::SelfDeal;
		if (output <= 0) return Result::NotProducing;
		if (this->next_id > MAX_ID || this->CountLive(d.buyer, d.buyer_year) >= MAX_LIVE_PER_BUYER) return Result::TooMany;
		if (this->HasOpen(d)) return Result::DuplicateOpen;
		const Result cr = CheckCapacity(t.qty, output, this->Reserved(d.industry, d.ident, d.built, d.cargo), this->Reserved(d.industry, d.ident, d.built, d.cargo, d.buyer, d.buyer_year));
		if (cr != Result::Ok) return cr;
		if (buyer_cash < EscrowOf(t)) return Result::NoCash;

		Contract c;
		c.id = this->next_id++;
		c.buyer = d.buyer;
		c.buyer_year = d.buyer_year;
		c.seller = d.seller;
		c.seller_year = d.seller_year;
		c.industry = d.industry;
		c.ident = d.ident;
		c.built = d.built;
		c.cargo = d.cargo;
		c.terms = t;
		c.months_total = static_cast<uint8_t>(t.months);
		c.round = 1;
		c.turn = Side::Seller;
		c.status = Status::Offered;
		c.expires_day = today + OFFER_EXPIRY_DAYS;
		id_out = c.id;
		this->rows[c.id] = c;
		return Result::Ok;
	}

	/** The checks that every answer shares: the contract is open, not too old, and it is the turn of `actor`. */
	Result CheckTurn(uint32_t id, Side actor, int64_t today) const
	{
		const Contract *c = this->Get(id);
		if (c == nullptr) return Result::NoSuchContract;
		if (IsClosed(c->status)) return Result::AlreadyClosed;
		if (!IsOpen(c->status)) return Result::NotOpen;
		if (today > c->expires_day) return Result::TimedOut;
		if (c->turn != actor) return Result::NotYourTurn;
		return Result::Ok;
	}

	/** Round 2 or 3: the side whose turn it is answers with new terms. A counter of the buyer needs the cash for the escrow (the buyer commits to pay). */
	Result Counter(uint32_t id, Side actor, const Terms &t, int64_t output, int64_t buyer_cash, int64_t today)
	{
		const Result turn = this->CheckTurn(id, actor, today);
		if (turn != Result::Ok) return turn;
		Contract &c = this->rows[id];
		if (c.round >= MAX_ROUNDS) return Result::TooManyRounds;
		const Result tr = CheckTerms(t);
		if (tr != Result::Ok) return tr;
		if (t == c.terms) return Result::SameTerms;
		const Result cr = CheckCapacity(t.qty, output, this->Reserved(c.industry, c.ident, c.built, c.cargo), this->Reserved(c.industry, c.ident, c.built, c.cargo, c.buyer, c.buyer_year));
		if (cr != Result::Ok) return cr;
		if (actor == Side::Buyer && buyer_cash < EscrowOf(t)) return Result::NoCash;

		c.terms = t;
		c.months_total = static_cast<uint8_t>(t.months);
		c.round++;
		c.turn = Other(c.turn);
		c.status = Status::Countered;
		c.expires_day = today + OFFER_EXPIRY_DAYS;
		return Result::Ok;
	}

	/**
	 * The side whose turn it is accepts. It is checked again NOW (the output may have fallen, or another contract was accepted in the meantime):
	 * free output, the share of one buyer, and the cash of the BUYER for the escrow (whoever accepts). On Ok the contract is Active and holds the escrow:
	 * the game takes `c.escrow` from the buyer.
	 */
	Result Accept(uint32_t id, Side actor, int64_t output, int64_t buyer_cash, int64_t today)
	{
		const Result turn = this->CheckTurn(id, actor, today);
		if (turn != Result::Ok) return turn;
		Contract &c = this->rows[id];
		const Result cr = CheckCapacity(c.terms.qty, output, this->Reserved(c.industry, c.ident, c.built, c.cargo), this->Reserved(c.industry, c.ident, c.built, c.cargo, c.buyer, c.buyer_year));
		if (cr != Result::Ok) return cr;
		const int64_t escrow = EscrowOf(c.terms);
		if (buyer_cash < escrow) return Result::NoCash;

		c.status = Status::Active;
		c.months_total = static_cast<uint8_t>(c.terms.months);
		c.months_left = c.months_total;
		c.escrow = escrow;
		c.delivered = 0;
		c.miss_streak = 0;
		return Result::Ok;
	}

	/** The side whose turn it is refuses. No money was taken. */
	Result Refuse(uint32_t id, Side actor, int64_t today)
	{
		const Contract *c0 = this->Get(id);
		if (c0 == nullptr) return Result::NoSuchContract;
		if (IsClosed(c0->status)) return Result::AlreadyClosed;
		if (!IsOpen(c0->status)) return Result::NotOpen;
		if (c0->turn != actor) return Result::NotYourTurn;
		Contract &c = this->rows[id];
		c.status = Status::Refused;
		c.closed_day = today;
		return Result::Ok;
	}

	/**
	 * Cancel. An OPEN contract: either side may withdraw (no money was taken). An ACTIVE contract: only the buyer. After a shortfall streak of 2 months the escrow goes back
	 * to the buyer; before that the escrow is a fine and goes to the seller (EARLY_CANCEL_FORFEITS_ESCROW). `s` tells the game what to pay.
	 */
	Result Cancel(uint32_t id, Side actor, int64_t today, Settlement &s)
	{
		s = Settlement{};
		const Contract *c0 = this->Get(id);
		if (c0 == nullptr) return Result::NoSuchContract;
		if (IsClosed(c0->status)) return Result::AlreadyClosed;
		Contract &c = this->rows[id];
		if (c.status == Status::Active) {
			if (actor != Side::Buyer) return Result::NotAllowed;
			if (CanCancelFree(c) || !EARLY_CANCEL_FORFEITS_ESCROW) {
				s.to_buyer = c.escrow;
			} else {
				s.to_seller = c.escrow;
			}
			c.escrow = 0;
		}
		c.status = Status::Cancelled;
		c.closed_day = today;
		return Result::Ok;
	}

	/** Close the open contracts that nobody answered in time. Returns how many (the ids go to `closed` when it is given). No money moves. */
	int ExpireOpen(int64_t today, std::vector<uint32_t> *closed = nullptr)
	{
		int n = 0;
		for (auto &[id, c] : this->rows) {
			if (!IsOpen(c.status) || today <= c.expires_day) continue;
			c.status = Status::Expired;
			c.closed_day = today;
			n++;
			if (closed != nullptr) closed->push_back(id);
		}
		return n;
	}

	/**
	 * `check(contract)` returns a Verdict for every contract that is not closed.
	 *   BuyerGone     the buyer is bankrupt: the contract is Cancelled. An active contract pays its escrow to the seller (DECISION: the test table of the spec says the
	 *                 escrow goes to the owner of the industry; the 'creditors pool' of the bank text of the spec has no function that this file knows).
	 *   IndustryGone  the industry is closed or was replaced: open -> Expired; active -> Cancelled and the escrow goes back to the buyer.
	 *   SellerGone    the owner is bankrupt: the contract stays and belongs to the SYSTEM (spec).
	 * Returns how many contracts changed. `payouts` gets the money for companies (a payout to SYSTEM is only added to `system_income`).
	 */
	template <class F>
	int Sweep(F check, int64_t today, std::vector<Payout> &payouts)
	{
		int changed = 0;
		for (auto &[id, c] : this->rows) {
			if (IsClosed(c.status)) continue;
			switch (check(static_cast<const Contract &>(c))) {
				case Verdict::Keep: break;
				case Verdict::SellerGone:
					c.seller = SYSTEM;
					c.seller_year = 0;
					changed++;
					break;
				case Verdict::BuyerGone:
					if (c.status == Status::Active && c.escrow > 0) this->Pay(payouts, c.seller, c.escrow, id);
					c.escrow = 0;
					c.status = Status::Cancelled;
					c.closed_day = today;
					changed++;
					break;
				case Verdict::IndustryGone:
					if (c.status == Status::Active) {
						if (c.escrow > 0) this->Pay(payouts, c.buyer, c.escrow, id);
						c.escrow = 0;
						c.status = Status::Cancelled;
					} else {
						c.status = Status::Expired;
					}
					c.closed_day = today;
					changed++;
					break;
			}
		}
		return changed;
	}

	/** Pay a company; a payout to SYSTEM is counted only. */
	void Pay(std::vector<Payout> &payouts, uint8_t company, int64_t amount, uint32_t id)
	{
		if (amount <= 0) return;
		if (company == SYSTEM) {
			this->system_income += amount;
			return;
		}
		payouts.push_back(Payout{company, amount, id});
	}

	/** Forget the closed contracts that are older than CLOSED_KEEP_DAYS. Returns how many. */
	int PurgeClosed(int64_t today)
	{
		int n = 0;
		for (auto it = this->rows.begin(); it != this->rows.end();) {
			if (IsClosed(it->second.status) && today - it->second.closed_day > CLOSED_KEEP_DAYS) {
				it = this->rows.erase(it);
				n++;
			} else {
				++it;
			}
		}
		return n;
	}

	/**
	 * Step 6: the monthly settlement of every ACTIVE contract (id order, the same on every client). Per contract:
	 *   delivered >= amount: the streak of short months goes back to 0, nothing is forfeited, the escrow stays held (it is the bond of the whole contract).
	 *   delivered <  amount: the streak grows; the seller owes a fine of PENALTY_PERCENT of the value of the MISSING units, paid to the buyer. A seller that is a company pays what
	 *                        it has in unborrowed cash (`owner_cash`, shared by all its contracts) and the rest becomes `fine_debt`, paid at the next settlement before any new fine
	 *                        (so the cash never goes negative). The SYSTEM pays no fine (DECISION: the shortfall may be the buyer's own hauling and the NPC would create money).
	 *   The pay of the units that were delivered was already given at the delivery (step 5: contract price), so nothing more is paid for them here.
	 *   Then `delivered` is set to 0 and the months left fall by 1; at 0 the contract is Completed and the escrow goes back to the buyer.
	 * @param owner_cash Function (company number) -> unborrowed cash of that company now; read once per company.
	 */
	template <class F>
	std::vector<MonthResult> SettleMonth(F owner_cash, int64_t today)
	{
		std::vector<MonthResult> out;
		std::map<uint8_t, int64_t> avail;
		for (auto &[id, c] : this->rows) {
			if (c.status != Status::Active) continue;
			MonthResult r;
			r.id = id;
			r.buyer = c.buyer;
			r.seller = c.seller;
			r.qty = c.terms.qty;
			r.delivered = c.delivered;
			r.price_cents = c.terms.price_cents;
			r.missing = std::max<int64_t>(0, c.terms.qty - c.delivered);
			r.paid_at_delivery = MonthValue(std::min(c.delivered, c.terms.qty), c.terms.price_cents);

			int64_t *cash = nullptr;
			if (c.seller != SYSTEM) {
				auto it = avail.find(c.seller);
				if (it == avail.end()) it = avail.emplace(c.seller, std::max<int64_t>(0, owner_cash(c.seller))).first;
				cash = &it->second;
			}

			/* Old debt first, then the fine of this month. */
			int64_t owe = c.fine_debt;
			if (r.missing > 0) {
				c.miss_streak = static_cast<uint8_t>(std::min<int>(255, c.miss_streak + 1));
				if (c.seller != SYSTEM) {
					r.fine = FineOf(r.missing, c.terms.price_cents);
					owe += r.fine;
				}
			} else {
				c.miss_streak = 0;
			}
			if (owe > 0 && cash != nullptr) {
				const int64_t pay = std::min(owe, *cash);
				*cash -= pay;
				owe -= pay;
				r.fine_paid = pay;
			}
			c.fine_debt = owe;
			r.debt_after = owe;
			r.streak = c.miss_streak;
			r.free_cancel = CanCancelFree(c);

			c.delivered = 0;
			if (c.months_left > 0) c.months_left--;
			if (c.months_left == 0) {
				r.completed = true;
				r.escrow_back = c.escrow;
				c.escrow = 0;
				c.status = Status::Completed;
				c.closed_day = today;
			}
			out.push_back(r);
		}
		return out;
	}

	/** Money held in escrow by all contracts (for a total in a window). */
	int64_t TotalEscrow(uint8_t buyer = SYSTEM, int32_t buyer_year = 0) const
	{
		int64_t sum = 0;
		for (const auto &[id, c] : this->rows) {
			if (c.status != Status::Active) continue;
			if (buyer != SYSTEM && (c.buyer != buyer || c.buyer_year != buyer_year)) continue;
			sum += c.escrow;
		}
		return sum;
	}

	/* ---- Save and load ---- */

	/** Every contract that is not closed, as records (part 0, then part 1). The totals come first, only when there is something to keep. */
	std::vector<Record> ToRecords() const
	{
		std::vector<Record> out;
		bool any = false;
		for (const auto &[id, c] : this->rows) {
			if (!IsClosed(c.status)) any = true;
		}
		if (any || this->system_income != 0) {
			Record t;
			t.ym = -1;
			t.v1 = this->next_id;
			t.v2 = this->system_income;
			t.v3 = this->last_month + 1; // 0 = not known
			out.push_back(t);
		}
		for (const auto &[id, c] : this->rows) {
			if (IsClosed(c.status)) continue;
			Record r;
			r.ym = static_cast<int32_t>(c.id);
			r.a = c.buyer;
			r.b = c.seller;
			r.v1 = static_cast<int64_t>(c.industry) | (static_cast<int64_t>(c.cargo) << 16) | (static_cast<int64_t>(c.status) << 24) | (static_cast<int64_t>(c.round) << 32) |
					(static_cast<int64_t>(c.turn) << 40) | (static_cast<int64_t>(c.miss_streak) << 48) | (static_cast<int64_t>(c.months_total) << 56);
			r.v2 = static_cast<int64_t>(static_cast<uint32_t>(c.terms.qty)) | (static_cast<int64_t>(c.months_left) << 32) | (static_cast<int64_t>(static_cast<uint16_t>(c.buyer_year)) << 40);
			r.v3 = c.terms.price_cents;
			r.v4 = c.escrow;
			r.v5 = static_cast<int64_t>(static_cast<uint32_t>(c.delivered)) | (c.expires_day << 32);
			out.push_back(r);

			Record p;
			p.ym = static_cast<int32_t>(c.id | (1u << 24));
			p.v1 = c.ident;
			p.v2 = static_cast<int64_t>(static_cast<uint32_t>(c.built)) | (static_cast<int64_t>(static_cast<uint16_t>(c.seller_year)) << 32);
			p.v3 = c.fine_debt;
			out.push_back(p);
		}
		return out;
	}

	/** Read one record. A record that makes no sense is ignored (false). Part 1 needs part 0 before it (ToRecords writes them in that order). */
	bool FromRecord(const Record &r)
	{
		if (r.ym == -1) {
			if (r.v1 < 1 || r.v1 > static_cast<int64_t>(MAX_ID) + 1 || r.v2 < 0) return false;
			this->next_id = std::max(this->next_id, static_cast<uint32_t>(r.v1));
			this->system_income = r.v2;
			if (r.v3 < 0) return false;
			this->last_month = r.v3 - 1;
			return true;
		}
		if (r.ym < 0) return false;
		const uint32_t id = static_cast<uint32_t>(r.ym) & MAX_ID;
		const uint32_t part = static_cast<uint32_t>(r.ym) >> 24;
		if (id == 0 || part > 1) return false;

		if (part == 1) {
			Contract *c = this->Get(id);
			if (c == nullptr) return false;
			c->ident = r.v1;
			c->built = static_cast<int32_t>(static_cast<uint32_t>(r.v2 & 0xFFFFFFFF));
			c->seller_year = static_cast<int32_t>((r.v2 >> 32) & 0xFFFF);
			if (r.v3 >= 0) c->fine_debt = r.v3; // an older savegame has 0 here
			return true;
		}

		if (this->rows.count(id) != 0) return false;
		if (r.a >= MAX_COMPANIES_N || (r.b >= MAX_COMPANIES_N && r.b != SYSTEM)) return false;

		Contract c;
		c.id = id;
		c.buyer = r.a;
		c.seller = r.b;
		c.industry = static_cast<uint16_t>(r.v1 & 0xFFFF);
		c.cargo = static_cast<uint8_t>((r.v1 >> 16) & 0xFF);
		const uint8_t status = static_cast<uint8_t>((r.v1 >> 24) & 0xFF);
		if (!IsValidStatus(status) || IsClosed(static_cast<Status>(status))) return false;
		c.status = static_cast<Status>(status);
		c.round = static_cast<uint8_t>((r.v1 >> 32) & 0xFF);
		const uint8_t turn = static_cast<uint8_t>((r.v1 >> 40) & 0xFF);
		if (c.round < 1 || c.round > MAX_ROUNDS || turn > 1) return false;
		c.turn = static_cast<Side>(turn);
		c.miss_streak = static_cast<uint8_t>((r.v1 >> 48) & 0xFF);
		c.months_total = static_cast<uint8_t>((r.v1 >> 56) & 0x7F);
		c.terms.qty = static_cast<int64_t>(r.v2 & 0xFFFFFFFF);
		c.months_left = static_cast<uint8_t>((r.v2 >> 32) & 0xFF);
		c.buyer_year = static_cast<int32_t>((r.v2 >> 40) & 0xFFFF);
		c.terms.price_cents = r.v3;
		c.terms.months = c.months_total;
		c.escrow = r.v4;
		c.delivered = static_cast<int64_t>(r.v5 & 0xFFFFFFFF);
		c.expires_day = r.v5 >> 32;
		if (CheckTerms(c.terms) != Result::Ok) return false;
		if (c.escrow < 0 || c.expires_day < 0) return false;
		if (c.status == Status::Active) {
			if (c.months_left < 1 || c.months_left > c.months_total) return false;
		} else if (c.escrow != 0) {
			return false; // An open contract holds no money.
		}
		this->rows[id] = c;
		this->next_id = std::max(this->next_id, id + 1);
		return true;
	}
};

/* ---- Step 4: the automatic answer of the SYSTEM (the NPC that owns every industry nobody bought). DECISIONS of the designer: the spec text is not in the repo ---- */
static constexpr int AUTO_ACCEPT_PERCENT = 95;       ///< Round 1 / 2: a price of at least 95% of the market price is accepted at once.
static constexpr int AUTO_COUNTER_MIN_PERCENT = 80;  ///< SPEC step 4: below 80% of the market price the NPC refuses; from 80% to 95% it counters at a price BETWEEN the offer and the market price.
static constexpr int HUMAN_OWNER_ANSWER_DAYS = 3;    ///< SPEC step 4: an owner that is a company has 3 game days to answer; then the system answers for it with the same rule.
static constexpr int NO_CONTRACT_PAY_PERCENT = 70;   ///< SPEC step 5: cargo from an industry that has an owner, hauled without a contract, pays 70% of the normal pay.
static constexpr int AUTO_LAST_ROUND_PERCENT = 90;   ///< In the last round (only accept or refuse) the NPC accepts from 90% of the market price.
static constexpr int AUTO_MIN_DELAY_DAYS = 1;        ///< The NPC answers after 1 .. 3 game days (so a window shows 'waiting' for a moment).
static constexpr int AUTO_SPREAD_DAYS = 3;

/**
 * Step 5: the pay of one final delivery of `count` units. `normal` = what the game pays for all `count` units. `contract_units` of them are on an ACTIVE contract
 * and pay `contract_value` (money units, from Table::AddDelivery). The other units pay the normal rate, and only NO_CONTRACT_PAY_PERCENT of it when the industry
 * has an owner (a company); an industry of the SYSTEM pays the normal rate. Integer maths, no overflow for normal below 2^62 / 100.
 */
inline int64_t DeliveryProfit(int64_t normal, int64_t count, int64_t contract_units, int64_t contract_value, bool industry_owned)
{
	if (count <= 0) return normal;
	contract_units = std::clamp<int64_t>(contract_units, 0, count);
	const int64_t rest_units = count - contract_units;
	int64_t rest = rest_units == count ? normal : (normal / count) * rest_units + (normal % count) * rest_units / count;
	if (industry_owned) rest = rest / 100 * NO_CONTRACT_PAY_PERCENT + (rest % 100) * NO_CONTRACT_PAY_PERCENT / 100;
	return std::max<int64_t>(0, contract_value) + rest;
}

enum class AutoKind : uint8_t { Wait, Accept, Refuse, Counter };

/** The decision of the NPC for one contract. `terms` is only used for Counter. */
struct AutoAnswer {
	AutoKind kind = AutoKind::Wait;
	Terms terms{};
};

/** Game days after the last move before the NPC answers: 1 .. 3, from the id and the round (no random numbers: every client gets the same). */
inline int AutoDelayDays(const Contract &c)
{
	return AUTO_MIN_DELAY_DAYS + static_cast<int>((static_cast<uint64_t>(c.id) + c.round) % AUTO_SPREAD_DAYS);
}

/** The game day of the last move of an open contract (every move gives 7 days, see OFFER_EXPIRY_DAYS). */
inline int64_t LastMoveDay(const Contract &c) { return c.expires_day - OFFER_EXPIRY_DAYS; }

/**
 * Is it the turn of the seller, and is the delay over? SYSTEM answers after 1 .. 3 days (AutoDelayDays). A seller that is a COMPANY has HUMAN_OWNER_ANSWER_DAYS (3) game days
 * to answer itself; when they are over the system answers for it (spec step 4).
 */
inline bool AutoDue(const Contract &c, uint8_t current_seller, int64_t today)
{
	if (!IsOpen(c.status) || c.turn != Side::Seller) return false;
	if (today > c.expires_day) return false; // Too late: the daily sweep expires it.
	const int delay = current_seller == SYSTEM ? AutoDelayDays(c) : HUMAN_OWNER_ANSWER_DAYS;
	return today >= LastMoveDay(c) + delay;
}

/**
 * What the NPC does with an open contract that waits for it.
 * @param ref_price_cents Market price of one unit now, in 1/100 of a money unit (the game measures it); 0 or less = unknown, the NPC refuses.
 * Rules (spec step 4): price >= 95% of market -> accept; 80% .. 95% -> counter at the price BETWEEN the offer and the market price (same amount and length); below 80% -> refuse.
 * In the last round (3) nobody may counter: accept from 90% (DECISION, the spec is silent), else refuse.
 */
inline AutoAnswer AutoDecide(const Contract &c, int64_t ref_price_cents)
{
	AutoAnswer a;
	if (!IsOpen(c.status) || c.turn != Side::Seller) return a; // Wait
	if (ref_price_cents <= 0) { a.kind = AutoKind::Refuse; return a; }
	const int64_t price100 = c.terms.price_cents * 100; // price_cents <= MAX_PRICE_CENTS (1e9): no overflow.
	if (price100 >= ref_price_cents * AUTO_ACCEPT_PERCENT) { a.kind = AutoKind::Accept; return a; }
	if (c.round >= MAX_ROUNDS) {
		a.kind = price100 >= ref_price_cents * AUTO_LAST_ROUND_PERCENT ? AutoKind::Accept : AutoKind::Refuse;
		return a;
	}
	if (price100 < ref_price_cents * AUTO_COUNTER_MIN_PERCENT) { a.kind = AutoKind::Refuse; return a; }
	a.kind = AutoKind::Counter;
	a.terms = c.terms;
	a.terms.price_cents = std::clamp<int64_t>((c.terms.price_cents + ref_price_cents) / 2, 1, MAX_PRICE_CENTS); // half way between the offer and the market price
	if (a.terms == c.terms) a.kind = AutoKind::Accept; // Cannot happen below 95%, kept as a safety net against a SameTerms loop.
	return a;
}

} // namespace contract

#endif /* CONTRACT_MATH_H */
