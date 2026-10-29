/* Stand-alone test of town_wallet_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/twt town_wallet_test.cpp && /tmp/twt */
#include <cstdio>
#include <initializer_list>
#include <vector>
#include "town_wallet_math.h"
using namespace townwallet;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
	/* Consumer cargo: the town pays what it has, never more than the income, and never from a negative treasury. */
	{
		Payment p = PayConsumer(1000, 5000);
		CHECK(p.paid == 1000 && p.from_town == 1000 && p.shortfall == 0 && p.tax == 0);
		p = PayConsumer(1000, 300);
		CHECK(p.paid == 300 && p.shortfall == 700 && p.paid + p.shortfall == 1000);
		p = PayConsumer(1000, 0);
		CHECK(p.paid == 0 && p.shortfall == 1000);
		p = PayConsumer(1000, -50);
		CHECK(p.paid == 0 && p.from_town == 0 && p.shortfall == 1000);
		p = PayConsumer(0, 100); CHECK(p.paid == 0 && p.shortfall == 0);
		p = PayConsumer(-5, 100); CHECK(p.paid == 0 && p.shortfall == 0);
	}
	/* Industrial cargo: 6% business tax, the rest is paid; paid + tax = income. */
	{
		Payment p = PayIndustrial(10'000);
		CHECK(p.tax == 600 && p.paid == 9'400 && p.paid + p.tax == 10'000);
		for (int64_t x : std::initializer_list<int64_t>{1, 7, 99, 101, 12345, 999999999999LL}) { p = PayIndustrial(x); CHECK(p.paid + p.tax == x && p.tax >= 0 && p.paid >= 0); }
		CHECK(PayIndustrial(0).paid == 0 && PayIndustrial(-3).tax == 0);
	}
	/* Interest: carry keeps the rest, a year of days gives debt x rate. */
	{
		int64_t carry = 0, total = 0;
		for (int d = 0; d < 365; d++) total += DailyInterest(100'000, 500, carry); // 5% a year
		CHECK(total == 5'000 || total == 4'999);
		carry = 0; CHECK(DailyInterest(0, 500, carry) == 0 && DailyInterest(1000, 0, carry) == 0);
		carry = 0; CHECK(DailyInterest(10, 500, carry) == 0 && carry > 0); // less than one unit: kept
	}
	/* Rates. */
	{
		CHECK(LoanRateBp(350) == 450 && BondRateBp(350) == 350 && LoanRateBp(350) > BondRateBp(350));
		CHECK(LoanRateBp(0) == 100 && BondRateBp(0) == RATE_MIN_BP && LoanRateBp(-500) == RATE_MIN_BP);
	}
	/* Funding: bonds first, then the bank; limits; nothing is created (from_bonds + from_loan + unmet = need). */
	{
		Funding f = Fund(1000, 1000, 0, 0, 100'000, 1'000'000);
		CHECK(f.from_bonds == 1000 && f.from_loan == 0 && f.unmet == 0);
		f = Fund(1000, 1000, 0, 0, 500, 1'000'000); // the savers have 500: 60% = 300
		CHECK(f.from_bonds == 300 && f.from_loan == 700 && f.unmet == 0);
		f = Fund(1000, 1000, 0, 0, 0, 1'000'000);
		CHECK(f.from_bonds == 0 && f.from_loan == 1000);
		f = Fund(1000, 1000, 0, 0, 0, 1000); // the bank lends 20% of its free money = 200
		CHECK(f.from_loan == 200 && f.unmet == 800);
		f = Fund(1000, 10, 0, 0, 1'000'000, 1'000'000); // 10 citizens: bond limit 600, loan limit 400
		CHECK(f.from_bonds == 600 && f.from_loan == 400 && f.unmet == 0);
		f = Fund(5000, 10, 600, 400, 1'000'000, 1'000'000); // limits used up
		CHECK(f.from_bonds == 0 && f.from_loan == 0 && f.unmet == 5000);
		f = Fund(0, 10, 0, 0, 1, 1); CHECK(f.from_bonds == 0 && f.from_loan == 0 && f.unmet == 0);
		for (int64_t need : {1, 17, 999, 123'456}) for (int64_t cash : {0, 50, 10'000}) for (int64_t bank : {0, 100, 1'000'000}) {
			f = Fund(need, 500, 100, 50, cash, bank);
			CHECK(f.from_bonds >= 0 && f.from_loan >= 0 && f.unmet >= 0 && f.from_bonds + f.from_loan + f.unmet == need);
		}
	}
	/* Repayment: only from the treasury above the floor, the loan first, never more than the debt or the spare money. */
	{
		Repay r = RepayDay(100, 1000, 5000, 5000); // floor 2000: nothing spare
		CHECK(r.loan == 0 && r.bonds == 0);
		r = RepayDay(10'000, 1000, 5000, 5000); // spare 8000: loan 10% = 500, bonds 10% = 500
		CHECK(r.loan == 500 && r.bonds == 500);
		r = RepayDay(2'100, 1000, 5000, 5000); // spare 100: all to the loan
		CHECK(r.loan == 100 && r.bonds == 0);
		r = RepayDay(1'000'000, 1000, 3, 4); // tiny debts: at least 1 a day, never above the debt
		CHECK(r.loan == 1 && r.bonds == 1);
		r = RepayDay(1'000'000, 1000, 0, 0); CHECK(r.loan == 0 && r.bonds == 0);
		for (int64_t t : {0, 1999, 2000, 2001, 50'000}) for (int64_t l : {0, 1, 40, 99'999}) for (int64_t b : {0, 1, 60, 99'999}) {
			r = RepayDay(t, 1000, l, b);
			CHECK(r.loan >= 0 && r.bonds >= 0 && r.loan <= l && r.bonds <= b && r.loan + r.bonds <= std::max<int64_t>(0, t - TreasuryFloor(1000)));
		}
	}
	/* Grants: weights favour poor treasuries, shares add up to at most the pool, the cap per citizen holds. */
	{
		CHECK(GrantWeight(0, 0) == 0);
		CHECK(GrantWeight(1000, 0) > GrantWeight(1000, 10'000) && GrantWeight(1000, 10'000) > GrantWeight(1000, 20'000));
		CHECK(GrantWeight(1000, 1'000'000) == GrantWeight(1000, 20'000)); // rich: the minimum weight
		CHECK(GrantWeight(2000, 0) > GrantWeight(1000, 0));
		const int64_t pops[4] = {200, 1000, 5000, 40'000}, tr[4] = {0, 50'000, 5'000, 800'000};
		int64_t w[4], total = 0;
		for (int i = 0; i < 4; i++) { w[i] = GrantWeight(pops[i], tr[i]); total += w[i]; }
		for (int64_t pool : std::initializer_list<int64_t>{0, 1, 1000, 77777, 10000000000LL}) {
			int64_t sum = 0;
			for (int i = 0; i < 4; i++) { const int64_t g = GrantOf(pool, w[i], total, pops[i]); CHECK(g >= 0 && g <= pops[i] * GRANT_MAX_PER_CAPITA); sum += g; }
			CHECK(sum <= pool);
		}
		CHECK(GrantOf(0, 5, 10, 100) == 0 && GrantOf(100, 0, 10, 100) == 0 && GrantOf(100, 5, 0, 100) == 0);
		CHECK(GrantPool(25'000, 12'500) == 1'250 && GrantPool(10'000, 12'500) == 0 && GrantPool(-5, 0) == 0);
	}
	/* A closed loop over 20 years: citizens' savers, treasury, bank. The sum of all three never changes except by the tax that enters
	 * (income tax and grants are moves between the treasury and the bank / citizens, interest moves between them too). */
	{
		for (int64_t pop : {300, 3000, 30'000}) {
			int64_t treasury = 0, savers = 50'000 * pop / 1000, bank = 25'000'000;
			int64_t loan = 0, bonds = 0, lcarry = 0, bcarry = 0;
			int64_t taxed_in = 0, paid_out = 0, tax_total = 0;
			const int64_t start = treasury + savers + bank;
			bool ok = true;
			for (int64_t day = 1; day <= 365 * 20; day++) {
				const int64_t tax = pop * 12 / 100 + 1; // the income tax of the citizens (money in from outside the loop)
				treasury += tax; tax_total += tax;
				/* Interest. */
				const int64_t li = DailyInterest(loan, LoanRateBp(350), lcarry), bi = DailyInterest(bonds, BondRateBp(350), bcarry);
				const int64_t pay_l = std::min(treasury, li); treasury -= pay_l; bank += pay_l; loan += li - pay_l;
				const int64_t pay_b = std::min(treasury, bi); treasury -= pay_b; savers += pay_b; bonds += bi - pay_b;
				/* A delivery of goods every 3 days that the town buys. */
				if (day % 3 == 0) {
					const int64_t income = 4'000;
					Payment p = PayConsumer(income, treasury);
					treasury -= p.from_town; paid_out += p.paid;
					if (p.shortfall > 0 && day % 6 == 0) { // the town borrows what it lacks for the next time
						Funding f = Fund(p.shortfall, pop, bonds, loan, savers, bank - 12'500);
						treasury += f.from_bonds + f.from_loan; savers -= f.from_bonds; bonds += f.from_bonds; bank -= f.from_loan; loan += f.from_loan;
					}
				}
				/* Repay. */
				Repay r = RepayDay(treasury, pop, loan, bonds);
				treasury -= r.loan + r.bonds; bank += r.loan; loan -= r.loan; savers += r.bonds; bonds -= r.bonds;
				/* Grant once a year. */
				if (day % 365 == 0) {
					const int64_t w = GrantWeight(pop, treasury);
					const int64_t g = GrantOf(GrantPool(bank, 12'500), w, w, pop);
					treasury += g; bank -= g;
				}
				if (treasury < 0 || loan < 0 || bonds < 0 || bank < 0 || savers < 0) ok = false;
				if (loan > LoanLimit(pop) + 1'000'000 || bonds > BondLimit(pop) + 1'000'000) ok = false; // interest added to debts can pass a limit a little
			}
			(void)taxed_in;
			CHECK(ok);
			/* Conservation: what is in the loop at the end = what was there at the start + the income tax that came in - what the companies were paid. */
			CHECK(treasury + savers + bank == start + tax_total - paid_out);
			std::printf("pop %6lld: treasury %lld, loan %lld, bonds %lld, paid to companies in 20 years %lld (tax in %lld)\n", (long long)pop, (long long)treasury, (long long)loan, (long long)bonds, (long long)paid_out, (long long)tax_total);
		}
	}
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
