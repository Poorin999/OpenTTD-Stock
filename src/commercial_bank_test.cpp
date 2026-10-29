/* Stand-alone test of commercial_bank_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/cbt commercial_bank_test.cpp && /tmp/cbt */
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>
#include "commercial_bank_math.h"
using namespace cbank;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/** A small deterministic random number generator (the game uses hashes, the test only needs a repeatable stream). */
struct Rng {
	uint64_t s = 88172645463325252ULL;
	uint64_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
	int64_t Below(int64_t n) { return n <= 0 ? 0 : static_cast<int64_t>(Next() % static_cast<uint64_t>(n)); }
};

int main()
{
	/* Start capital: per citizen with a minimum, scaled by the price index. */
	CHECK(StartCapital(0, PPM) == START_CAPITAL_MIN);
	CHECK(StartCapital(10, PPM) == START_CAPITAL_MIN);
	CHECK(StartCapital(1000, PPM) == 1000 * START_CAPITAL_PER_CAPITA);
	CHECK(StartCapital(1000, 2 * PPM) == 2 * 1000 * START_CAPITAL_PER_CAPITA);
	CHECK(StartCapital(-5, PPM) == START_CAPITAL_MIN);
	CHECK(StartCapital(1000, 0) == 0);

	/* Rating bands at 25 / 17 / 11 / 7 percent; no loans = AAA, no equity = C; never better with less equity. */
	CHECK(RatingIndex(0, 0) == 0 && RatingIndex(-5, 100) == 0);
	CHECK(RatingIndex(1, 0) == 4 && RatingIndex(1000, 0) == 4);
	CHECK(RatingIndex(2500, 10000) == 4 && RatingIndex(2499, 10000) == 3);
	CHECK(RatingIndex(1700, 10000) == 3 && RatingIndex(1699, 10000) == 2);
	CHECK(RatingIndex(1100, 10000) == 2 && RatingIndex(1099, 10000) == 1);
	CHECK(RatingIndex(700, 10000) == 1 && RatingIndex(699, 10000) == 0);
	{
		int prev = 0;
		for (int64_t eq = 0; eq <= 5000; eq += 7) { const int r = RatingIndex(eq, 10000); CHECK(r >= prev); prev = r; }
		CHECK(RatingIndex(INT64_MAX / 4, 1) == 4);
		CHECK(RatingIndex(900'000'000'000'000LL, 3'000'000'000'000'000LL) >= 0);
	}
	CHECK(std::string(RatingName(0)) == "C" && std::string(RatingName(2)) == "A" && std::string(RatingName(4)) == "AAA" && std::string(RatingName(99)) == "AAA");

	/* Margin: a better bank is cheaper; a building makes it cheaper still. */
	for (int r = 0; r + 1 < RATINGS; r++) CHECK(LoanMarginBp(r, false) > LoanMarginBp(r + 1, false));
	for (int r = 0; r < RATINGS; r++) CHECK(LoanMarginBp(r, true) == LoanMarginBp(r, false) - BUILDING_MARGIN_CUT_BP);
	CHECK(LoanMarginBp(2, false) == -10 && LoanMarginBp(0, false) == 75 && LoanMarginBp(4, false) == -40);
	CHECK(TownLoanRateBp(350, 2, false) == 350 + 100 - 10);
	CHECK(TownLoanRateBp(0, 4, true) >= townwallet::RATE_MIN_BP);
	CHECK(DepositRateBp(350) == 200 && DepositRateBp(100) == 0 && DepositRateBp(0) == 0 && DepositRateBp(-50) == 0);
	CHECK(RefinanceRateBp(350) == 400 && RefinanceRateBp(0) == std::max<int64_t>(townwallet::RATE_MIN_BP, REFINANCE_SPREAD_BP));
	for (int64_t up = 0; up <= 2500; up += 25) {
		CHECK(DepositRateBp(up) <= std::max<int64_t>(0, up - 10));
		CHECK(DepositRateBp(up) <= TownLoanRateBp(up, 0, true));
	}

	/* Deposit target. */
	CHECK(DepositTargetPercent(2, true) == 0 && DepositTargetPercent(2, false) == 0);
	CHECK(DepositTargetPercent(3, false) == 50 && DepositTargetPercent(3, true) == 60);
	CHECK(DepositTargetPercent(0, false) == 30 && DepositTargetPercent(0, true) == 40);
	CHECK(DepositTargetPercent(-1, false) == 30 && DepositTargetPercent(77, false) == 50);

	/* Deposit move. */
	{
		int64_t cash = 100'000, dep = 0;
		for (int d = 0; d < 120; d++) {
			const int64_t m = DepositMove(cash, dep, 50);
			CHECK(m >= 0 && m <= cash);
			cash -= m; dep += m;
			CHECK(cash >= 0 && dep >= 0 && cash + dep == 100'000);
		}
		CHECK(dep >= 49'990 && dep <= 50'010);
		for (int d = 0; d < 400; d++) {
			const int64_t m = DepositMove(cash, dep, 0);
			CHECK(m <= 0 && -m <= dep);
			cash -= m; dep += m;
		}
		CHECK(dep == 0 && cash == 100'000);
		CHECK(DepositMove(0, 0, 50) == 0 && DepositMove(-5, -5, 50) == 0);
		CHECK(DepositMove(1000, 500, 200) >= 0);
		CHECK(DepositMove(10, 0, 50) == 5);
	}

	/* Capacity. */
	{
		Sheet s; s.liquid = 500'000; s.deposits = {100'000, 0, 0, 100'000};
		CHECK(ReserveNeed(s.TotalDeposits()) == 20'000);
		const int64_t eq = Equity(s, 0);
		CHECK(eq == 500'000 - 200'000);
		CHECK(Capacity(s, eq, 0) == 480'000);
		CHECK(Capacity(s, eq, 3'500'000) == 100'000);
		CHECK(Capacity(s, eq, 3'600'000) == 0 && Capacity(s, eq, 9'999'999) == 0);
		CHECK(Capacity(s, 0, 0) == 0 && Capacity(s, -10, 0) == 0);
		s.liquid = 10'000;
		CHECK(Capacity(s, 100'000, 0) == 0);
		CHECK(LargeExposureLimit(100'000) == 40'000 && LargeExposureLimit(-1) == 0 && LargeExposureLimit(0) == 0);
		CHECK(TownLoanLimit(1000) == 40'000 && TownLoanLimit(-1) == 0);
		CHECK(TotalCreditLimit(100'000) == 125'000 && TotalCreditLimit(0) == 0 && TotalCreditLimit(-5) == 0 && TotalCreditLimit(3) == 3);
	}

	/* SplitByDeposits. */
	{
		std::array<int64_t, GROUPS> d = {1000, 0, 0, 3000};
		auto p = SplitByDeposits(d, 1001);
		CHECK(p[0] + p[3] == 1001 && p[1] == 0 && p[2] == 0 && p[0] >= 250 && p[0] <= 251);
		d = {0, 0, 0, 0}; p = SplitByDeposits(d, 50);
		CHECK(p[0] + p[1] + p[2] + p[3] == 0);
		d = {7, 11, 13, 17}; int64_t sum = 0;
		for (int64_t a : std::initializer_list<int64_t>{1, 2, 3, 99, 100, 12345, 999999999999LL}) { p = SplitByDeposits(d, a); sum = p[0] + p[1] + p[2] + p[3]; CHECK(sum == a); }
	}

	/* Settle: deposit interest credited. */
	{
		Sheet s; s.liquid = 1'000'000; s.deposits = {0, 0, 0, 100'000};
		int64_t credited = 0;
		DayIn in; in.policy_upper_bp = 350 + 150; in.iorb_bp = 0; in.cb_free = 0;
		for (int d = 0; d < 365; d++) {
			const DayOut o = Settle(s, in);
			credited += o.deposit_interest;
			CHECK(o.deposit_interest == o.deposit_interest_by_group[0] + o.deposit_interest_by_group[1] + o.deposit_interest_by_group[2] + o.deposit_interest_by_group[3]);
		}
		CHECK(credited > 3'400 && credited < 3'700);
		CHECK(s.deposits[3] == 100'000 + credited && s.deposit_interest == credited);
	}
	/* Settle: reserve interest is new money and is counted. */
	{
		Sheet s; s.liquid = 3'650'000;
		DayIn in; in.policy_upper_bp = 500; in.iorb_bp = 490; in.cb_free = 0;
		int64_t made = 0;
		for (int d = 0; d < 365; d++) made += Settle(s, in).reserve_interest;
		CHECK(made > 170'000 && made < 190'000 && s.created == made);
		Sheet z; DayIn zin; zin.iorb_bp = 500; zin.policy_upper_bp = 500;
		CHECK(Settle(z, zin).reserve_interest == 0 && z.created == 0);
	}
	/* Settle: borrow when short, repay with plenty. */
	{
		Sheet s; s.liquid = 5'000; s.deposits = {100'000, 0, 0, 0};
		DayIn in; in.policy_upper_bp = 350; in.iorb_bp = 0; in.cb_free = 100'000;
		DayOut o = Settle(s, in);
		CHECK(o.borrowed == 5'000 && s.liquid == 10'000 && s.cb_loan == 5'000);
		DayIn poor = in; poor.cb_free = 1'000; Sheet t = s; t.liquid = 2'000; t.cb_loan = 0;
		o = Settle(t, poor);
		CHECK(o.borrowed == 1'000 && t.liquid == 3'000);
		DayIn none = in; none.cb_free = 0; Sheet u; u.liquid = 0; u.deposits = {100'000, 0, 0, 0};
		o = Settle(u, none); CHECK(o.borrowed == 0 && u.liquid == 0);
		Sheet r; r.liquid = 1'000'000; r.deposits = {100'000, 0, 0, 0}; r.cb_loan = 50'000;
		o = Settle(r, none);
		CHECK(o.repaid == 10'000 && r.cb_loan <= 40'000 + 5 && r.cb_loan >= 40'000 - 5);
		Sheet keep; keep.liquid = 25'000; keep.deposits = {100'000, 0, 0, 0}; keep.cb_loan = 50'000;
		o = Settle(keep, none);
		CHECK(o.repaid >= 4'980 && o.repaid <= 5'000 && keep.liquid >= 19'980 && keep.liquid <= 20'000);
	}
	/* Settle: interest on the central bank loan. */
	{
		Sheet s; s.liquid = 100'000; s.cb_loan = 3'650'000;
		DayIn in; in.policy_upper_bp = 350; in.iorb_bp = 0; in.cb_free = 0;
		const DayOut o = Settle(s, in);
		CHECK(o.cb_interest_paid == 400 || o.cb_interest_paid == 399);
		Sheet p; p.liquid = 0; p.cb_loan = 3'650'000;
		const DayOut q = Settle(p, in);
		CHECK(q.cb_interest_paid == 0 && p.cb_loan > 3'650'000 - 1 && p.liquid == 0);
	}

	/* Resolve. */
	{
		Sheet s; s.liquid = 100; s.deposits = {500, 0, 0, 500}; s.cb_loan = 0;
		CHECK(Equity(s, 500) == 100 + 500 - 1000);
		Rescue r = Resolve(s, 500, 1000);
		CHECK(r.covered == 400 && r.haircut == 0 && Equity(s, 500) == 0 && s.failures == 1);
		Sheet t; t.liquid = 100; t.deposits = {500, 0, 0, 500};
		r = Resolve(t, 500, 150);
		CHECK(r.covered == 150 && r.haircut == 250 && Equity(t, 500) == 0 && t.deposits[0] + t.deposits[3] == 750 && t.deposits[0] == 375);
		Sheet u; u.liquid = 100; u.deposits = {500, 0, 0, 500};
		r = Resolve(u, 500, 0);
		CHECK(r.covered == 0 && r.haircut == 400 && Equity(u, 500) == 0);
		Sheet ok; ok.liquid = 1000; ok.deposits = {500, 0, 0, 0};
		r = Resolve(ok, 0, 1000);
		CHECK(r.covered == 0 && r.haircut == 0 && ok.failures == 0);
		Sheet hopeless; hopeless.liquid = 0; hopeless.deposits = {100, 0, 0, 0};
		r = Resolve(hopeless, 0, 0);
		CHECK(r.haircut == 100 && hopeless.TotalDeposits() == 0);
		r = Resolve(hopeless, 0, 0); CHECK(r.covered == 0 && r.haircut == 0 && hopeless.failures == 1);
	}

	/* Long simulation: 20 game years, 40 runs, invariants every day. */
	{
		Rng rng;
		for (int run = 0; run < 40; run++) {
			Sheet s;
			int64_t citizens_cash = 2'000'000 + rng.Below(1'000'000);
			const int64_t cap0 = StartCapital(5000 + rng.Below(20'000), PPM);
			s.liquid = cap0;
			int64_t cb_money = 500'000 + rng.Below(500'000);
			std::vector<int64_t> loans;
			int64_t loans_sum = 0;
			const int64_t cash_plus_dep0 = citizens_cash;
			int64_t haircuts = 0, interest_to_depositors = 0, rescued = 0;
			const bool building = (run % 2) == 0;
			for (int day = 0; day < 365 * 20; day++) {
				const int64_t upper = 350 + (day / 365 % 5) * 50;
				for (int g = 0; g < GROUPS; g++) {
					const int64_t cash_g = citizens_cash / GROUPS;
					int64_t mv = DepositMove(cash_g, s.deposits[g], DepositTargetPercent(g, building));
					if (mv < 0) mv = -std::min(-mv, std::max<int64_t>(0, s.liquid));
					s.deposits[g] += mv; s.liquid += mv; citizens_cash -= mv;
					CHECK(s.deposits[g] >= 0 && citizens_cash >= 0);
				}
				citizens_cash += 800 + rng.Below(400);
				if (rng.Below(10) == 0) {
					const int64_t eq = Equity(s, loans_sum);
					const int64_t want = 10'000 + rng.Below(150'000);
					const int64_t give = std::min({want, Capacity(s, eq, loans_sum), LargeExposureLimit(eq)});
					if (give > 0) {
						s.liquid -= give; loans.push_back(give); loans_sum += give;
						CHECK(s.liquid >= ReserveNeed(s.TotalDeposits()) - 1);
						CHECK(loans_sum <= std::max<int64_t>(eq, 1) * MAX_LEVERAGE);
					}
				}
				for (size_t i = 0; i < loans.size();) {
					const int64_t in_day = loans[i] * 600 / 3'650'000;
					s.liquid += in_day; s.loan_interest += in_day;
					const int64_t roll = rng.Below(3000);
					if (roll == 0) { s.liquid += loans[i]; loans_sum -= loans[i]; loans.erase(loans.begin() + static_cast<long>(i)); continue; }
					if (roll == 1 && rng.Below(4) == 0) { s.written_off += loans[i]; loans_sum -= loans[i]; loans.erase(loans.begin() + static_cast<long>(i)); continue; }
					i++;
				}
				DayIn in; in.policy_upper_bp = upper; in.iorb_bp = std::max<int64_t>(0, upper - 10); in.cb_free = std::max<int64_t>(0, cb_money - 12'500);
				const DayOut o = Settle(s, in);
				cb_money -= o.borrowed; cb_money += o.repaid; cb_money += o.cb_interest_paid;
				interest_to_depositors += o.deposit_interest;
				const Rescue r = Resolve(s, loans_sum, std::max<int64_t>(0, cb_money));
				cb_money -= r.covered; haircuts += r.haircut; rescued += r.covered;
				CHECK(s.liquid >= 0);
				CHECK(s.cb_loan >= 0 && cb_money >= 0);
				CHECK(s.TotalDeposits() >= 0);
				CHECK(Equity(s, loans_sum) >= 0 || s.TotalDeposits() == 0);
				CHECK(RatingIndex(Equity(s, loans_sum), loans_sum) >= 0 && RatingIndex(Equity(s, loans_sum), loans_sum) <= 4);
				if (day % 365 == 0) CHECK(citizens_cash + s.TotalDeposits() >= cash_plus_dep0 / 2);
			}
			CHECK(interest_to_depositors >= 0 && haircuts >= 0 && rescued >= 0);
		}
	}

	/* Identity of the balance sheet with exact numbers. */
	{
		Sheet s; s.liquid = 200'000;
		int64_t loans = 0;
		s.deposits[3] = 400'000; s.liquid += 400'000;
		CHECK(Equity(s, loans) == 200'000);
		s.liquid -= 300'000; loans += 300'000;
		CHECK(Equity(s, loans) == 200'000);
		CHECK(Capacity(s, Equity(s, loans), loans) == std::min<int64_t>(300'000 - 40'000, 12 * 200'000 - 300'000));
		s.written_off += loans; loans = 0;
		CHECK(Equity(s, loans) == -100'000);
		const Rescue r = Resolve(s, loans, 60'000);
		CHECK(r.covered == 60'000 && r.haircut == 40'000 && Equity(s, loans) == 0 && s.deposits[3] == 360'000);
	}

	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
