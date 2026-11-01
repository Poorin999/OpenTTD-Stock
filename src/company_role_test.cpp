/* Stand-alone test of company_role_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/crt company_role_test.cpp && /tmp/crt */
#include <cstdio>
#include "company_role_math.h"
using namespace crole;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
	const VehicleKind kinds[4] = {VehicleKind::Train, VehicleKind::Road, VehicleKind::Ship, VehicleKind::Aircraft};

	/* ---- The permission matrix. ---- */
	for (VehicleKind k : kinds) CHECK(VehicleAllowed(Role::TransportOwner, k));  // a transport owner may own everything
	CHECK(!VehicleAllowed(Role::IndustryOwner, VehicleKind::Train));
	CHECK(VehicleAllowed(Role::IndustryOwner, VehicleKind::Road));               // road vehicles only
	CHECK(!VehicleAllowed(Role::IndustryOwner, VehicleKind::Ship));
	CHECK(!VehicleAllowed(Role::IndustryOwner, VehicleKind::Aircraft));
	CHECK(IsValidRole(0) && IsValidRole(1) && !IsValidRole(2) && !IsValidRole(255));

	/* ---- START: only a stopped, forbidden vehicle is blocked; stopping is always fine. ---- */
	CHECK(StartBlocked(Role::IndustryOwner, VehicleKind::Train, true));   // a stopped train may not be started
	CHECK(!StartBlocked(Role::IndustryOwner, VehicleKind::Train, false)); // stopping a running one is allowed
	CHECK(!StartBlocked(Role::IndustryOwner, VehicleKind::Road, true));
	CHECK(!StartBlocked(Role::TransportOwner, VehicleKind::Aircraft, true));

	/* ---- The daily net. ---- */
	CHECK(NetVerdict(Role::TransportOwner, VehicleKind::Train, true, false) == NetAction::Allowed);
	CHECK(NetVerdict(Role::IndustryOwner, VehicleKind::Road, false, false) == NetAction::Allowed);
	CHECK(NetVerdict(Role::IndustryOwner, VehicleKind::Train, true, false) == NetAction::StopInDepot);
	CHECK(NetVerdict(Role::IndustryOwner, VehicleKind::Ship, true, true) == NetAction::AlreadyStopped);
	CHECK(NetVerdict(Role::IndustryOwner, VehicleKind::Aircraft, false, false) == NetAction::NotifyOnly); // not in a depot: left alone
	CHECK(NetVerdict(Role::IndustryOwner, VehicleKind::Aircraft, false, true) == NetAction::NotifyOnly);

	const int32_t Y = 1950;

	/* ---- Everybody starts as a transport owner without Dealer mode. ---- */
	Table t;
	CHECK(t.rows.empty());
	for (uint8_t c = 0; c < MAX_COMPANIES_N; c++) CHECK(t.RoleOf(c, Y) == Role::TransportOwner && !t.DealerOf(c, Y));

	/* ---- Dealer mode. ---- */
	CHECK(t.CanSetDealer(3, Y, true) == SetDealerResult::Ok);
	CHECK(t.CanSetDealer(3, Y, false) == SetDealerResult::Unchanged);
	CHECK(t.rows.empty()); // CanSet changes nothing
	CHECK(t.SetDealer(3, Y, true, 10) == SetDealerResult::Ok);
	CHECK(t.DealerOf(3, Y) && t.RoleOf(3, Y) == Role::TransportOwner && t.rows.size() == 1);
	CHECK(t.SetDealer(3, Y, true, 11) == SetDealerResult::Unchanged);
	CHECK(t.Find(3, Y)->since_day == 10); // an unchanged request does not touch the row
	CHECK(!t.DealerOf(4, Y)); // another company is not a dealer
	CHECK(t.SetDealer(3, Y, false, 12) == SetDealerResult::Ok);
	CHECK(!t.DealerOf(3, Y) && t.rows.empty()); // back to the default: no row is kept

	/* ---- Changing the role. ---- */
	CHECK(t.CanSetRole(3, Y, 7, 0) == SetRoleResult::BadRole);
	CHECK(t.CanSetRole(3, Y, 0, 0) == SetRoleResult::Unchanged);             // already a transport owner
	CHECK(t.CanSetRole(3, Y, 1, 5) == SetRoleResult::HasForbiddenVehicles);  // owns 5 trains / ships / aircraft
	CHECK(t.SetRole(3, Y, 1, 5, 20) == SetRoleResult::HasForbiddenVehicles && t.rows.empty()); // refused, nothing changed
	CHECK(t.SetRole(3, Y, 7, 0, 20) == SetRoleResult::BadRole && t.rows.empty());
	CHECK(t.SetRole(3, Y, 1, 0, 20) == SetRoleResult::Ok);
	CHECK(t.RoleOf(3, Y) == Role::IndustryOwner && !t.DealerOf(3, Y) && t.Find(3, Y)->since_day == 20);
	CHECK(t.CanSetRole(3, Y, 1, 0) == SetRoleResult::Unchanged);
	CHECK(t.CanSetDealer(3, Y, true) == SetDealerResult::NotTransportOwner); // an industry owner is not a dealer
	CHECK(t.SetDealer(3, Y, true, 21) == SetDealerResult::NotTransportOwner && !t.DealerOf(3, Y));
	/* Going back to the transport role has no vehicle condition (nothing is forbidden for it). */
	CHECK(t.CanSetRole(3, Y, 0, 0) == SetRoleResult::Ok);
	CHECK(t.SetRole(3, Y, 0, 0, 30) == SetRoleResult::Ok && t.RoleOf(3, Y) == Role::TransportOwner && t.rows.empty());

	/* A dealer that becomes an industry owner stops being a dealer; coming back it is not a dealer again. */
	CHECK(t.SetDealer(5, Y, true, 40) == SetDealerResult::Ok && t.DealerOf(5, Y));
	CHECK(t.SetRole(5, Y, 1, 0, 41) == SetRoleResult::Ok && !t.DealerOf(5, Y) && t.RoleOf(5, Y) == Role::IndustryOwner);
	CHECK(t.SetRole(5, Y, 0, 0, 42) == SetRoleResult::Ok && !t.DealerOf(5, Y));

	/* ---- A new company in the slot of an old one does not inherit the role (other founding year). ---- */
	t.Reset();
	CHECK(t.SetRole(2, 1950, 1, 0, 1) == SetRoleResult::Ok);
	CHECK(t.RoleOf(2, 1950) == Role::IndustryOwner);
	CHECK(t.RoleOf(2, 1955) == Role::TransportOwner);        // the new company is a transport owner
	CHECK(t.Find(2, 1955) == nullptr && !t.DealerOf(2, 1955));
	CHECK(t.CanSetRole(2, 1955, 1, 0) == SetRoleResult::Ok); // and can choose a role itself
	CHECK(t.SetRole(2, 1955, 1, 0, 9) == SetRoleResult::Ok && t.Find(2, 1955)->owner_year == 1955 && t.rows.size() == 1); // the stale row is replaced

	/* ---- Sweep and release. ---- */
	t.Reset();
	t.SetRole(1, 1950, 1, 0, 1);
	t.SetDealer(2, 1950, true, 1);
	t.SetRole(3, 1950, 1, 0, 1);
	CHECK(t.rows.size() == 3);
	const int dropped = t.Sweep([](uint8_t company, const Row &row) {
		(void)row;
		return company == 2 ? Verdict::CompanyGone : Verdict::Keep;
	});
	CHECK(dropped == 1 && t.rows.size() == 2 && t.RoleOf(1, 1950) == Role::IndustryOwner && !t.DealerOf(2, 1950));
	CHECK(t.ReleaseCompany(1) == 1 && t.ReleaseCompany(1) == 0 && t.RoleOf(1, 1950) == Role::TransportOwner);
	CHECK(t.Sweep([](uint8_t, const Row &) { return Verdict::Keep; }) == 0);

	/* ---- Save and load: every value the same, in the order of the company number. ---- */
	t.Reset();
	t.SetRole(9, 1960, 1, 0, 700);
	t.SetDealer(4, 1951, true, 365);
	t.SetRole(12, 1949, 1, 0, 5);
	const std::vector<Record> recs = t.ToRecords();
	CHECK(recs.size() == 3 && recs[0].a == 4 && recs[1].a == 9 && recs[2].a == 12);
	CHECK(recs[0].b == 0 && recs[0].v1 == 1 && recs[0].v2 == 365 && recs[0].v5 == 1951);
	CHECK(recs[1].b == 1 && recs[1].v1 == 0 && recs[1].v2 == 700 && recs[1].v5 == 1960);
	Table u;
	for (const Record &r : recs) CHECK(u.FromRecord(r));
	CHECK(u.rows.size() == 3);
	CHECK(u.DealerOf(4, 1951) && u.RoleOf(9, 1960) == Role::IndustryOwner && u.RoleOf(12, 1949) == Role::IndustryOwner);
	CHECK(u.Find(9, 1960)->since_day == 700 && u.Find(4, 1951)->since_day == 365);
	CHECK(u.ToRecords().size() == 3 && u.ToRecords()[1].v2 == 700);

	/* The role is still there in the middle of the game: save, load into a fresh table, the verdicts are the same. */
	CHECK(u.RoleOf(9, 1960) == t.RoleOf(9, 1960) && u.DealerOf(4, 1951) == t.DealerOf(4, 1951));

	/* ---- An old savegame has no record: everybody is a transport owner. ---- */
	Table old;
	CHECK(old.ToRecords().empty());
	for (uint8_t c = 0; c < MAX_COMPANIES_N; c++) CHECK(old.RoleOf(c, Y) == Role::TransportOwner && !old.DealerOf(c, Y));

	/* ---- Records that make no sense are ignored. ---- */
	Table bad;
	{
		Record r; r.a = 15; r.b = 1; r.v5 = Y;
		CHECK(!bad.FromRecord(r)); // company number out of range
		r.a = 200;
		CHECK(!bad.FromRecord(r));
		Record s; s.a = 2; s.b = 9; s.v5 = Y;
		CHECK(!bad.FromRecord(s)); // not a role
		Record d; d.a = 2; d.b = 0; d.v1 = 0; d.v5 = Y;
		CHECK(!bad.FromRecord(d)); // the default needs no row
		CHECK(bad.rows.empty());
		Record neg; neg.a = 2; neg.b = 1; neg.v2 = -50; neg.v5 = Y;
		CHECK(bad.FromRecord(neg) && bad.Find(2, Y)->since_day == 0); // a negative day is cut to 0
		Record dealer_io; dealer_io.a = 6; dealer_io.b = 1; dealer_io.v1 = 1; dealer_io.v5 = Y;
		CHECK(bad.FromRecord(dealer_io) && !bad.DealerOf(6, Y) && bad.RoleOf(6, Y) == Role::IndustryOwner); // an industry owner is never a dealer
		Record dealer_to; dealer_to.a = 7; dealer_to.b = 0; dealer_to.v1 = 5; dealer_to.v5 = Y;
		CHECK(bad.FromRecord(dealer_to) && bad.DealerOf(7, Y)); // any non-zero value is "on"
	}

	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
