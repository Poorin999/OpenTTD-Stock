#include "central_bank.h"
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <vector>
#include "update_body.inc"
#include "check_body.inc"
static int fails=0, checks=0;
#define CHECK(c) do{checks++; if(!(c)){fails++; printf("FAIL line %d: %s\n",__LINE__,#c);}}while(0)

int main(){
	/* helpers */
	CHECK(PolicyUpperBp(0)==350); CHECK(PolicyLowerBp(0)==325); CHECK(PolicyPrimeBp(0)==650);
	CHECK(PolicyIorbBp(0)==340); CHECK(PolicyUpperBp(POLICY_SHIFT_MIN_BP)==25); CHECK(PolicyLowerBp(POLICY_SHIFT_MIN_BP)==0);
	CHECK(PolicyRoundMove(0,50)==0); CHECK(PolicyRoundMove(12,50)==0); CHECK(PolicyRoundMove(-12,50)==0);
	CHECK(PolicyRoundMove(13,50)==25); CHECK(PolicyRoundMove(-37,50)==-25); CHECK(PolicyRoundMove(38,50)==50);
	CHECK(PolicyRoundMove(300,50)==50); CHECK(PolicyRoundMove(-300,50)==-50);
	CHECK(PolicyRoundMove(300,75)==75); CHECK(PolicyRoundMove(60,75)==50); CHECK(PolicyRoundMove(90,75)==75);
	for(int g=-400; g<=400; g++) for(int ms : {50,75}){ int64_t m=PolicyRoundMove(g,ms); CHECK(m%25==0); CHECK(std::llabs(m)<=ms); }
	/* meetings: exactly 8 per 365 days, first day of each period, gap 45..46 */
	int cnt=0; int64_t prev=-1; int mn=999,mx=0;
	for(int64_t d=0; d<365*5; d++){ if(PolicyMeetingIndex(d)!=PolicyMeetingIndex(d-1)){ cnt++; if(prev>=0){mn=std::min<int>(mn,d-prev);mx=std::max<int>(mx,d-prev);} prev=d; } }
	CHECK(cnt==8*5+1-1+0 || cnt==8*5); printf("meetings in 5y: %d gap %d..%d\n",cnt,mn,mx);
	CHECK(mn>=45 && mx<=46);
	for(int64_t d=0; d<400; d++){ int64_t n=PolicyDaysToNextMeeting(d); CHECK(n>=1&&n<=46); CHECK(PolicyMeetingIndex(d+n)==PolicyMeetingIndex(d)+1); CHECK(n==1 || PolicyMeetingIndex(d+n-1)==PolicyMeetingIndex(d)); }

	/* scenario: steady 2.5% inflation, normal growth -> holds, no move */
	{ MonetaryPolicy p; int64_t idx=1'000'000; int moves=0, meet=0;
	  for(int64_t day=0; day<365*4; day++){ idx = 1'000'000 * 1.0; /* placeholder */ }
	  idx=1'000'000;
	  for(int64_t day=0; day<365*4; day++){ idx += 1'000'000*250/10000/365*1 ; /* ~2.5%/y linear */
	     int64_t v=1'000'000'000LL+ day*1'000'000LL*6/365*10; bool m=p.Update(day,idx, v*1, v*1 - v*6/100*90/365,0,false); if(m){meet++; if(p.last_step_bp) moves++; CHECK(p.shift_bp%25==0);} }
	  printf("steady: meetings %d moves %d shift %lld infl %lld\n",meet,moves,(long long)p.shift_bp,(long long)p.inflation_bp);
	  CHECK(meet>=30 && meet<=32); }

	/* scenario: inflation 8%: hikes in 25/50 steps, never faster than 50 per meeting, never above ceiling, emergency 75 */
	{ MonetaryPolicy p; double idx=1e6; int64_t maxstep=0; int emerg_seen=0; int64_t maxshift=-9999; int hikes=0;
	  for(int64_t day=0; day<365*6; day++){ idx*=1.0+0.15/365; /* 15% a year */
	     bool m=p.Update(day,(int64_t)idx,1'000'000'000LL,1'000'000'000LL,0,false);
	     if(m){ maxstep=std::max<int64_t>(maxstep,std::llabs(p.last_step_bp)); if(p.emergency)emerg_seen++; if(p.last_step_bp>0)hikes++; CHECK(p.shift_bp%25==0); }
	     maxshift=std::max(maxshift,p.shift_bp); CHECK(p.shift_bp>=POLICY_SHIFT_MIN_BP && p.shift_bp<=POLICY_EMERGENCY_SHIFT_MAX_BP); }
	  printf("15%% inflation: maxstep %lld emergency meetings %d hikes %d max shift %lld\n",(long long)maxstep,emerg_seen,hikes,(long long)maxshift);
	  CHECK(maxstep<=75); CHECK(emerg_seen>0); CHECK(maxshift>POLICY_SHIFT_MAX_BP); }

	/* scenario: deflation / weak economy -> cuts down to the zero lower bound and stops there */
	{ MonetaryPolicy p; double idx=1e6; int64_t minshift=9999;
	  for(int64_t day=0; day<365*8; day++){ idx*=1.0-0.02/365;
	     p.Update(day,(int64_t)idx,800'000'000LL,1'000'000'000LL,-3000,true);
	     minshift=std::min(minshift,p.shift_bp); }
	  printf("deflation: min shift %lld upper %lld lower %lld\n",(long long)minshift,(long long)PolicyUpperBp(minshift),(long long)PolicyLowerBp(minshift));
	  CHECK(minshift==POLICY_SHIFT_MIN_BP); CHECK(PolicyUpperBp(minshift)==25); }

	/* old savegame shift that is not on 25 bp is snapped at the next meeting */
	{ MonetaryPolicy p; p.shift_bp=137; p.prev_index_ppm=1'000'000; p.last_decision_day=0; p.measured=true; p.inflation_bp=250;
	  bool m=p.Update(60,1'000'000+1'000'000*250/10000*60/365,1,1,0,false); CHECK(m); CHECK(p.shift_bp%25==0); printf("snap: 137 -> %lld\n",(long long)p.shift_bp); }

	/* jobs mandate: strong jobs push the wanted rate up, weak jobs pull it down (same inflation) */
	{ MonetaryPolicy a,b; for(MonetaryPolicy* p:{&a,&b}){ p->prev_index_ppm=1'000'000; p->last_decision_day=0; p->measured=true; p->inflation_bp=250; }
	  int64_t idx=1'000'000+1'000'000*250/10000*46/365;
	  a.Update(46,idx,1'000'000,1'000'000,+4000,true); b.Update(46,idx,1'000'000,1'000'000,-4000,true);
	  printf("jobs strong: wanted gap %lld ; weak: %lld\n",(long long)a.wanted_gap_bp,(long long)b.wanted_gap_bp);
	  CHECK(a.wanted_gap_bp>b.wanted_gap_bp); }


	/* market push: sign, size, clamp */
	CHECK(PolicyGoldPushPpm(25,0)==-5000); CHECK(PolicyGoldPushPpm(25,25)==-10000); CHECK(PolicyGoldPushPpm(-25,-25)==10000);
	CHECK(PolicyGoldPushPpm(0,0)==0); CHECK(PolicyGoldPushPpm(75,75)==-30000); CHECK(PolicyGoldPushPpm(5000,5000)==-POLICY_GOLD_PUSH_MAX_PPM);
	CHECK(PolicySharePushPpm(25,25)==-5000); CHECK(PolicySharePushPpm(-50,-50)==10000); CHECK(PolicySharePushPpm(-9999,0)==POLICY_SHARE_PUSH_MAX_PPM);
	/* surprise: a meeting that follows its own bias is half surprise; a first meeting has no expectation */
	{ MonetaryPolicy p; p.prev_index_ppm=1'000'000; p.last_decision_day=0; double idx=1e6; int64_t sum_exp=0;
	  for(int64_t day=1; day<365*3; day++){ idx*=1.0+0.09/365; if(p.Update(day,(int64_t)idx,1'000'000,1'000'000,0,false)){ CHECK(std::llabs(p.last_surprise_bp)<=std::llabs(p.last_step_bp)+75); sum_exp++; } }
	  printf("surprise meetings %lld last surprise %lld step %lld\n",(long long)sum_exp,(long long)p.last_surprise_bp,(long long)p.last_step_bp); }

	/* unscheduled: jobs crash cuts 50 bp once, needs 14 days, needs a NEW report, never again for the same report */
	{ MonetaryPolicy p; p.shift_bp=100; p.prev_index_ppm=1'000'000; p.last_decision_day=100; p.measured=true; p.inflation_bp=250;
	  CHECK(!p.CheckUnscheduled(105,1'000'000,-9000,true,104));          /* too early */
	  CHECK(!p.CheckUnscheduled(120,1'000'000,-9000,true,90));           /* report older than the last meeting */
	  CHECK(!p.CheckUnscheduled(120,1'000'000,-1000,true,110));          /* not a crash */
	  CHECK(p.CheckUnscheduled(120,1'000'000,-9000,true,110));           /* crash */
	  CHECK(p.shift_bp==50); CHECK(p.last_unscheduled); CHECK(p.last_step_bp==-50); CHECK(p.last_surprise_bp==-50); CHECK(p.last_decision_day==120); CHECK(p.log.front().unscheduled);
	  CHECK(!p.CheckUnscheduled(121,1'000'000,-9000,true,110));          /* same report, and too soon */
	  CHECK(!p.CheckUnscheduled(140,1'000'000,-9000,true,110));          /* same report again */
	  CHECK(p.CheckUnscheduled(140,1'000'000,-9000,true,135));           /* a new crash report */
	  CHECK(p.shift_bp==0); }
	/* unscheduled cut stops at the zero lower bound */
	{ MonetaryPolicy p; p.shift_bp=POLICY_SHIFT_MIN_BP; p.prev_index_ppm=1'000'000; p.last_decision_day=0; p.measured=true;
	  CHECK(!p.CheckUnscheduled(50,1'000'000,-9000,true,40)); CHECK(p.shift_bp==POLICY_SHIFT_MIN_BP); }
	/* unscheduled hike on an inflation spike: needs inflation already high at the last meeting */
	{ MonetaryPolicy p; p.shift_bp=300; p.prev_index_ppm=1'000'000; p.last_decision_day=0; p.measured=true; p.inflation_bp=300;
	  int64_t idx=1'000'000+1000000LL*3000/10000*20/365;                  /* 30% a year over 20 days */
	  CHECK(!p.CheckUnscheduled(20,idx,0,false,-1));                      /* last meeting inflation was calm */
	  p.inflation_bp=1200; CHECK(p.CheckUnscheduled(20,idx,0,false,-1)); CHECK(p.shift_bp==375); CHECK(p.last_step_bp==75); }
	/* a spike hike never goes above the ceiling that applies */
	{ MonetaryPolicy p; p.shift_bp=POLICY_SHIFT_MAX_BP; p.prev_index_ppm=1'000'000; p.last_decision_day=0; p.measured=true; p.inflation_bp=900;
	  int64_t idx=1'000'000+1000000LL*3000/10000*20/365; CHECK(!p.CheckUnscheduled(20,idx,0,false,-1)); CHECK(p.shift_bp==POLICY_SHIFT_MAX_BP); }
	/* meetings keep their schedule after an unscheduled one */
	{ MonetaryPolicy p; p.shift_bp=100; p.prev_index_ppm=1'000'000; p.last_decision_day=100; p.measured=true; p.inflation_bp=250;
	  p.CheckUnscheduled(120,1'000'000,-9000,true,110);
	  int64_t nextm=120+PolicyDaysToNextMeeting(120); CHECK(!p.Update(nextm-1,1'000'000,1,1,0,false)); CHECK(p.Update(nextm,1'000'000,1,1,0,false)); CHECK(!p.last_unscheduled); }

	/* news: a crisis in the news cuts 50 bp after the minimum days; waits until then; expires; one news = one meeting */
	{ MonetaryPolicy p; p.shift_bp=100; p.prev_index_ppm=1'000'000; p.last_decision_day=100; p.measured=true; p.inflation_bp=250;
	  p.RequestNewsMeeting(PolicyNewsTrigger::Crisis,105);
	  CHECK(!p.CheckUnscheduled(108,1'000'000,0,false,-1)); CHECK(p.news_trigger==PolicyNewsTrigger::Crisis);   /* too soon, kept */
	  CHECK(p.CheckUnscheduled(115,1'000'000,0,false,-1)); CHECK(p.shift_bp==50); CHECK(p.last_reason==PolicyUnscheduledReason::CrisisNews);
	  CHECK(p.news_trigger==PolicyNewsTrigger::None); CHECK(p.last_surprise_bp==-50);
	  CHECK(!p.CheckUnscheduled(140,1'000'000,0,false,-1)); }
	{ MonetaryPolicy p; p.shift_bp=100; p.prev_index_ppm=1'000'000; p.last_decision_day=100; p.measured=true; p.inflation_bp=250;
	  p.RequestNewsMeeting(PolicyNewsTrigger::Crisis,101);
	  CHECK(!p.CheckUnscheduled(116,1'000'000,0,false,-1)); CHECK(p.news_trigger==PolicyNewsTrigger::None); CHECK(p.shift_bp==100);  /* 15 days old: forgotten */ }
	{ MonetaryPolicy p; p.shift_bp=POLICY_SHIFT_MIN_BP; p.prev_index_ppm=1'000'000; p.last_decision_day=0; p.measured=true;
	  p.RequestNewsMeeting(PolicyNewsTrigger::Crisis,20); CHECK(!p.CheckUnscheduled(20,1'000'000,0,false,-1)); CHECK(p.news_trigger==PolicyNewsTrigger::None); }  /* at the floor */
	/* inflation shock news: only when inflation is above 4% */
	{ MonetaryPolicy p; p.shift_bp=100; p.prev_index_ppm=1'000'000; p.last_decision_day=0; p.measured=true; p.inflation_bp=250;
	  p.RequestNewsMeeting(PolicyNewsTrigger::InflationShock,20); CHECK(!p.CheckUnscheduled(20,1'000'000,0,false,-1)); CHECK(p.shift_bp==100); CHECK(p.news_trigger==PolicyNewsTrigger::None);
	  p.inflation_bp=600; p.RequestNewsMeeting(PolicyNewsTrigger::InflationShock,20); CHECK(p.CheckUnscheduled(20,1'000'000,0,false,-1)); CHECK(p.shift_bp==150); CHECK(p.last_reason==PolicyUnscheduledReason::InflationNews); }
	/* data beats news: a jobs crash and crisis news together give one cut, the crash reason */
	{ MonetaryPolicy p; p.shift_bp=100; p.prev_index_ppm=1'000'000; p.last_decision_day=0; p.measured=true; p.inflation_bp=250;
	  p.RequestNewsMeeting(PolicyNewsTrigger::Crisis,20); CHECK(p.CheckUnscheduled(20,1'000'000,-9000,true,15)); CHECK(p.shift_bp==50); CHECK(p.last_reason==PolicyUnscheduledReason::JobsCrash); }
	/* news from a different game (day in the future) is dropped */
	{ MonetaryPolicy p; p.shift_bp=100; p.prev_index_ppm=1'000'000; p.last_decision_day=0; p.measured=true; p.RequestNewsMeeting(PolicyNewsTrigger::Crisis,500);
	  CHECK(!p.CheckUnscheduled(20,1'000'000,0,false,-1)); CHECK(p.news_trigger==PolicyNewsTrigger::None); }
	/* IORB arithmetic: 100,000 at upper 3.50% - 0.10% = 3.40% a day, and always below the cheapest loan */
	CHECK(100'000LL*PolicyIorbBp(0)/BANK_INTEREST_DIVISOR==9);
	for(int64_t sh=POLICY_SHIFT_MIN_BP; sh<=POLICY_EMERGENCY_SHIFT_MAX_BP; sh+=25) CHECK(PolicyIorbBp(sh) <= BankShiftedRateBp(BANK_RATE_GOOD_MIN_BP,sh));

	/* loan rate with the news spread: adds to the risk rate, stays inside floor / ceiling, never touches the penalty rate, never below IORB */
	{ BankAccount a0; a0.debt=1'000'000; BankAccount a1=a0, a2=a0, a3=a0, a4=a0;
	  BankDayResult r0=BankSettleDay(a0, 10'000'000, 5'000'000, 1, 0, 0);
	  BankDayResult r1=BankSettleDay(a1, 10'000'000, 5'000'000, 1, 0, 60);
	  BankDayResult r2=BankSettleDay(a2, 10'000'000, 5'000'000, 1, 0, -500); /* a negative spread counts as 0 */
	  BankDayResult r3=BankSettleDay(a3, 10'000'000, 5'000'000, 1, 0, 100000); /* far above: the ceiling holds */
	  CHECK(r1.rate_bp==r0.rate_bp+60); CHECK(r2.rate_bp==r0.rate_bp); CHECK(r3.rate_bp==BANK_RATE_CEIL_BP); CHECK(r3.rate_bp<BankPenaltyRateBp(1,0));
	  BankDayResult r4=BankSettleDay(a4, 0, 5'000'000, 1, 0, 150); /* no cash: default, penalty rate without the spread */
	  CHECK(r4.defaulted && r4.rate_bp==BankPenaltyRateBp(1,0));
	  for(int64_t sh=POLICY_SHIFT_MIN_BP; sh<=POLICY_EMERGENCY_SHIFT_MAX_BP; sh+=25) for(int64_t sp : {0,25,150}) CHECK(PolicyIorbBp(sh) <= BankShiftedRateBp(BANK_RATE_GOOD_MIN_BP+sp,sh)); }

	/* loan rates still the same numbers at shift 0 */
	CHECK(BankShiftedRateBp(BANK_RATE_GOOD_MIN_BP,0)==350); CHECK(BankShiftedRateBp(BANK_RATE_POOR_MAX_BP,0)==1200);
	CHECK(BankPolicyCreditPercent(0)==50);
	printf("%d checks, %d failed\n",checks,fails); return fails?1:0; }
