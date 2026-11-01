/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file industry_owner_math.h Owners of industries: the table and the price of ownership, integer maths only and no game headers, so that industry_owner_test.cpp
 * can test it alone. OpenTTD has no owner of an industry (Industry::owner stays OWNER_NONE), so the mod keeps its own table. An industry that is not in the
 * table belongs to the SYSTEM (an NPC that answers contracts by itself); a company can buy the right of ownership.
 *
 * An industry is known by its pool index, but the game re-uses an index after an industry is closed. So a row also keeps the IDENTITY of the industry it was made for
 * (type, tile, construction date): a row whose identity does not match the industry now standing at that index is stale and counts as not there.
 * The same for the owner: a row also keeps the year in which the owning company was founded, so a new company that gets the slot of a bankrupt one does not inherit its industries.
 */

#ifndef INDUSTRY_OWNER_MATH_H
#define INDUSTRY_OWNER_MATH_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace indowner {

static constexpr int64_t PRICE_MONTHS = 12;          ///< Price of ownership = output per month x market price x 12 (pays back in about one year).
static constexpr int64_t PRICE_FLOOR = 20'000;       ///< An industry that has not produced yet (new) or produces very little still costs at least this (money units).
static constexpr uint8_t SYSTEM = 0xFF;              ///< The owner code of the system (NPC). Nobody can be a company with this number.
static constexpr uint8_t MAX_COMPANIES_N = 15;       ///< Companies are 0 .. 14 (MAX_COMPANIES of OpenTTD).
static constexpr uint8_t MAX_LEVEL = 4;              ///< Upgrade levels 0 .. 4 (100%, 125%, 150%, 200%, 300%): used by the upgrades of step 12; every row starts at 0.
static constexpr int32_t SUMMARY_KEY = -1;           ///< ym of the record that holds the totals.
static constexpr int32_t MAX_INDUSTRY_INDEX = 0xFFFE;

/** What identifies the industry that a row was made for. */
struct Identity {
	uint8_t type = 0;
	uint32_t tile = 0;
	int32_t built = 0; ///< Construction date (days).
	bool operator==(const Identity &o) const { return type == o.type && tile == o.tile && built == o.built; }
	bool operator!=(const Identity &o) const { return !(*this == o); }
};

/** One owned industry. */
struct Row {
	uint8_t owner = SYSTEM;   ///< Company number.
	uint8_t level = 0;        ///< Upgrade level, 0 .. MAX_LEVEL.
	int64_t acquired_day = 0; ///< Game day on which the right was bought.
	int64_t price_paid = 0;   ///< What it cost (money units).
	int32_t owner_year = 0;   ///< Economy year in which the owning company was founded.
	Identity id{};
};

/** What happens when a company tries to buy the right of ownership. */
enum class BuyResult : uint8_t {
	Ok,
	NotForSale,   ///< The industry produces nothing (a bank, a power station ...): there is no output to own.
	AlreadyYours, ///< The company owns it already.
	OwnedByOther, ///< Another company owns it (selling between companies is not part of step 1).
	NoCash,       ///< The unborrowed cash is below the price.
};

/** Why a row is dropped by a sweep. */
enum class Verdict : uint8_t { Keep, IndustryGone, OwnerGone };

struct SweepResult {
	int industry_gone = 0;
	int owner_gone = 0;
	int Total() const { return this->industry_gone + this->owner_gone; }
};

/** The saved form of one row (the game puts it in a StockRecord with kind SRK_INDUSTRY_OWNER). */
struct Record {
	int32_t ym = 0;  ///< Index of the industry (SUMMARY_KEY for the totals).
	uint8_t a = 0;   ///< Owner (company number).
	uint8_t b = 0;   ///< Upgrade level.
	int64_t v1 = 0;  ///< Game day of the purchase (summary: total paid to the system).
	int64_t v2 = 0;  ///< Price paid.
	int64_t v3 = 0;  ///< tile | type << 32.
	int64_t v4 = 0;  ///< Construction date.
	int64_t v5 = 0;  ///< Founding year of the owner.
};

/** a * b, saturating at INT64_MAX, for non-negative values. */
inline int64_t SaturatingMul(int64_t a, int64_t b)
{
	if (a <= 0 || b <= 0) return 0;
	if (a > INT64_MAX / b) return INT64_MAX;
	return a * b;
}

/**
 * The price of the right of ownership of an industry.
 * @param monthly_value What one month of output is worth: the sum over the cargo of (units per month x market price of one unit), money units.
 * @return monthly_value x 12, but never below PRICE_FLOOR.
 */
inline int64_t OwnershipPrice(int64_t monthly_value)
{
	return std::max<int64_t>(PRICE_FLOOR, SaturatingMul(std::max<int64_t>(monthly_value, 0), PRICE_MONTHS));
}

/** The table of owners. Rows are kept in a std::map: the order of a walk is the same on every client. */
struct Table {
	std::map<uint32_t, Row> rows; ///< Key = pool index of the industry.
	int64_t system_income = 0;    ///< Everything that was paid to the system for ownership (money units).

	void Reset()
	{
		this->rows.clear();
		this->system_income = 0;
	}

	/** The row of this industry, but only when it was made for the industry that stands there now. */
	const Row *Find(uint32_t industry, const Identity &id) const
	{
		const auto it = this->rows.find(industry);
		if (it == this->rows.end() || it->second.id != id) return nullptr;
		return &it->second;
	}

	/** The owner: a company number, or SYSTEM when there is no row (or a stale one). */
	uint8_t OwnerOf(uint32_t industry, const Identity &id) const
	{
		const Row *r = this->Find(industry, id);
		return r == nullptr ? SYSTEM : r->owner;
	}

	int Level(uint32_t industry, const Identity &id) const
	{
		const Row *r = this->Find(industry, id);
		return r == nullptr ? 0 : r->level;
	}

	/** Would the purchase work? `for_sale` is false for an industry without output. Pure: changes nothing. */
	BuyResult CanBuy(uint32_t industry, const Identity &id, uint8_t buyer, int64_t unborrowed_cash, int64_t price, bool for_sale) const
	{
		if (!for_sale || price <= 0) return BuyResult::NotForSale;
		const uint8_t owner = this->OwnerOf(industry, id);
		if (owner == buyer) return BuyResult::AlreadyYours;
		if (owner != SYSTEM) return BuyResult::OwnedByOther;
		if (unborrowed_cash < price) return BuyResult::NoCash;
		return BuyResult::Ok;
	}

	/** Do the purchase when it is allowed (same checks as CanBuy). The money itself is moved by the caller. A stale row is replaced. */
	BuyResult Buy(uint32_t industry, const Identity &id, uint8_t buyer, int32_t buyer_year, int64_t unborrowed_cash, int64_t price, bool for_sale, int64_t today)
	{
		const BuyResult r = this->CanBuy(industry, id, buyer, unborrowed_cash, price, for_sale);
		if (r != BuyResult::Ok) return r;
		Row row;
		row.owner = buyer;
		row.level = 0;
		row.acquired_day = today;
		row.price_paid = price;
		row.owner_year = buyer_year;
		row.id = id;
		this->rows[industry] = row;
		this->system_income += price;
		return BuyResult::Ok;
	}

	/** Give the industries of a company back to the system (the company is gone). Returns how many. */
	int ReleaseOwner(uint8_t owner)
	{
		int n = 0;
		for (auto it = this->rows.begin(); it != this->rows.end();) {
			if (it->second.owner == owner) {
				it = this->rows.erase(it);
				n++;
			} else {
				++it;
			}
		}
		return n;
	}

	/** Drop the rows that are no longer true. `check(industry, row)` returns a Verdict. An industry that was closed loses its row; a company that is gone loses its industries to the system. */
	template <typename F>
	SweepResult Sweep(F &&check)
	{
		SweepResult out;
		for (auto it = this->rows.begin(); it != this->rows.end();) {
			const Verdict v = check(it->first, it->second);
			if (v == Verdict::Keep) {
				++it;
				continue;
			}
			if (v == Verdict::IndustryGone) out.industry_gone++; else out.owner_gone++;
			it = this->rows.erase(it);
		}
		return out;
	}

	/** The rows as records for the savegame, in the order of the industry index, with the totals first. */
	std::vector<Record> ToRecords() const
	{
		std::vector<Record> out;
		Record s;
		s.ym = SUMMARY_KEY;
		s.v1 = this->system_income;
		out.push_back(s);
		for (const auto &kv : this->rows) {
			const Row &r = kv.second;
			Record rec;
			rec.ym = static_cast<int32_t>(kv.first);
			rec.a = r.owner;
			rec.b = r.level;
			rec.v1 = r.acquired_day;
			rec.v2 = r.price_paid;
			rec.v3 = static_cast<int64_t>(r.id.tile) | (static_cast<int64_t>(r.id.type) << 32);
			rec.v4 = r.id.built;
			rec.v5 = r.owner_year;
			out.push_back(rec);
		}
		return out;
	}

	/** Read one record of a savegame. A record that makes no sense is ignored (returns false). */
	bool FromRecord(const Record &rec)
	{
		if (rec.ym == SUMMARY_KEY) {
			this->system_income = std::max<int64_t>(rec.v1, 0);
			return true;
		}
		if (rec.ym < 0 || rec.ym > MAX_INDUSTRY_INDEX || rec.a >= MAX_COMPANIES_N) return false;
		Row r;
		r.owner = rec.a;
		r.level = std::min<uint8_t>(rec.b, MAX_LEVEL);
		r.acquired_day = std::max<int64_t>(rec.v1, 0);
		r.price_paid = std::max<int64_t>(rec.v2, 0);
		r.id.tile = static_cast<uint32_t>(rec.v3 & 0xFFFFFFFF);
		r.id.type = static_cast<uint8_t>((rec.v3 >> 32) & 0xFF);
		r.id.built = static_cast<int32_t>(rec.v4);
		r.owner_year = static_cast<int32_t>(rec.v5);
		this->rows[static_cast<uint32_t>(rec.ym)] = r;
		return true;
	}
};

} // namespace indowner

#endif /* INDUSTRY_OWNER_MATH_H */
