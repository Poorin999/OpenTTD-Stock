/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file contract_test.cpp Test of contract_math.h, alone (no game needed).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o contract_test contract_test.cpp && ./contract_test */

#include <cstdio>
#include "contract_math.h"

using namespace contract;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(expr) do { g_checks++; if (!(expr)) { g_failed++; std::printf("FAIL line %d: %s\n", __LINE__, #expr); } } while (0)

static constexpr int64_t CASH = 1000000;
static constexpr int64_t TODAY = 1000;

static Deal MakeDeal(uint8_t buyer = 1, uint8_t seller = SYSTEM, uint16_t industry = 7, uint8_t cargo = 3)
{
	Deal d;
	d.buyer = buyer;
	d.buyer_year = 1950;
	d.seller = seller;
	d.seller_year = seller == SYSTEM ? 0 : 1951;
	d.industry = industry;
	d.ident = 123456789012345LL;
	d.built = 40000;
	d.cargo = cargo;
	return d;
}

static Terms T(int64_t qty, int64_t price, int64_t months) { return Terms{qty, price, months}; }

/** Make an active contract of `qty` for buyer `buyer` on industry 7 (output 200). */
static uint32_t MakeActive(Table &t, uint8_t buyer, int64_t qty, int64_t output = 200)
{
	uint32_t id = 0;
	Deal d = MakeDeal(buyer);
	if (t.Offer(d, T(qty, 500, 6), output, CASH, TODAY, id) != Result::Ok) return 0;
	if (t.Accept(id, Side::Seller, output, CASH, TODAY) != Result::Ok) return 0;
	return id;
}

static void TestPureRules()
{
	CHECK(MonthValue(80, 500) == 400);          // 80 units at 5.00
	CHECK(MonthValue(3, 150) == 4);             // 4.5 -> 4, rounded down
	CHECK(EscrowOf(T(80, 500, 6)) == 400);
	CHECK(FreeOf(200, 112) == 88);
	CHECK(FreeOf(100, 150) == 0);               // never negative
	CHECK(BuyerCapOf(200) == 120);
	CHECK(BuyerCapOf(99) == 59);                // rounded down
	CHECK(CheckTerms(T(1, 1, 1)) == Result::Ok);
	CHECK(CheckTerms(T(0, 1, 1)) == Result::BadQty);
	CHECK(CheckTerms(T(-5, 1, 1)) == Result::BadQty);
	CHECK(CheckTerms(T(MAX_QTY + 1, 1, 1)) == Result::BadQty);
	CHECK(CheckTerms(T(1, 0, 1)) == Result::BadPrice);
	CHECK(CheckTerms(T(1, MAX_PRICE_CENTS + 1, 1)) == Result::BadPrice);
	CHECK(CheckTerms(T(1, 1, 0)) == Result::BadMonths);
	CHECK(CheckTerms(T(1, 1, MAX_MONTHS)) == Result::Ok);
	CHECK(CheckTerms(T(1, 1, MAX_MONTHS + 1)) == Result::BadMonths);
	CHECK(IsOpen(Status::Offered) && IsOpen(Status::Countered) && !IsOpen(Status::Active));
	CHECK(IsClosed(Status::Refused) && IsClosed(Status::Cancelled) && IsClosed(Status::Expired) && IsClosed(Status::Completed) && !IsClosed(Status::Active));
	CHECK(IsValidStatus(6) && !IsValidStatus(7));
	CHECK(Other(Side::Buyer) == Side::Seller && Other(Side::Seller) == Side::Buyer);
	CHECK(CheckCapacity(80, 200, 112, 0) == Result::Ok);
	CHECK(CheckCapacity(90, 200, 112, 0) == Result::ExceedsFree);
	CHECK(CheckCapacity(10, 0, 0, 0) == Result::NotProducing);
	CHECK(CheckCapacity(61, 100, 0, 0) == Result::ExceedsShare);
	CHECK(CheckCapacity(60, 100, 0, 0) == Result::Ok);
	CHECK(CheckCapacity(30, 100, 0, 40) == Result::ExceedsShare); // 40 + 30 > 60
}

/** Spec case: offer 80 t when 88 t are free -> ok, books 80, 8 left. Offer 90 -> refused, no money. */
static void TestOfferAndReserve()
{
	Table t;
	uint32_t other = MakeActive(t, 2, 112, 200 * 2); // another buyer booked 112 (output 400 makes the cap 240)
	CHECK(other == 1);
	/* The industry now has an output of 200 again; 112 are booked by buyer 2, so 88 are free. */
	uint32_t id = 0;
	CHECK(t.FreeOutput(7, MakeDeal().ident, 40000, 3, 200) == 88);
	CHECK(t.Offer(MakeDeal(1), T(90, 500, 6), 200, CASH, TODAY, id) == Result::ExceedsFree);
	CHECK(t.rows.size() == 1);                       // nothing was added
	CHECK(t.Offer(MakeDeal(1), T(80, 500, 6), 200, CASH, TODAY, id) == Result::Ok);
	CHECK(id == 2);
	const Contract *c = t.Get(id);
	CHECK(c != nullptr && c->status == Status::Offered && c->round == 1 && c->turn == Side::Seller);
	CHECK(c != nullptr && c->expires_day == TODAY + OFFER_EXPIRY_DAYS && c->escrow == 0);
	CHECK(t.FreeOutput(7, MakeDeal().ident, 40000, 3, 200) == 88); // an offer books nothing yet
	CHECK(t.Accept(id, Side::Seller, 200, CASH, TODAY + 1) == Result::Ok);
	c = t.Get(id);
	CHECK(c->status == Status::Active && c->escrow == 400 && c->months_left == 6 && c->miss_streak == 0 && c->delivered == 0);
	CHECK(t.FreeOutput(7, MakeDeal().ident, 40000, 3, 200) == 8);  // 80 booked, 8 free
	CHECK(t.Reserved(7, MakeDeal().ident, 40000, 3, 1, 1950) == 80);
	CHECK(t.Reserved(7, MakeDeal().ident, 40000, 3, 2, 1950) == 112);
	CHECK(t.Reserved(7, MakeDeal().ident, 40000, 4) == 0);         // another cargo
	CHECK(t.Reserved(8, MakeDeal().ident, 40000, 3) == 0);         // another industry
	CHECK(t.Reserved(7, MakeDeal().ident, 40001, 3) == 0);         // same index, built again later = another industry
	CHECK(t.TotalEscrow() == 400 + 112 * 500 / 100);
	CHECK(t.TotalEscrow(1, 1950) == 400);
}

static void TestOfferChecks()
{
	Table t;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(1), T(0, 500, 6), 200, CASH, TODAY, id) == Result::BadQty);
	CHECK(t.Offer(MakeDeal(1), T(10, 0, 6), 200, CASH, TODAY, id) == Result::BadPrice);
	CHECK(t.Offer(MakeDeal(1), T(10, 500, 0), 200, CASH, TODAY, id) == Result::BadMonths);
	CHECK(t.Offer(MakeDeal(1, 1), T(10, 500, 6), 200, CASH, TODAY, id) == Result::SelfDeal);       // the buyer owns the industry
	CHECK(t.Offer(MakeDeal(1), T(10, 500, 6), 0, CASH, TODAY, id) == Result::NotProducing);
	CHECK(t.Offer(MakeDeal(1), T(130, 500, 6), 200, CASH, TODAY, id) == Result::ExceedsShare);     // cap = 120
	CHECK(t.Offer(MakeDeal(1), T(120, 500, 6), 200, CASH, TODAY, id) == Result::Ok);               // exactly the cap
	CHECK(t.Offer(MakeDeal(1), T(10, 500, 6), 200, CASH, TODAY, id) == Result::DuplicateOpen);     // one open offer of this buyer for this thing
	CHECK(t.rows.size() == 1);
	/* No cash for the escrow: 120 x 5.00 = 600. */
	Table u;
	CHECK(u.Offer(MakeDeal(1), T(120, 500, 6), 200, 599, TODAY, id) == Result::NoCash);
	CHECK(u.rows.empty());
	CHECK(u.Offer(MakeDeal(1), T(120, 500, 6), 200, 600, TODAY, id) == Result::Ok);
	CHECK(u.next_id == 2);
	/* A full table of one buyer. */
	Table w;
	for (int i = 0; i < MAX_LIVE_PER_BUYER; i++) CHECK(w.Offer(MakeDeal(1, SYSTEM, static_cast<uint16_t>(100 + i)), T(10, 100, 1), 200, CASH, TODAY, id) == Result::Ok);
	CHECK(w.Offer(MakeDeal(1, SYSTEM, 999), T(10, 100, 1), 200, CASH, TODAY, id) == Result::TooMany);
	CHECK(w.Offer(MakeDeal(2, SYSTEM, 999), T(10, 100, 1), 200, CASH, TODAY, id) == Result::Ok); // another buyer is not blocked
	/* The id space. */
	Table x;
	x.next_id = MAX_ID + 1;
	CHECK(x.Offer(MakeDeal(1), T(10, 100, 1), 200, CASH, TODAY, id) == Result::TooMany);
}

/** Round 1 buyer, round 2 seller, round 3 buyer; then only accept or refuse. */
static void TestNegotiation()
{
	Table t;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(1, 4), T(80, 400, 6), 200, CASH, TODAY, id) == Result::Ok);
	/* The buyer may not answer his own offer. */
	CHECK(t.Counter(id, Side::Buyer, T(70, 400, 6), 200, CASH, TODAY) == Result::NotYourTurn);
	CHECK(t.Accept(id, Side::Buyer, 200, CASH, TODAY) == Result::NotYourTurn);
	CHECK(t.Refuse(id, Side::Buyer, TODAY) == Result::NotYourTurn);
	/* Round 2: the seller wants a higher price. */
	CHECK(t.Counter(id, Side::Seller, T(80, 400, 6), 200, CASH, TODAY + 1) == Result::SameTerms);
	CHECK(t.Counter(id, Side::Seller, T(80, 0, 6), 200, CASH, TODAY + 1) == Result::BadPrice);
	CHECK(t.Counter(id, Side::Seller, T(150, 460, 6), 200, CASH, TODAY + 1) == Result::ExceedsShare);
	CHECK(t.Counter(id, Side::Seller, T(80, 460, 6), 200, CASH, TODAY + 2) == Result::Ok);
	const Contract *c = t.Get(id);
	CHECK(c->status == Status::Countered && c->round == 2 && c->turn == Side::Buyer && c->terms.price_cents == 460 && c->expires_day == TODAY + 2 + OFFER_EXPIRY_DAYS);
	/* Round 3: the buyer meets in the middle. He needs cash for the escrow of his terms. */
	CHECK(t.Counter(id, Side::Buyer, T(80, 430, 6), 200, 100, TODAY + 3) == Result::NoCash);
	CHECK(t.Counter(id, Side::Buyer, T(80, 430, 6), 200, CASH, TODAY + 3) == Result::Ok);
	c = t.Get(id);
	CHECK(c->round == 3 && c->turn == Side::Seller && c->status == Status::Countered);
	/* The 3 rounds are used: no fourth move, only accept or refuse. */
	CHECK(t.Counter(id, Side::Seller, T(80, 440, 6), 200, CASH, TODAY + 4) == Result::TooManyRounds);
	CHECK(t.Accept(id, Side::Seller, 200, CASH, TODAY + 4) == Result::Ok);
	CHECK(t.Get(id)->status == Status::Active && t.Get(id)->escrow == 80 * 430 / 100 && t.Get(id)->terms.price_cents == 430);
	/* An active contract can not be negotiated or refused any more. */
	CHECK(t.Counter(id, Side::Seller, T(80, 440, 6), 200, CASH, TODAY + 5) == Result::NotOpen);
	CHECK(t.Refuse(id, Side::Seller, TODAY + 5) == Result::NotOpen);
	CHECK(t.Accept(id, Side::Seller, 200, CASH, TODAY + 5) == Result::NotOpen);
	CHECK(t.Counter(9999, Side::Seller, T(1, 1, 1), 200, CASH, TODAY) == Result::NoSuchContract);
}

/** Spec case: 3 rounds without agreement -> the contract expires, no money taken. */
static void TestThreeRoundsExpire()
{
	Table t;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(1, 4), T(80, 400, 6), 200, CASH, TODAY, id) == Result::Ok);
	CHECK(t.Counter(id, Side::Seller, T(80, 500, 6), 200, CASH, TODAY + 1) == Result::Ok);
	CHECK(t.Counter(id, Side::Buyer, T(80, 450, 6), 200, CASH, TODAY + 2) == Result::Ok);
	/* Nobody answers round 3 within 7 days. */
	std::vector<uint32_t> closed;
	CHECK(t.ExpireOpen(TODAY + 2 + OFFER_EXPIRY_DAYS, &closed) == 0); // the last day still counts
	CHECK(t.ExpireOpen(TODAY + 3 + OFFER_EXPIRY_DAYS, &closed) == 1);
	CHECK(closed.size() == 1 && closed[0] == id);
	CHECK(t.Get(id)->status == Status::Expired && t.Get(id)->escrow == 0 && t.Get(id)->closed_day == TODAY + 3 + OFFER_EXPIRY_DAYS);
	CHECK(t.Accept(id, Side::Seller, 200, CASH, TODAY + 20) == Result::AlreadyClosed);
	CHECK(t.TotalEscrow() == 0 && t.Reserved(7, MakeDeal().ident, 40000, 3) == 0);
	/* An offer that is too old can not be accepted even before the sweep has run. */
	uint32_t id2 = 0;
	CHECK(t.Offer(MakeDeal(1, 4), T(80, 400, 6), 200, CASH, TODAY + 30, id2) == Result::Ok); // the closed one does not count as an open duplicate
	CHECK(t.Accept(id2, Side::Seller, 200, CASH, TODAY + 30 + OFFER_EXPIRY_DAYS + 1) == Result::TimedOut);
	CHECK(t.Counter(id2, Side::Seller, T(70, 400, 6), 200, CASH, TODAY + 30 + OFFER_EXPIRY_DAYS + 1) == Result::TimedOut);
	CHECK(t.Get(id2)->status == Status::Offered); // a failed move changes nothing
}

static void TestRefuseAndWithdraw()
{
	Table t;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(1, 4), T(80, 400, 6), 200, CASH, TODAY, id) == Result::Ok);
	CHECK(t.Refuse(id, Side::Seller, TODAY + 1) == Result::Ok);
	CHECK(t.Get(id)->status == Status::Refused && t.Get(id)->escrow == 0);
	CHECK(t.Refuse(id, Side::Seller, TODAY + 1) == Result::AlreadyClosed);
	CHECK(t.Offer(MakeDeal(1, 4), T(80, 400, 6), 200, CASH, TODAY + 2, id) == Result::Ok); // a refused offer is not a duplicate
	/* The buyer withdraws his own open offer. */
	Settlement s;
	CHECK(t.Cancel(id, Side::Buyer, TODAY + 3, s) == Result::Ok);
	CHECK(s.to_buyer == 0 && s.to_seller == 0 && t.Get(id)->status == Status::Cancelled);
	CHECK(t.Cancel(id, Side::Buyer, TODAY + 3, s) == Result::AlreadyClosed);
	CHECK(t.Cancel(4242, Side::Buyer, TODAY, s) == Result::NoSuchContract);
}

/** Spec case: the output fell after the offer; accepting is checked again. A buyer with too little cash is refused with the reason. */
static void TestAcceptChecksAgain()
{
	Table t;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(1, 4), T(60, 500, 6), 200, CASH, TODAY, id) == Result::Ok);
	CHECK(t.Accept(id, Side::Seller, 50, CASH, TODAY) == Result::ExceedsFree);   // the output fell to 50: 60 > 50
	CHECK(t.Get(id)->status == Status::Offered);
	CHECK(t.Accept(id, Side::Seller, 0, CASH, TODAY) == Result::NotProducing);
	CHECK(t.Accept(id, Side::Seller, 100, CASH, TODAY) == Result::Ok);           // 60% of 100 = 60: exactly the share
}

static void TestAcceptShare()
{
	Table t;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(1, 4), T(60, 500, 6), 100, CASH, TODAY, id) == Result::Ok);
	CHECK(t.Accept(id, Side::Seller, 99, CASH, TODAY) == Result::ExceedsShare); // cap of 99 is 59 < 60
	CHECK(t.Accept(id, Side::Seller, 100, 299, TODAY) == Result::NoCash);       // escrow is 60 x 5.00 = 300
	CHECK(t.Get(id)->status == Status::Offered && t.Get(id)->escrow == 0);      // refused: nothing taken
	CHECK(t.Accept(id, Side::Seller, 100, 300, TODAY) == Result::Ok);
	CHECK(t.Get(id)->escrow == 300);
	/* Two offers that each fit when made; the second does not fit after the first was accepted. */
	Table u;
	uint32_t a = 0, b = 0;
	CHECK(u.Offer(MakeDeal(1, 4), T(60, 500, 6), 100, CASH, TODAY, a) == Result::Ok);
	CHECK(u.Offer(MakeDeal(2, 4), T(60, 500, 6), 100, CASH, TODAY, b) == Result::Ok);
	CHECK(u.Accept(a, Side::Seller, 100, CASH, TODAY) == Result::Ok);
	CHECK(u.Accept(b, Side::Seller, 100, CASH, TODAY) == Result::ExceedsFree);  // only 40 free now
	CHECK(u.Counter(b, Side::Seller, T(40, 500, 6), 100, CASH, TODAY) == Result::Ok);
	CHECK(u.Accept(b, Side::Buyer, 100, CASH, TODAY) == Result::Ok);
	CHECK(u.FreeOutput(7, MakeDeal().ident, 40000, 3, 100) == 0);
}

/** Cancel of an active contract: early = escrow to the seller; after 2 missed months in a row = back to the buyer; the seller can not cancel. */
static void TestCancelActive()
{
	Table t;
	const uint32_t id = MakeActive(t, 1, 80);
	CHECK(id != 0);
	Settlement s;
	CHECK(t.Cancel(id, Side::Seller, TODAY + 5, s) == Result::NotAllowed);
	CHECK(t.Get(id)->status == Status::Active && t.Get(id)->escrow == 400);
	CHECK(!CanCancelFree(*t.Get(id)));
	CHECK(t.Cancel(id, Side::Buyer, TODAY + 5, s) == Result::Ok);
	CHECK(s.to_buyer == 0 && s.to_seller == 400);                  // early: the escrow is the fine
	CHECK(t.Get(id)->status == Status::Cancelled && t.Get(id)->escrow == 0);
	CHECK(t.Reserved(7, MakeDeal().ident, 40000, 3) == 0);          // the reservation is released
	CHECK(t.TotalEscrow() == 0);

	const uint32_t id2 = MakeActive(t, 1, 80);
	t.Get(id2)->miss_streak = 1;
	CHECK(!CanCancelFree(*t.Get(id2)));
	t.Get(id2)->miss_streak = 2;
	CHECK(CanCancelFree(*t.Get(id2)));
	CHECK(t.Cancel(id2, Side::Buyer, TODAY + 9, s) == Result::Ok);
	CHECK(s.to_buyer == 400 && s.to_seller == 0);                  // spec: no fine after 2 missed months in a row
}

/** Spec case: the buyer is bankrupt -> contract cancelled, the escrow goes to the owner of the industry. A bankrupt owner -> the contract goes to the SYSTEM. */
static void TestBankruptcy()
{
	Table t;
	uint32_t open1 = 0;
	Deal d = MakeDeal(1, 4);
	uint32_t act = 0;
	CHECK(t.Offer(d, T(80, 500, 6), 200, CASH, TODAY, act) == Result::Ok);
	CHECK(t.Accept(act, Side::Seller, 200, CASH, TODAY) == Result::Ok);
	Deal d2 = MakeDeal(1, 4, 8);
	CHECK(t.Offer(d2, T(10, 500, 6), 200, CASH, TODAY, open1) == Result::Ok);
	/* A company that was never involved. */
	Deal d3 = MakeDeal(5, SYSTEM, 9);
	uint32_t other = 0;
	CHECK(t.Offer(d3, T(10, 500, 6), 200, CASH, TODAY, other) == Result::Ok);

	std::vector<Payout> pay;
	/* Buyer 1 goes bankrupt. */
	int n = t.Sweep([](const Contract &c) { return c.buyer == 1 ? Verdict::BuyerGone : Verdict::Keep; }, TODAY + 10, pay);
	CHECK(n == 2);
	CHECK(t.Get(act)->status == Status::Cancelled && t.Get(act)->escrow == 0);
	CHECK(t.Get(open1)->status == Status::Cancelled);
	CHECK(t.Get(other)->status == Status::Offered);
	CHECK(pay.size() == 1 && pay[0].company == 4 && pay[0].amount == 400 && pay[0].contract == act);
	CHECK(t.Reserved(7, d.ident, 40000, 3) == 0);

	/* Owner 4 goes bankrupt: the contracts of the buyer 5 stay, the seller becomes the SYSTEM. */
	Table u;
	uint32_t a = 0;
	CHECK(u.Offer(MakeDeal(5, 4), T(10, 500, 6), 200, CASH, TODAY, a) == Result::Ok);
	CHECK(u.Accept(a, Side::Seller, 200, CASH, TODAY) == Result::Ok);
	pay.clear();
	CHECK(u.Sweep([](const Contract &c) { return c.seller == 4 ? Verdict::SellerGone : Verdict::Keep; }, TODAY + 1, pay) == 1);
	CHECK(u.Get(a)->seller == SYSTEM && u.Get(a)->seller_year == 0 && u.Get(a)->status == Status::Active && u.Get(a)->escrow == 50);
	CHECK(pay.empty());
	/* The buyer of a contract with the SYSTEM goes bankrupt: nobody is paid, the SYSTEM counts it. */
	CHECK(u.Sweep([](const Contract &c) { return c.buyer == 5 ? Verdict::BuyerGone : Verdict::Keep; }, TODAY + 2, pay) == 1);
	CHECK(pay.empty() && u.system_income == 50 && u.Get(a)->status == Status::Cancelled);
	/* The industry is closed: an active contract gives the escrow back to the buyer, an open one expires. */
	Table v;
	uint32_t p = 0, q = 0;
	CHECK(v.Offer(MakeDeal(1, 4, 7), T(10, 500, 6), 200, CASH, TODAY, p) == Result::Ok);
	CHECK(v.Accept(p, Side::Seller, 200, CASH, TODAY) == Result::Ok);
	CHECK(v.Offer(MakeDeal(2, 4, 7), T(10, 500, 6), 200, CASH, TODAY, q) == Result::Ok);
	pay.clear();
	CHECK(v.Sweep([](const Contract &) { return Verdict::IndustryGone; }, TODAY + 3, pay) == 2);
	CHECK(v.Get(p)->status == Status::Cancelled && v.Get(q)->status == Status::Expired);
	CHECK(pay.size() == 1 && pay[0].company == 1 && pay[0].amount == 50);
	/* Closed contracts are not touched by a second sweep. */
	CHECK(v.Sweep([](const Contract &) { return Verdict::IndustryGone; }, TODAY + 4, pay) == 0);
	CHECK(pay.size() == 1);
}

static void TestPurge()
{
	Table t;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(1, 4), T(80, 500, 6), 200, CASH, TODAY, id) == Result::Ok);
	CHECK(t.Refuse(id, Side::Seller, TODAY) == Result::Ok);
	CHECK(t.PurgeClosed(TODAY + CLOSED_KEEP_DAYS) == 0);
	CHECK(t.rows.size() == 1);
	CHECK(t.PurgeClosed(TODAY + CLOSED_KEEP_DAYS + 1) == 1);
	CHECK(t.rows.empty());
	/* A live contract is never purged. */
	const uint32_t act = MakeActive(t, 1, 80);
	CHECK(t.PurgeClosed(TODAY + 100000) == 0 && t.Get(act) != nullptr);
}

static bool SameContract(const Contract &a, const Contract &b)
{
	return a.id == b.id && a.buyer == b.buyer && a.buyer_year == b.buyer_year && a.seller == b.seller && a.seller_year == b.seller_year && a.industry == b.industry &&
			a.ident == b.ident && a.built == b.built && a.cargo == b.cargo && a.terms == b.terms && a.months_total == b.months_total && a.months_left == b.months_left &&
			a.round == b.round && a.turn == b.turn && a.status == b.status && a.escrow == b.escrow && a.delivered == b.delivered && a.miss_streak == b.miss_streak &&
			a.expires_day == b.expires_day;
}

/** Spec case: save and load in the middle of a contract: every value as before, the contract still Active. */
static void TestSaveLoad()
{
	Table t;
	const uint32_t act = MakeActive(t, 1, 80);
	t.Get(act)->delivered = 33;
	t.Get(act)->miss_streak = 1;
	t.Get(act)->months_left = 4;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(2, 4, 12, 5), T(25, 777, 11), 300, CASH, TODAY + 3, id) == Result::Ok);
	CHECK(t.Counter(id, Side::Seller, T(20, 800, 12), 300, CASH, TODAY + 4) == Result::Ok);
	uint32_t closed = 0;
	CHECK(t.Offer(MakeDeal(3, SYSTEM, 13), T(5, 100, 1), 100, CASH, TODAY, closed) == Result::Ok);
	CHECK(t.Refuse(closed, Side::Seller, TODAY) == Result::Ok);
	t.system_income = 12345;

	const std::vector<Record> recs = t.ToRecords();
	/* The totals + 2 live contracts x 2 parts = 5 records; the closed contract is not saved. */
	CHECK(recs.size() == 5);
	Table u;
	for (const Record &r : recs) CHECK(u.FromRecord(r));
	CHECK(u.rows.size() == 2);
	CHECK(u.Get(closed) == nullptr);
	CHECK(SameContract(*t.Get(act), *u.Get(act)));
	CHECK(SameContract(*t.Get(id), *u.Get(id)));
	CHECK(u.Get(act)->status == Status::Active && u.Get(act)->delivered == 33 && u.Get(act)->miss_streak == 1 && u.Get(act)->months_left == 4);
	CHECK(u.Get(id)->status == Status::Countered && u.Get(id)->round == 2 && u.Get(id)->turn == Side::Buyer && u.Get(id)->terms.months == 12);
	CHECK(u.Get(id)->seller == 4 && u.Get(id)->seller_year == 1951 && u.Get(id)->buyer_year == 1950);
	CHECK(u.system_income == 12345);
	CHECK(u.next_id == t.next_id);                  // the closed id is not used again
	CHECK(u.TotalEscrow() == t.TotalEscrow());
	CHECK(u.Reserved(7, MakeDeal().ident, 40000, 3) == 80);
	/* A second round trip gives the same records. */
	const std::vector<Record> again = u.ToRecords();
	CHECK(again.size() == recs.size());
	bool same = again.size() == recs.size();
	for (size_t i = 0; same && i < recs.size(); i++) same = recs[i].ym == again[i].ym && recs[i].a == again[i].a && recs[i].b == again[i].b && recs[i].v1 == again[i].v1 &&
			recs[i].v2 == again[i].v2 && recs[i].v3 == again[i].v3 && recs[i].v4 == again[i].v4 && recs[i].v5 == again[i].v5;
	CHECK(same);
	/* The game keeps going after a load: a new offer gets a fresh id. */
	uint32_t nid = 0;
	CHECK(u.Offer(MakeDeal(6, 4, 20), T(5, 100, 1), 100, CASH, TODAY + 9, nid) == Result::Ok);
	CHECK(nid == t.next_id);
}

/** Spec case: a savegame from before the contracts has no record at all: an empty table. Records that make no sense are ignored. */
static void TestOldSaveAndNonsense()
{
	Table t;
	CHECK(t.rows.empty() && t.next_id == 1 && t.system_income == 0);
	CHECK(t.ToRecords().empty());                   // nothing to write for an empty table
	t.rows[1].status = Status::Refused;             // only a closed contract: still nothing to write
	CHECK(t.ToRecords().empty());

	Table u;
	Record bad;
	bad.ym = 5; bad.a = 1; bad.b = SYSTEM; bad.v1 = (static_cast<int64_t>(Status::Active) << 24) | (1LL << 32); bad.v2 = 80 | (6LL << 32) | (1950LL << 40); bad.v3 = 500;
	bad.v1 |= (6LL << 56); // months_total 6
	CHECK(u.FromRecord(bad));                       // this one is fine (an active contract with months_left 6, escrow 0 is allowed)
	Table w;
	Record r = bad;
	r.a = 15;                                       CHECK(!w.FromRecord(r));   // a company that does not exist
	r = bad; r.b = 77;                              CHECK(!w.FromRecord(r));   // a seller that does not exist
	r = bad; r.ym = 0;                              CHECK(!w.FromRecord(r));   // id 0
	r = bad; r.ym = -7;                             CHECK(!w.FromRecord(r));
	r = bad; r.ym = 5 | (2 << 24);                  CHECK(!w.FromRecord(r));   // part 2 does not exist
	r = bad; r.v1 = (static_cast<int64_t>(Status::Completed) << 24) | (1LL << 32) | (6LL << 56); CHECK(!w.FromRecord(r)); // a closed contract is never saved
	r = bad; r.v1 = (99LL << 24) | (1LL << 32) | (6LL << 56);                                    CHECK(!w.FromRecord(r)); // status 99
	r = bad; r.v1 = (static_cast<int64_t>(Status::Active) << 24) | (9LL << 32) | (6LL << 56);    CHECK(!w.FromRecord(r)); // round 9
	r = bad; r.v1 = (static_cast<int64_t>(Status::Active) << 24) | (0LL << 32) | (6LL << 56);    CHECK(!w.FromRecord(r)); // round 0
	r = bad; r.v2 = 0 | (6LL << 32);                CHECK(!w.FromRecord(r));   // quantity 0
	r = bad; r.v3 = 0;                              CHECK(!w.FromRecord(r));   // price 0
	r = bad; r.v2 = 80 | (9LL << 32);               CHECK(!w.FromRecord(r));   // months_left 9 > months_total 6
	r = bad; r.v2 = 80 | (0LL << 32);               CHECK(!w.FromRecord(r));   // an active contract with no month left
	r = bad; r.v4 = -5;                             CHECK(!w.FromRecord(r));   // negative escrow
	r = bad; r.v1 = (static_cast<int64_t>(Status::Offered) << 24) | (1LL << 32) | (6LL << 56); r.v4 = 100; CHECK(!w.FromRecord(r)); // an open offer that holds money
	Record ident;
	ident.ym = 99 | (1 << 24);                      CHECK(!w.FromRecord(ident)); // identity of a contract that is not there
	CHECK(w.rows.empty());
	Record totals;
	totals.ym = -1; totals.v1 = 0;                  CHECK(!w.FromRecord(totals));
	totals.v1 = 10; totals.v2 = -1;                 CHECK(!w.FromRecord(totals));
	totals.v1 = 10; totals.v2 = 5;                  CHECK(w.FromRecord(totals) && w.next_id == 10 && w.system_income == 5);
	/* The same id twice: the second one is ignored. */
	Table x;
	CHECK(x.FromRecord(bad));
	CHECK(!x.FromRecord(bad));
	CHECK(x.rows.size() == 1);
}

static void TestAutoAnswer()
{
	Table t;
	uint32_t id = 0;
	CHECK(t.Offer(MakeDeal(1), T(50, 500, 6), 200, CASH, TODAY, id) == Result::Ok);
	Contract &c = *t.Get(id);
	const int64_t REF = 500;
	/* Timing: waits 1 .. 3 days after the move, never for a company seller or when it is the buyer's turn. */
	CHECK(LastMoveDay(c) == TODAY);
	const int d = AutoDelayDays(c);
	CHECK(d >= 1 && d <= 3);
	CHECK(!AutoDue(c, SYSTEM, TODAY + d - 1));
	CHECK(AutoDue(c, SYSTEM, TODAY + d));
	/* A company seller has 3 game days, then the system answers for it (spec step 4). */
	CHECK(!AutoDue(c, 2, TODAY + HUMAN_OWNER_ANSWER_DAYS - 1));
	CHECK(AutoDue(c, 2, TODAY + HUMAN_OWNER_ANSWER_DAYS));
	CHECK(!AutoDue(c, 2, TODAY + OFFER_EXPIRY_DAYS + 1));
	CHECK(!AutoDue(c, SYSTEM, TODAY + OFFER_EXPIRY_DAYS + 1));
	c.turn = Side::Buyer;
	CHECK(!AutoDue(c, SYSTEM, TODAY + d));
	CHECK(AutoDecide(c, REF).kind == AutoKind::Wait);
	c.turn = Side::Seller;

	/* Round 1 (spec): 95% accepts, 94% counters half way between offer and market, 80% counters, 79% refuses, unknown market refuses. */
	c.terms.price_cents = 475; CHECK(AutoDecide(c, REF).kind == AutoKind::Accept);
	c.terms.price_cents = 600; CHECK(AutoDecide(c, REF).kind == AutoKind::Accept);
	c.terms.price_cents = 470;
	AutoAnswer a = AutoDecide(c, REF);
	CHECK(a.kind == AutoKind::Counter && a.terms.price_cents == (470 + REF) / 2 && a.terms.qty == 50 && a.terms.months == 6);
	c.terms.price_cents = 400; a = AutoDecide(c, REF);
	CHECK(a.kind == AutoKind::Counter && a.terms.price_cents == 450);
	c.terms.price_cents = 399; CHECK(AutoDecide(c, REF).kind == AutoKind::Refuse);
	c.terms.price_cents = 500; CHECK(AutoDecide(c, 0).kind == AutoKind::Refuse);
	/* A huge market price is clamped. */
	c.terms.price_cents = 1; a = AutoDecide(c, MAX_PRICE_CENTS * 10);
	CHECK(a.kind == AutoKind::Refuse);

	/* Last round: only accept or refuse (90%). */
	c.round = MAX_ROUNDS;
	c.terms.price_cents = 450; CHECK(AutoDecide(c, REF).kind == AutoKind::Accept);
	c.terms.price_cents = 449; CHECK(AutoDecide(c, REF).kind == AutoKind::Refuse);
	c.terms.price_cents = 475; CHECK(AutoDecide(c, REF).kind == AutoKind::Accept);

	/* Whole negotiation through the table: offer 400 -> NPC counters 450 -> buyer counters 460 -> NPC accepts (92%) -> Active with escrow. */
	Table n;
	CHECK(n.Offer(MakeDeal(1), T(50, 400, 6), 200, CASH, TODAY, id) == Result::Ok);
	Contract &k = *n.Get(id);
	a = AutoDecide(k, REF);
	CHECK(a.kind == AutoKind::Counter);
	CHECK(n.Counter(id, Side::Seller, a.terms, 200, CASH, TODAY + 1) == Result::Ok);
	CHECK(k.round == 2 && k.turn == Side::Buyer && k.status == Status::Countered);
	CHECK(AutoDecide(k, REF).kind == AutoKind::Wait); // the buyer's turn
	CHECK(n.Counter(id, Side::Buyer, T(50, 460, 6), 200, CASH, TODAY + 2) == Result::Ok);
	CHECK(k.round == 3 && k.turn == Side::Seller);
	CHECK(AutoDue(k, SYSTEM, LastMoveDay(k) + AutoDelayDays(k)));
	CHECK(AutoDecide(k, REF).kind == AutoKind::Accept);
	CHECK(n.Accept(id, Side::Seller, 200, CASH, TODAY + 4) == Result::Ok);
	CHECK(k.status == Status::Active && k.escrow == EscrowOf(k.terms));
	/* The same with a buyer who stays at 400: the NPC refuses in the last round. */
	Table r;
	CHECK(r.Offer(MakeDeal(1), T(50, 400, 6), 200, CASH, TODAY, id) == Result::Ok);
	CHECK(r.Counter(id, Side::Seller, AutoDecide(*r.Get(id), REF).terms, 200, CASH, TODAY + 1) == Result::Ok);
	CHECK(r.Counter(id, Side::Buyer, T(50, 440, 6), 200, CASH, TODAY + 2) == Result::Ok);
	CHECK(AutoDecide(*r.Get(id), REF).kind == AutoKind::Refuse); // 88% in the last round
	CHECK(r.Refuse(id, Side::Seller, TODAY + 3) == Result::Ok);
}

static void TestDelivery()
{
	Table t;
	const uint32_t a = MakeActive(t, 1, 40);
	const uint32_t b = MakeActive(t, 1, 30);
	const Deal d = MakeDeal(1);
	/* Fills the first contract up to its amount, then the next; the rest is not counted. */
	CHECK(t.AddDelivery(1, d.buyer_year, 7, d.ident, d.built, 3, 25) == 25);
	CHECK(t.Get(a)->delivered == 25 && t.Get(b)->delivered == 0);
	CHECK(t.AddDelivery(1, d.buyer_year, 7, d.ident, d.built, 3, 25) == 25);
	CHECK(t.Get(a)->delivered == 40 && t.Get(b)->delivered == 10);
	CHECK(t.AddDelivery(1, d.buyer_year, 7, d.ident, d.built, 3, 100) == 20);
	CHECK(t.Get(b)->delivered == 30);
	CHECK(t.AddDelivery(1, d.buyer_year, 7, d.ident, d.built, 3, 5) == 0);
	/* Other buyer, other year of the buyer, other cargo, other industry, other identity: nothing is counted. */
	Table u;
	const uint32_t x = MakeActive(u, 1, 40);
	CHECK(u.AddDelivery(2, d.buyer_year, 7, d.ident, d.built, 3, 5) == 0);
	CHECK(u.AddDelivery(1, d.buyer_year + 1, 7, d.ident, d.built, 3, 5) == 0);
	CHECK(u.AddDelivery(1, d.buyer_year, 7, d.ident, d.built, 4, 5) == 0);
	CHECK(u.AddDelivery(1, d.buyer_year, 8, d.ident, d.built, 3, 5) == 0);
	CHECK(u.AddDelivery(1, d.buyer_year, 7, d.ident + 1, d.built, 3, 5) == 0);
	CHECK(u.AddDelivery(1, d.buyer_year, 7, d.ident, d.built, 3, 0) == 0);
	CHECK(u.Get(x)->delivered == 0);
	/* An open offer or a closed contract counts nothing. */
	Table v;
	uint32_t id = 0;
	CHECK(v.Offer(MakeDeal(1), T(50, 500, 6), 200, CASH, TODAY, id) == Result::Ok);
	CHECK(v.AddDelivery(1, d.buyer_year, 7, d.ident, d.built, 3, 5) == 0);
}


/** Step 5: the contract price and the 70% without a contract. */
static void TestDeliveryProfit()
{
	/* 100 units, normal pay 1000. No contract, industry of the SYSTEM: normal. With an owner: 70%. */
	CHECK(DeliveryProfit(1000, 100, 0, 0, false) == 1000);
	CHECK(DeliveryProfit(1000, 100, 0, 0, true) == 700);
	/* 60 of the 100 units are on a contract that pays 6 a unit (360): the other 40 units pay 400 x 70% = 280. */
	CHECK(DeliveryProfit(1000, 100, 60, 360, true) == 360 + 280);
	CHECK(DeliveryProfit(1000, 100, 60, 360, false) == 360 + 400);
	/* All units on the contract: the normal pay does not count. */
	CHECK(DeliveryProfit(1000, 100, 100, 450, true) == 450);
	/* Bad input: more units counted than delivered is clamped; 0 units is the normal pay; nothing negative. */
	CHECK(DeliveryProfit(1000, 100, 500, 450, true) == 450);
	CHECK(DeliveryProfit(1000, 0, 0, 0, true) == 1000);
	CHECK(DeliveryProfit(0, 100, 0, 0, true) == 0);
	/* Huge pay does not overflow. */
	CHECK(DeliveryProfit(4000000000000000000LL / 100, 3, 1, 5, true) > 0);
	/* The value of counted units at the contract price (AddDelivery): 25 units at 4.50 and 15 units at 6.00. */
	Table t;
	const uint32_t a = MakeActive(t, 1, 25);
	const uint32_t b = MakeActive(t, 1, 30);
	t.Get(a)->terms.price_cents = 450;
	t.Get(b)->terms.price_cents = 600;
	const Deal d = MakeDeal(1);
	int64_t value = 0;
	CHECK(t.AddDelivery(1, d.buyer_year, 7, d.ident, d.built, 3, 40, &value) == 40);
	CHECK(value == 25 * 450 + 15 * 600);
}

/** Step 6: the monthly settlement. */
static void TestSettleMonth()
{
	const int64_t FULL_CASH = 1000000;
	auto cash_of = [&](int64_t amount) { return [amount](uint8_t) { return amount; }; };

	/* Spec case: 60 of 80 delivered, price 4.50. Missing 20 -> value 90 -> fine 5% = 4 (rounded down), paid by the owner to the buyer. */
	{
		Table t;
		Table &tt = t;
		const uint32_t id = MakeActive(tt, 1, 80);
		Contract &c = *t.Get(id);
		c.terms.price_cents = 450;
		c.seller = 2; c.seller_year = 1951;
		c.escrow = EscrowOf(c.terms);
		c.delivered = 60;
		const int64_t escrow = c.escrow;
		const auto r = t.SettleMonth(cash_of(FULL_CASH), TODAY);
		CHECK(r.size() == 1);
		CHECK(r[0].missing == 20 && r[0].delivered == 60 && r[0].paid_at_delivery == 270);
		CHECK(r[0].fine == 4 && r[0].fine_paid == 4 && r[0].debt_after == 0);
		CHECK(r[0].streak == 1 && !r[0].free_cancel && !r[0].completed);
		CHECK(c.delivered == 0 && c.miss_streak == 1 && c.status == Status::Active && c.escrow == escrow); // the escrow stays held
		CHECK(c.months_left == c.months_total - 1);
	}
	/* Full delivery: no fine, streak back to 0. */
	{
		Table t;
		const uint32_t id = MakeActive(t, 1, 80);
		Contract &c = *t.Get(id);
		c.seller = 2; c.seller_year = 1951;
		c.miss_streak = 1;
		c.delivered = 80;
		const auto r = t.SettleMonth(cash_of(FULL_CASH), TODAY);
		CHECK(r[0].missing == 0 && r[0].fine == 0 && r[0].fine_paid == 0 && r[0].streak == 0 && c.miss_streak == 0);
		/* More than agreed is not counted by AddDelivery, but even if it were, there is no negative shortfall. */
		c.delivered = 500;
		const auto r2 = t.SettleMonth(cash_of(FULL_CASH), TODAY + 30);
		CHECK(r2[0].missing == 0 && r2[0].fine == 0);
	}
	/* Short 2 months in a row: the buyer may end the contract without a fine (and gets the escrow back). */
	{
		Table t;
		const uint32_t id = MakeActive(t, 1, 80);
		Contract &c = *t.Get(id);
		c.seller = 2; c.seller_year = 1951;
		c.escrow = EscrowOf(c.terms);
		c.delivered = 10;
		auto r = t.SettleMonth(cash_of(FULL_CASH), TODAY);
		CHECK(r[0].streak == 1 && !r[0].free_cancel);
		c.delivered = 0;
		r = t.SettleMonth(cash_of(FULL_CASH), TODAY + 30);
		CHECK(r[0].streak == 2 && r[0].free_cancel);
		Settlement s;
		CHECK(t.Cancel(id, Side::Buyer, TODAY + 31, s) == Result::Ok);
		CHECK(s.to_buyer > 0 && s.to_seller == 0);
	}
	/* The owner has not enough cash: pays what it has, the rest is a debt that is paid first at the next settlement. */
	{
		Table t;
		const uint32_t id = MakeActive(t, 1, 100);
		Contract &c = *t.Get(id);
		c.terms.price_cents = 1000; // value of the 100 missing units = 1000, fine = 50
		c.seller = 2; c.seller_year = 1951;
		c.delivered = 0;
		auto r = t.SettleMonth(cash_of(30), TODAY);
		CHECK(r[0].fine == 50 && r[0].fine_paid == 30 && r[0].debt_after == 20 && c.fine_debt == 20);
		/* Next month the owner has 25: the old debt (20) is paid, then 5 of the new fine of 50. */
		c.delivered = 0;
		r = t.SettleMonth(cash_of(25), TODAY + 30);
		CHECK(r[0].fine == 50 && r[0].fine_paid == 25 && r[0].debt_after == 45 && c.fine_debt == 45);
		/* An owner with no cash at all pays nothing and the cash never goes below 0. */
		c.delivered = 0;
		r = t.SettleMonth(cash_of(-500), TODAY + 60);
		CHECK(r[0].fine_paid == 0 && r[0].debt_after == 95);
		/* The debt is saved and loaded. */
		Table u;
		for (const Record &rec : t.ToRecords()) CHECK(u.FromRecord(rec));
		CHECK(u.Get(id) != nullptr && u.Get(id)->fine_debt == 95);
	}
	/* One owner with two contracts shares the cash. */
	{
		Table t;
		const uint32_t a = MakeActive(t, 1, 100);
		const uint32_t b = MakeActive(t, 3, 100);
		for (const uint32_t id : {a, b}) { Contract &c = *t.Get(id); c.terms.price_cents = 1000; c.seller = 2; c.seller_year = 1951; c.delivered = 0; }
		int reads = 0;
		const auto r = t.SettleMonth([&reads](uint8_t) { reads++; return int64_t(70); }, TODAY);
		CHECK(reads == 1);
		CHECK(r.size() == 2 && r[0].fine_paid == 50 && r[1].fine_paid == 20 && r[1].debt_after == 30);
	}
	/* A SYSTEM seller pays no fine; the streak still counts. */
	{
		Table t;
		const uint32_t id = MakeActive(t, 1, 80);
		Contract &c = *t.Get(id);
		c.delivered = 0;
		const auto r = t.SettleMonth(cash_of(FULL_CASH), TODAY);
		CHECK(r[0].fine == 0 && r[0].fine_paid == 0 && r[0].streak == 1);
	}
	/* The last month: Completed, the escrow goes back to the buyer, the delivered count is reset. */
	{
		Table t;
		const uint32_t id = MakeActive(t, 1, 80);
		Contract &c = *t.Get(id);
		c.seller = 2; c.seller_year = 1951;
		c.escrow = EscrowOf(c.terms);
		const int64_t escrow = c.escrow;
		c.months_left = 1;
		c.delivered = 80;
		const auto r = t.SettleMonth(cash_of(FULL_CASH), TODAY);
		CHECK(r[0].completed && r[0].escrow_back == escrow && c.status == Status::Completed && c.escrow == 0 && c.closed_day == TODAY);
		CHECK(t.Reserved(c.industry, c.ident, c.built, c.cargo) == 0); // the output is free again
		/* A completed contract is not settled again. */
		CHECK(t.SettleMonth(cash_of(FULL_CASH), TODAY + 30).empty());
	}
	/* Fine of a shortfall: rounding and a huge amount. */
	CHECK(FineOf(20, 450) == 4);
	CHECK(FineOf(0, 450) == 0);
	CHECK(FineOf(MAX_QTY, MAX_PRICE_CENTS) == MAX_QTY * (MAX_PRICE_CENTS / 100) * 5 / 100);
	/* The month number that is saved: last_month round trip, and an old record without it. */
	{
		Table t;
		MakeActive(t, 1, 80);
		t.last_month = 123;
		Table u;
		for (const Record &rec : t.ToRecords()) CHECK(u.FromRecord(rec));
		CHECK(u.last_month == 123);
		Table v = u;
		Record old{};
		old.ym = -1; old.v1 = 5; old.v2 = 0; old.v3 = 0;
		Table w;
		CHECK(w.FromRecord(old) && w.last_month == -1);
	}
}

int main()
{
	TestPureRules();
	TestOfferAndReserve();
	TestOfferChecks();
	TestNegotiation();
	TestThreeRoundsExpire();
	TestRefuseAndWithdraw();
	TestAcceptChecksAgain();
	TestAcceptShare();
	TestCancelActive();
	TestBankruptcy();
	TestPurge();
	TestSaveLoad();
	TestOldSaveAndNonsense();
	TestAutoAnswer();
	TestDelivery();
	TestDeliveryProfit();
	TestSettleMonth();
	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
