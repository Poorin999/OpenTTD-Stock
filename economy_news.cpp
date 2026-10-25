/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_news.cpp The monthly Transport Payrolls report and its own window. See economy_news.h. */

#include "stdafx.h"
#include <chrono>
#include "economy_news.h"
#include "central_bank.h"
#include "gold_reserve.h"
#include "core/format.hpp"
#include "company_base.h"
#include "company_func.h"
#include "gfx_func.h"
#include "news_func.h"
#include "strings_func.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"
#include "game_time/game_time_manager.h"
#include "timer/timer.h"
#include "timer/timer_window.h"

#include "widgets/central_bank_widget.h"

#include "table/strings.h"

#include "safeguards.h"

PayrollNews _payroll_news;

/** True: the window shows Thai text. False: the same window in English only. Change it here and build again. */
static constexpr bool PN_USE_THAI = true;

static std::string PnText(const char *thai, const char *english)
{
	return PN_USE_THAI ? std::string(thai) : std::string(english);
}

/** Window number of the news window inside the class #WC_CENTRAL_BANK (0 bank, 1 statement, 2 past statements, 3 buy gold, 4 policy). */
static constexpr int PN_WINDOW_NUMBER = 5;

/** A push in ppm as a signed percentage with two decimals, for example -0.50%. Done by hand so that a value between -1% and 0 keeps its minus sign. */
static std::string PnPpm(int64_t ppm)
{
	const int64_t a = ppm < 0 ? -ppm : ppm;
	return fmt::format("{}{}.{:02}%", ppm < 0 ? "-" : "+", a / 10'000, a % 10'000 / 100);
}

/** Vehicles of all companies now: every primary vehicle (a train counts once, not per wagon). */
static int64_t CountAllVehicles()
{
	int64_t count = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!Company::IsValidID(v->owner) || v->Previous() != nullptr || !v->IsPrimaryVehicle() || !IsCompanyBuildableVehicleType(v->type)) continue;
		count++;
	}
	return count;
}

/** Count the vehicles of today; part of the average of the running period. */
void PayrollNews::Sample()
{
	this->sum += CountAllVehicles();
	this->samples++;
}

/**
 * The report at the end of a period: actual, forecast, surprise, and the push on the gold demand. The first period only sets the baseline.
 * @param day Game day of the report.
 */
void PayrollNews::Release(int64_t day)
{
	if (this->samples <= 0) return; // Nothing was counted (a load in the middle of a period): wait for the next one.
	PayrollRelease r;
	r.day = day;
	r.avg_x100 = this->RunningAvgX100();
	this->sum = 0;
	this->samples = 0;
	this->last_day = day;

	if (this->releases.empty()) {
		r.baseline = true; // The first period: only the baseline.
	} else {
		const PayrollRelease &prev = this->releases.front();
		r.actual_x100 = r.avg_x100 - prev.avg_x100;
		r.forecast_x100 = PayrollForecastX100(this->releases);
		const bool have_history = !prev.baseline; // A forecast needs at least one earlier real report.
		r.gold_ppm = have_history ? PayrollGoldPushPpm(r.Surprise(), prev.avg_x100) : 0;
		if (r.gold_ppm != 0 && _gold_reserve.GetPriceCents() > 0) {
			_gold_reserve.pressure_ppm = std::clamp<int64_t>(_gold_reserve.pressure_ppm + r.gold_ppm,
					-GoldReserveManager::PRESSURE_MAX_PPM, GoldReserveManager::PRESSURE_MAX_PPM);
		}
	}
	this->releases.push_front(r);
	while (this->releases.size() > PAYROLL_KEEP) this->releases.pop_back();

	if (!r.baseline) {
		const int64_t jobs = r.actual_x100 * PAYROLL_JOBS_PER_VEHICLE / 100;
		const int64_t surprise = r.Surprise() * PAYROLL_JOBS_PER_VEHICLE / 100;
		AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("Transport Payrolls: {:+} jobs (surprise {:+}). Gold price pressure {}.",
				jobs, surprise, PnPpm(r.gold_ppm))), NewsType::General, NewsStyle::Small, {});
		InvalidateWindowData(WC_CENTRAL_BANK, PN_WINDOW_NUMBER);
	}
}

/** Once per game day: count the vehicles; at the end of a period make the report. */
static const GameTimeSubscription _payroll_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	const int64_t day = static_cast<int64_t>(now.total_minutes / GameTimeManager::MINUTES_PER_DAY);
	_payroll_news.Sample();
	if (day > 0 && day % PAYROLL_PERIOD_DAYS == 0 && day != _payroll_news.last_day) _payroll_news.Release(day);
});

/* ------------------------------------------------------------------------------------------------------------------ */
/* The news window                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------------ */

static int PnRowHeight() { return GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3); }

/** Jobs from a number in vehicles times 100, as text with a sign. */
static std::string PnJobs(int64_t x100)
{
	return fmt::format("{:+}", x100 * PAYROLL_JOBS_PER_VEHICLE / 100);
}

static void DrawPayrollNews(const Rect &r)
{
	const int left = r.left + ScaleGUITrad(10);
	const int right = r.right - ScaleGUITrad(10);
	const int row_h = PnRowHeight();
	int y = r.top + ScaleGUITrad(8);
	const int bottom = r.bottom - ScaleGUITrad(4);
	auto fits = [&]() { return y + row_h <= bottom; };
	auto row = [&](const std::string &label, const std::string &value, TextColour colour = TC_WHITE) {
		if (!fits()) return;
		DrawString(left, right, y, label, TC_WHITE, SA_LEFT);
		DrawString(left, right, y, value, colour, SA_RIGHT);
		y += row_h;
	};

	if (fits()) { DrawString(left, right, y, PnText("ดัชนีการจ้างงานขนส่ง (Transport Payrolls)", "Transport Payrolls"), TC_LIGHT_BLUE, SA_LEFT); y += row_h; }
	if (fits()) { DrawString(left, right, y, PnText("นับรถทั้งหมดของทุกบริษัท 1 คัน = 1,000 ตำแหน่งงาน ใช้ค่าเฉลี่ยทั้งเดือน", "All vehicles of all companies, 1 vehicle = 1,000 jobs, monthly average"), TC_GREY, SA_LEFT); y += row_h + ScaleGUITrad(4); }

	const int64_t day = static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
	row(PnText("รายงานครั้งต่อไปใน", "Next report in"), fmt::format("{} {}", PayrollNews::DaysToNext(day), PnText("วัน", "days")));
	row(PnText("การจ้างงานเฉลี่ยงวดนี้ (ถึงตอนนี้)", "Employment this period so far"), fmt::format("{} {}", _payroll_news.RunningAvgX100() * PAYROLL_JOBS_PER_VEHICLE / 100, PnText("ตำแหน่ง", "jobs")), TC_GREY);
	const int64_t pressure = _gold_reserve.pressure_ppm;
	row(PnText("แรงกดดันราคาทองตอนนี้", "Gold demand pressure now"), PnPpm(pressure), pressure < 0 ? TC_RED : TC_GREEN);
	y += ScaleGUITrad(6);

	/* The table of the reports: day, actual, forecast, surprise, push on gold. */
	const int width = right - left;
	const int col_actual = left + width * 22 / 100;
	const int col_forecast = left + width * 44 / 100;
	const int col_surprise = left + width * 66 / 100;
	const int col_gold = left + width * 84 / 100;
	if (fits()) {
		DrawString(left, col_actual - 4, y, PnText("วันที่", "Day"), TC_LIGHT_BLUE, SA_LEFT);
		DrawString(col_actual, col_forecast - 4, y, PnText("จริง", "Actual"), TC_LIGHT_BLUE, SA_RIGHT);
		DrawString(col_forecast, col_surprise - 4, y, PnText("คาดการณ์", "Forecast"), TC_LIGHT_BLUE, SA_RIGHT);
		DrawString(col_surprise, col_gold - 4, y, PnText("ส่วนต่าง", "Surprise"), TC_LIGHT_BLUE, SA_RIGHT);
		DrawString(col_gold, right, y, PnText("ผลต่อทอง", "Gold"), TC_LIGHT_BLUE, SA_RIGHT);
		y += row_h;
	}
	if (_payroll_news.releases.empty() && fits()) {
		DrawString(left, right, y, PnText("ยังไม่มีรายงาน (งวดแรกใช้เป็นฐาน)", "No report yet (the first period sets the baseline)"), TC_GREY, SA_LEFT);
		y += row_h;
	}
	for (const PayrollRelease &rel : _payroll_news.releases) {
		if (!fits()) break;
		DrawString(left, col_actual - 4, y, fmt::format("D{}", rel.day + 1), TC_WHITE, SA_LEFT);
		if (rel.baseline) {
			DrawString(col_actual, right, y, PnText("ฐาน (baseline)", "baseline"), TC_GREY, SA_RIGHT);
		} else {
			const int64_t s = rel.Surprise();
			DrawString(col_actual, col_forecast - 4, y, PnJobs(rel.actual_x100), rel.actual_x100 >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
			DrawString(col_forecast, col_surprise - 4, y, PnJobs(rel.forecast_x100), TC_GREY, SA_RIGHT);
			DrawString(col_surprise, col_gold - 4, y, PnJobs(s), s > 0 ? TC_GREEN : (s < 0 ? TC_RED : TC_GREY), SA_RIGHT);
			DrawString(col_gold, right, y, PnPpm(rel.gold_ppm),
					rel.gold_ppm > 0 ? TC_GREEN : (rel.gold_ppm < 0 ? TC_ORANGE : TC_GREY), SA_RIGHT);
		}
		y += row_h;
	}
	y += ScaleGUITrad(6);
	if (fits()) { DrawString(left, right, y, PnText("ตัวเลขแรงกว่าคาด = เศรษฐกิจร้อน ทองมักลง  แย่กว่าคาด ทองมักขึ้น", "Better than forecast = hot economy, gold tends to fall; worse, gold tends to rise"), TC_GREY, SA_LEFT); }
}

/** The news window: its own window, so it can stay open next to the gold chart. */
struct PayrollNewsWindow : Window {
	static constexpr int REPORT_ROWS = 20; ///< Rows of the panel: heading, note, three lines, a table heading and the reports.

	/** Redraw regularly: the running numbers change all the time. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(500), [this](auto) {
		this->SetDirty();
	}};

	PayrollNewsWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(PN_WINDOW_NUMBER);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_PN_PANEL) return;
		size.width = std::max<uint>(size.width, ScaleGUITrad(560));
		size.height = std::max<uint>(size.height, PnRowHeight() * REPORT_ROWS + ScaleGUITrad(24));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_PN_PANEL) DrawPayrollNews(r);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_payroll_news_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_PN_CAPTION), SetStringTip(STR_PAYROLL_NEWS_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_PN_PANEL), SetMinimalSize(380, 120), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
};

static WindowDesc _payroll_news_desc(
	WDP_AUTO, "payroll_news", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_payroll_news_widgets
);

void ShowPayrollNewsWindow()
{
	if (BringWindowToFrontById(WC_CENTRAL_BANK, PN_WINDOW_NUMBER) != nullptr) return;
	new PayrollNewsWindow(_payroll_news_desc);
}

bool IsPayrollNewsWindowOpen()
{
	return FindWindowById(WC_CENTRAL_BANK, PN_WINDOW_NUMBER) != nullptr;
}
