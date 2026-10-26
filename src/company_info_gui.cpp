/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file company_info_gui.cpp The company window of the mod (design of the owner's screenshot): the history of the company with the face of the
 * president, the net worth, the vehicles of the company with the profit of the year, the income and costs of the last months, the company colour,
 * and the buttons. The IPO button is only a placeholder: the IPO system does not exist yet.
 *
 * What the window does itself: rename the company and the president, set the company colour, open the finances, the vehicle list, the
 * infrastructure window and the stock terminal. What only the ORIGINAL company window can do (new face, build or move the headquarters,
 * shares) is opened with ShowOriginalCompanyWindow().
 */

#include "stdafx.h"
#include "company_info_gui.h"
#include "company_info_math.h"
#include "central_bank.h"
#include "wallet_base.h"
#include "stock_gui.h"
#include "openttd.h"
#include "game_time/game_time_manager.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include "command_func.h"
#include "company_base.h"
#include "company_cmd.h"
#include "company_func.h"
#include "company_gui.h"
#include "company_manager_face.h"
#include "core/format.hpp"
#include "economy_func.h"
#include "gfx_func.h"
#include "livery.h"
#include "palette_func.h"
#include "strings_func.h"
#include "textbuf_gui.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_window.h"
#include "vehicle_base.h"
#include "vehicle_gui.h"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"

#include "gfx_func.h"
#include "object_type.h"
#include "tilehighlight_func.h"
#include "tilehighlight_type.h"

#include "table/sprites.h"
#include "table/strings.h"

#include "safeguards.h"

bool _company_info_use_original = false;

/* ------------------------------------------------------------------------------------------------------------------------------------ */
/* Income and costs per game month (for the chart). Not saved: the chart starts empty after a load and fills as game days pass.        */
/* ------------------------------------------------------------------------------------------------------------------------------------ */

namespace {

/** What one company earned and spent in one game month. */
struct MonthBucket {
	int64_t income = 0;  ///< Income of the month.
	int64_t expense = 0; ///< Costs of the month (positive).
};

static constexpr size_t MONTHS_KEPT = 12; ///< How many months are kept per company.
static constexpr int CHART_MONTHS = 5;    ///< How many months the chart shows.

std::array<std::map<int32_t, MonthBucket>, MAX_COMPANIES> _month_buckets;
std::array<int64_t, MAX_COMPANIES> _seen_income{};  ///< Income of the quarter at the last game day.
std::array<int64_t, MAX_COMPANIES> _seen_expense{}; ///< Costs of the quarter at the last game day.
std::array<bool, MAX_COMPANIES> _seen_valid{};      ///< True when the two numbers above were read.
uint64_t _seen_minutes = 0;                         ///< Game minute of the last game day.

/** Once per game day: add what the company earned and spent since yesterday to the bucket of this month. */
const GameTimeSubscription _company_month_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;

	if (now.total_minutes < _seen_minutes) {
		/* The clock went back (a new game or an older savegame): forget everything. */
		for (auto &m : _month_buckets) m.clear();
		_seen_valid.fill(false);
	} else if (_seen_minutes != 0 && now.total_minutes != _seen_minutes + static_cast<uint64_t>(GameTimeManager::MINUTES_PER_DAY)) {
		/* Days were skipped (a savegame was loaded, or stock_skip): the old totals no longer fit, read them again. */
		_seen_valid.fill(false);
	}
	_seen_minutes = now.total_minutes;

	const int32_t key = companyinfo::MonthKey(now.year, now.day);
	for (const Company *c : Company::Iterate()) {
		const size_t i = c->index.base();
		const int64_t income = static_cast<int64_t>(c->cur_economy.income);
		const int64_t expense = -static_cast<int64_t>(c->cur_economy.expenses);
		if (_seen_valid[i]) {
			MonthBucket &b = _month_buckets[i][key];
			b.income += companyinfo::BucketDelta(income, _seen_income[i]);
			b.expense += companyinfo::BucketDelta(expense, _seen_expense[i]);
			while (_month_buckets[i].size() > MONTHS_KEPT) _month_buckets[i].erase(_month_buckets[i].begin());
		}
		_seen_income[i] = income;
		_seen_expense[i] = expense;
		_seen_valid[i] = true;
	}
});

/** True: the window shows Thai text. False: English only. Change it here and build again. */
constexpr bool USE_THAI = true;

std::string CiText(const char *thai, const char *english)
{
	return USE_THAI ? std::string(thai) : std::string(english);
}

std::string CiMoney(int64_t v)
{
	return GetString(STR_JUST_CURRENCY_LONG, v);
}

/** The short names of the months of the chart. */
const char *const MONTH_TH[companyinfo::MONTHS_PER_YEAR] = {"ม.ค.", "ก.พ.", "มี.ค.", "เม.ย.", "พ.ค.", "มิ.ย.", "ก.ค.", "ส.ค.", "ก.ย.", "ต.ค.", "พ.ย.", "ธ.ค."};
const char *const MONTH_EN[companyinfo::MONTHS_PER_YEAR] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/** The 16 colours of the game, in the order of the enum. */
const char *const COLOUR_NAMES[16] = {"Dark Blue", "Pale Green", "Pink", "Yellow", "Red", "Light Blue", "Green", "Dark Green",
	"Blue", "Cream", "Mauve", "Purple", "Orange", "Brown", "Grey", "White"};
/** True for the colours that are light enough for black text. */
const bool COLOUR_IS_LIGHT[16] = {false, true, true, true, false, true, false, false, false, true, true, false, true, false, false, true};

/** Widgets of the company window (here, not in src/widgets: the script API generator reads that folder). */
enum CompanyInfoWidgets : WidgetID {
	WID_CIW_CAPTION,
	WID_CIW_PANEL,
};

/** The window number of a company window inside the class WC_CENTRAL_BANK (0-6 are the bank windows, 100000 + the index of the town are the town finance windows): this + the company. */
constexpr int CI_WINDOW_BASE = 200000;

/** Height of one text row of the window. It follows the font (Thai letters with tone marks are taller), so nothing is drawn on top of the next row. */
int InfoRow()
{
	return GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(5);
}

/** The texts of the tabs and of the buttons (Thai / English). Shared by the size calculation and by the drawing. */
const char *const TAB_TH[4] = {"ข้อมูลบริษัท", "การเงิน", "ยานพาหนะ", "โครงสร้างพื้นฐาน"};
const char *const TAB_EN[4] = {"Company", "Finance", "Vehicles", "Infrastructure"};
const char *const BTN_TH[4] = {"เปลี่ยนชื่อบริษัท", "ชื่อประธาน", "ซื้อหุ้น", "IPO บริษัท"};
const char *const BTN_EN[4] = {"Rename company", "President name", "Stocks", "Company IPO"};
constexpr int BTN_IPO = 3; ///< Index of the IPO button (orange).

/** All the rectangles of the window, from the rectangle of the panel. Draw and click use the same numbers. */
struct InfoLayout {
	Rect tabs[4];
	Rect date;
	Rect profile, ops, chart, colours;
	Rect face, face_btn, hq_btn, net_box;
	int mid_left = 0, mid_right = 0;
	Rect swatch[16];
	Rect btns[4];
	Rect footer;
	int hdr = 0; ///< Height of the title strip of a card.
	int pad = 0;
	int needed_w = 0; ///< The smallest width at which every text fits.
	int needed_h = 0; ///< The smallest height at which every text fits. A bigger window gives the extra height to the chart.
};

/**
 * Work out all the rectangles. Every height comes from the height of the font, not from fixed pixels, so a tall font (Thai) or a bigger GUI scale
 * makes the cards taller instead of cutting the text. The window can be made bigger by hand with the resize box; it can not be made smaller than needed_w x needed_h.
 * @param r The panel.
 * @param net_text_w Width of the net worth number in the large font.
 */
InfoLayout MakeInfoLayout(const Rect &r, int net_text_w)
{
	InfoLayout l;
	const int pad = ScaleGUITrad(8);
	const int gap = ScaleGUITrad(8);
	const int fh = GetCharacterHeight(FS_NORMAL);
	const int fl = GetCharacterHeight(FS_LARGE);
	const int fs = GetCharacterHeight(FS_SMALL);
	const int row = InfoRow();
	l.pad = pad;
	l.hdr = std::max(ScaleGUITrad(18), fh + ScaleGUITrad(6));
	const int left = r.left + pad;
	const int right = r.right - pad;
	int y = r.top + pad;

	/* The tabs (as wide as their text) and the date. */
	const int tab_h = std::max(ScaleGUITrad(22), fh + ScaleGUITrad(10));
	const int tab_min[4] = {92, 72, 86, 116};
	int x = left;
	for (int i = 0; i < 4; i++) {
		const int w = std::max(ScaleGUITrad(tab_min[i]), static_cast<int>(GetStringBoundingBox(CiText(TAB_TH[i], TAB_EN[i])).width) + ScaleGUITrad(24));
		l.tabs[i] = {x, y, x + w, y + tab_h};
		x += w + ScaleGUITrad(4);
	}
	const int date_w = std::max<int>(ScaleGUITrad(170), GetStringBoundingBox(GetString(STR_JUST_DATE_LONG, TimerGameCalendar::date)).width + ScaleGUITrad(8));
	l.date = {std::max(x, right - date_w), y, right, y + tab_h};
	const int tabs_w = (x - left) + date_w;
	y += tab_h + gap;

	/* The card of the history: the face column, the middle column and the box of the net worth. */
	const int face_size = ScaleGUITrad(72);
	const int fb_h = std::max(ScaleGUITrad(20), fh + ScaleGUITrad(8));
	const int hq_h = std::max(ScaleGUITrad(22), fh + ScaleGUITrad(8));
	const int face_col = std::max(ScaleGUITrad(92), static_cast<int>(GetStringBoundingBox(CiText("เลือกใบหน้าใหม่", "New face")).width) + ScaleGUITrad(16));
	const int net_w = std::max(ScaleGUITrad(190), net_text_w + 2 * pad);
	const int col_face_h = face_size + ScaleGUITrad(4) + 2 * row + ScaleGUITrad(6) + fb_h;
	const int col_mid_h = fl + ScaleGUITrad(2) + row + ScaleGUITrad(4) + 3 * row + ScaleGUITrad(6) + hq_h;
	const int col_net_h = ScaleGUITrad(6) + row + fl + ScaleGUITrad(6) + 2 * row + ScaleGUITrad(5) + row + ScaleGUITrad(6);
	const int profile_h = l.hdr + pad + std::max({col_face_h, col_mid_h, col_net_h}) + pad;
	const int mid_min_w = ScaleGUITrad(210);
	l.profile = {left, y, right, y + profile_h};
	y += profile_h + gap;

	/* The row of the vehicles table and of the chart. */
	const int ops_h = l.hdr + ScaleGUITrad(6) + 6 * row + ScaleGUITrad(3) + pad;
	const int chart_min_h = l.hdr + ScaleGUITrad(6) + fh + ScaleGUITrad(8) + ScaleGUITrad(60) + fs + ScaleGUITrad(4) + pad;
	const int row2_min = std::max(ops_h, chart_min_h);

	/* The card of the colours: a line of text and two rows of eight squares. */
	int sw_text_w = 0;
	for (const char *name : COLOUR_NAMES) sw_text_w = std::max(sw_text_w, static_cast<int>(GetStringBoundingBox(name, FS_SMALL).width));
	const int sw_gap = ScaleGUITrad(6);
	const int sw_min_w = sw_text_w + ScaleGUITrad(6);
	const int sw_h = std::max(ScaleGUITrad(22), fs + ScaleGUITrad(10));
	const int col_h = l.hdr + ScaleGUITrad(8) + row + ScaleGUITrad(8) + 2 * sw_h + sw_gap + pad;

	/* The buttons and the line at the bottom. */
	const int btn_h = std::max(ScaleGUITrad(26), fh + ScaleGUITrad(10));
	const int btn_gap = ScaleGUITrad(6);
	int btn_min_w = 0;
	for (int i = 0; i < 4; i++) btn_min_w = std::max(btn_min_w, static_cast<int>(GetStringBoundingBox(CiText(BTN_TH[i], BTN_EN[i])).width) + ScaleGUITrad(20));
	const int footer_h = fh + ScaleGUITrad(6);

	/* The smallest size. */
	l.needed_w = 2 * pad + std::max({tabs_w, pad + face_col + pad + mid_min_w + pad + net_w + pad, 8 * sw_min_w + 7 * sw_gap + 2 * pad, 4 * btn_min_w + 3 * btn_gap, ScaleGUITrad(640)});
	l.needed_h = pad + tab_h + gap + profile_h + gap + row2_min + gap + col_h + gap + btn_h + ScaleGUITrad(6) + footer_h + pad;

	/* The rest of the height (when the window is bigger) goes to the row of the chart. */
	const int extra = std::max(0, static_cast<int>(r.bottom - r.top + 1) - l.needed_h);
	const int row2_h = row2_min + extra;
	const int mid = left + (right - left) * 46 / 100;
	l.ops = {left, y, mid - gap / 2, y + row2_h};
	l.chart = {mid + gap / 2, y, right, y + row2_h};
	y += row2_h + gap;
	l.colours = {left, y, right, y + col_h};
	y += col_h + gap;

	const int bw = (right - left - 3 * btn_gap) / 4;
	for (int i = 0; i < 4; i++) {
		const int bx = left + i * (bw + btn_gap);
		l.btns[i] = {bx, y, bx + bw, y + btn_h};
	}
	y += btn_h + ScaleGUITrad(6);
	l.footer = {left, y, right, y + footer_h};

	/* Inside the card of the history. */
	const Rect &p = l.profile;
	const int fx = p.left + pad + (face_col - face_size) / 2;
	const int fy = p.top + l.hdr + pad;
	l.face = {fx, fy, fx + face_size, fy + face_size};
	l.face_btn = {p.left + pad, p.bottom - pad - fb_h, p.left + pad + face_col, p.bottom - pad};
	l.net_box = {p.right - pad - net_w, p.top + l.hdr + pad, p.right - pad, p.bottom - pad};
	l.mid_left = p.left + pad + face_col + pad;
	l.mid_right = l.net_box.left - pad;
	l.hq_btn = {l.mid_left, p.bottom - pad - hq_h, l.mid_right, p.bottom - pad};

	/* Inside the card of the colours: two rows of eight. */
	const Rect &c = l.colours;
	const int sw_w = (c.right - c.left - 2 * pad - 7 * sw_gap) / 8;
	const int sw_top = c.top + l.hdr + ScaleGUITrad(8) + row + ScaleGUITrad(8);
	for (int i = 0; i < 16; i++) {
		const int sx = c.left + pad + (i % 8) * (sw_w + sw_gap);
		const int sy = sw_top + (i / 8) * (sw_h + sw_gap);
		l.swatch[i] = {sx, sy, sx + sw_w, sy + sw_h};
	}
	return l;
}

/** The net worth of a company as the window shows it: company value + trading wallets - debt at the central bank. */
int64_t CompanyNetWorth(const Company *c, CompanyID id)
{
	return static_cast<int64_t>(CalculateCompanyValue(c)) + GetWalletsTotal(id) - GetCentralBankDebt(id);
}

bool InRect(const Rect &a, Point pt)
{
	return pt.x >= a.left && pt.x <= a.right && pt.y >= a.top && pt.y <= a.bottom;
}

/** The vehicles of one company: how many, and the profit of this year. */
struct VehicleStats {
	int64_t count[4] = {};  ///< Train, road vehicle, ship, aircraft.
	int64_t profit[4] = {};
};

VehicleStats CollectVehicles(CompanyID company)
{
	VehicleStats s;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner != company || !v->IsPrimaryVehicle()) continue;
		int slot;
		switch (v->type) {
			case VEH_TRAIN:    slot = 0; break;
			case VEH_ROAD:     slot = 1; break;
			case VEH_SHIP:     slot = 2; break;
			case VEH_AIRCRAFT: slot = 3; break;
			default: continue;
		}
		s.count[slot]++;
		s.profit[slot] += static_cast<int64_t>(v->profit_this_year) >> 8; // the game keeps the profit shifted by 8 bits
	}
	return s;
}

/** The company window. */
struct CompanyInfoWindow : Window {
	CompanyID company;                   ///< The company of this window.
	enum class QueryKind : uint8_t { None, CompanyName, President };
	QueryKind query_kind = QueryKind::None;
	bool placing_hq = false;             ///< True while the cursor places the headquarters (own tool, no original window).
	uint64_t ipo_flash_until = 0;        ///< Game minute until which the footer says that the IPO does not exist yet.

	/** The numbers change all the time: draw again 4 times a second so the cash is the same as in the status bar. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(250), [this](auto) {
		if (!Company::IsValidID(this->company)) {
			this->Close();
			return;
		}
		this->SetDirty();
	}};

	CompanyInfoWindow(WindowDesc &desc, CompanyID c) : Window(desc), company(c)
	{
		this->InitNested(CI_WINDOW_BASE + static_cast<int>(c.base()));
	}

	bool IsMine() const
	{
		return Company::IsValidID(this->company) && this->company == _local_company;
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_CIW_CAPTION) {
			if (!Company::IsValidID(this->company)) return std::string();
			return fmt::format("{} ({} {}) - {}", GetString(STR_STOCK_SYMBOL, this->company), CiText("บริษัท", "Company"), this->company.base() + 1, CiText("ข้อมูลบริษัท", "Company information"));
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	/** The panel can not be smaller than what every text needs (the font decides), and can be made bigger with the resize box. */
	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_CIW_PANEL) return;
		int net_w = 0;
		if (const Company *c = Company::GetIfValid(this->company); c != nullptr) {
			net_w = GetStringBoundingBox(CiMoney(CompanyNetWorth(c, this->company)), FS_LARGE).width;
		}
		const InfoLayout l = MakeInfoLayout({0, 0, ScaleGUITrad(4000), ScaleGUITrad(4000)}, net_w);
		size.width = std::max<uint>(size.width, l.needed_w);
		size.height = std::max<uint>(size.height, l.needed_h);
	}

	/** A button: flat box, outline, the text in the middle. */
	static void DrawButton(const Rect &b, const std::string &text, PixelColour fill, TextColour colour, bool enabled = true)
	{
		GfxFillRect(b.left, b.top, b.right, b.bottom, fill);
		DrawRectOutline(b, GetColourGradient(COLOUR_GREY, SHADE_LIGHT));
		const int ty = b.top + (b.bottom - b.top + 1 - GetCharacterHeight(FS_NORMAL)) / 2;
		DrawString(b.left + 2, b.right - 2, ty, text, enabled ? colour : TC_GREY, SA_HOR_CENTER);
	}

	/** A card: filled box with a strip and a title. */
	static void DrawCard(const Rect &r, int hdr, const std::string &title, const std::string &title_right)
	{
		GfxFillRect(r.left, r.top, r.right, r.bottom, GetColourGradient(COLOUR_GREY, SHADE_DARKER));
		GfxFillRect(r.left, r.top, r.right, r.top + hdr, GetColourGradient(COLOUR_GREY, SHADE_DARK));
		DrawRectOutline(r, GetColourGradient(COLOUR_GREY, SHADE_LIGHT));
		const int ty = r.top + (hdr + 1 - GetCharacterHeight(FS_NORMAL)) / 2;
		DrawString(r.left + ScaleGUITrad(8), r.right - ScaleGUITrad(8), ty, title, TC_LIGHT_BLUE, SA_LEFT);
		if (!title_right.empty()) DrawString(r.left + ScaleGUITrad(8), r.right - ScaleGUITrad(8), ty, title_right, TC_GREY, SA_RIGHT);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_CIW_PANEL) return;
		const Company *c = Company::GetIfValid(this->company);
		if (c == nullptr) return;

		const int64_t net = CompanyNetWorth(c, this->company);
		const InfoLayout l = MakeInfoLayout(r, GetStringBoundingBox(CiMoney(net), FS_LARGE).width);
		const int pad = l.pad;
		const int row = InfoRow();
		const bool mine = this->IsMine();
		const PixelColour line = GetColourGradient(COLOUR_GREY, SHADE_LIGHT);
		const PixelColour box_fill = GetColourGradient(COLOUR_GREY, SHADE_NORMAL);

		/* Tabs and date. */
		for (int i = 0; i < 4; i++) {
			DrawButton(l.tabs[i], CiText(TAB_TH[i], TAB_EN[i]), i == 0 ? GetColourGradient(COLOUR_YELLOW, SHADE_DARK) : box_fill, i == 0 ? TC_BLACK : TC_WHITE);
		}
		DrawString(l.date.left, l.date.right, l.date.top + (l.date.bottom - l.date.top + 1 - GetCharacterHeight(FS_NORMAL)) / 2, GetString(STR_JUST_DATE_LONG, TimerGameCalendar::date), TC_WHITE, SA_RIGHT);

		/* ---- Card 1: the history of the company ---- */
		const VehicleStats vs = CollectVehicles(this->company);
		int64_t total_vehicles = 0, total_profit = 0;
		for (int i = 0; i < 4; i++) {
			total_vehicles += vs.count[i];
			total_profit += vs.profit[i];
		}
		DrawCard(l.profile, l.hdr, CiText("ประวัติบริษัท", "History"), fmt::format("{} · {} {}", CiText("บริษัทขนส่ง", "Transport company"), CiText("ผู้เล่น", "Player"), this->company.base() + 1));

		/* The face and the president. */
		DrawCompanyManagerFace(c->face, c->colour, l.face);
		const int name_y = l.face.bottom + ScaleGUITrad(4);
		DrawString(l.face_btn.left, l.face_btn.right, name_y, GetString(STR_COMPANY_INFO_PRESIDENT, this->company), TC_WHITE, SA_HOR_CENTER);
		DrawString(l.face_btn.left, l.face_btn.right, name_y + row - ScaleGUITrad(2), CiText("ประธานบริษัท", "President"), TC_GREY, SA_HOR_CENTER);
		DrawButton(l.face_btn, CiText("เลือกใบหน้าใหม่", "New face"), box_fill, TC_WHITE, mine);

		/* The middle: the name, the line under it, three numbers and the headquarters button. */
		int y = l.profile.top + l.hdr + pad;
		DrawString(l.mid_left, l.mid_right, y, GetString(STR_STOCK_SYMBOL, this->company), TC_YELLOW, SA_LEFT, false, FS_LARGE);
		y += GetCharacterHeight(FS_LARGE) + ScaleGUITrad(2);
		DrawString(l.mid_left, l.mid_right, y, fmt::format("{} · {} {}", CiText("บริษัทขนส่ง", "Transport company"), CiText("ก่อตั้งปี", "founded"), static_cast<int>(c->inaugurated_year.base())), TC_GREY, SA_LEFT);
		y += row + ScaleGUITrad(4);
		uint32_t road_pieces = 0;
		for (uint32_t count : c->infrastructure.road) road_pieces += count;
		auto stat = [&](const std::string &label, const std::string &value) {
			DrawString(l.mid_left, l.mid_right, y, label, TC_WHITE, SA_LEFT);
			DrawString(l.mid_left, l.mid_right, y, value, TC_WHITE, SA_RIGHT);
			y += row;
		};
		stat(CiText("ยานพาหนะทั้งหมด", "Vehicles"), fmt::format("{} {}", total_vehicles, CiText("คัน", "")));
		stat(CiText("ถนนที่ดูแล", "Road pieces"), fmt::format("{} {}", road_pieces, CiText("ช่วง", "")));
		stat(CiText("สถานีในเครือข่าย", "Stations"), fmt::format("{} {}", static_cast<uint32_t>(c->infrastructure.station), CiText("สถานี", "")));
		const bool has_hq = c->location_of_HQ != INVALID_TILE;
		DrawButton(l.hq_btn, has_hq ? CiText("ดูที่ทำการบริษัท", "View headquarters") : CiText("สร้างที่ทำการบริษัท", "Build headquarters"), box_fill, TC_WHITE, has_hq || mine);

		/* The right box: net worth, cash, loan, status. */
		{
			const int64_t cash = static_cast<int64_t>(c->money);
			const int64_t debt = GetCentralBankDebt(this->company);
			GfxFillRect(l.net_box.left, l.net_box.top, l.net_box.right, l.net_box.bottom, GetColourGradient(COLOUR_GREY, SHADE_DARKEST));
			DrawRectOutline(l.net_box, line);
			const int nl = l.net_box.left + pad;
			const int nr = l.net_box.right - pad;
			int ny = l.net_box.top + ScaleGUITrad(6);
			DrawString(nl, nr, ny, CiText("ทรัพย์สินสุทธิ", "Net worth"), TC_GREY, SA_LEFT);
			ny += row;
			DrawString(nl, nr, ny, CiMoney(net), net < 0 ? TC_RED : TC_YELLOW, SA_LEFT, false, FS_LARGE);
			ny += GetCharacterHeight(FS_LARGE) + ScaleGUITrad(6);
			auto nrow = [&](const std::string &label, const std::string &value, TextColour colour) {
				DrawString(nl, nr, ny, label, TC_WHITE, SA_LEFT);
				DrawString(nl, nr, ny, value, colour, SA_RIGHT);
				ny += row;
			};
			nrow(CiText("เงินสด", "Cash"), CiMoney(cash), cash < 0 ? TC_RED : TC_WHITE);
			nrow(CiText("เงินกู้คงค้าง", "Outstanding loan"), CiMoney(debt), debt > 0 ? TC_ORANGE : TC_WHITE);
			GfxDrawLine(nl, ny + 1, nr, ny + 1, GetColourGradient(COLOUR_GREY, SHADE_NORMAL));
			ny += ScaleGUITrad(5);
			const bool broke = c->months_of_bankruptcy > 0;
			nrow(CiText("สถานะบริษัท", "Status"), broke ? CiText("ล้มละลาย", "Bankrupt") : CiText("ดำเนินกิจการ", "Active"), broke ? TC_RED : TC_GREEN);
		}

		/* ---- Card 2: the vehicles ---- */
		DrawCard(l.ops, l.hdr, CiText("การดำเนินงาน", "Operations"), fmt::format("{} {}", CiText("ข้อมูลปี", "Year"), GetLedgerCurrentYear()));
		{
			const int ol = l.ops.left + pad;
			const int orr = l.ops.right - pad;
			const int col_count = ol + (orr - ol) * 62 / 100;
			int oy = l.ops.top + l.hdr + ScaleGUITrad(6);
			DrawString(ol, col_count, oy, CiText("ประเภทการขนส่ง", "Type"), TC_GREY, SA_LEFT);
			DrawString(ol, col_count, oy, CiText("จำนวน", "Count"), TC_GREY, SA_RIGHT);
			DrawString(col_count, orr, oy, CiText("กำไรปีนี้", "Profit"), TC_GREY, SA_RIGHT);
			oy += row;
			GfxDrawLine(ol, oy - 1, orr, oy - 1, GetColourGradient(COLOUR_GREY, SHADE_NORMAL));
			const char *const type_th[4] = {"รถไฟ", "ยานพาหนะทางถนน", "เรือ", "เครื่องบิน"};
			const char *const type_en[4] = {"Trains", "Road vehicles", "Ships", "Aircraft"};
			for (int i = 0; i < 4; i++) {
				DrawString(ol, col_count, oy, CiText(type_th[i], type_en[i]), TC_WHITE, SA_LEFT);
				DrawString(ol, col_count, oy, fmt::format("{}", vs.count[i]), TC_WHITE, SA_RIGHT);
				if (vs.count[i] == 0) {
					DrawString(col_count, orr, oy, "—", TC_GREY, SA_RIGHT);
				} else {
					DrawString(col_count, orr, oy, CiMoney(vs.profit[i]), vs.profit[i] < 0 ? TC_RED : TC_YELLOW, SA_RIGHT);
				}
				oy += row;
			}
			GfxDrawLine(ol, oy, orr, oy, GetColourGradient(COLOUR_GREY, SHADE_NORMAL));
			oy += ScaleGUITrad(3);
			DrawString(ol, col_count, oy, CiText("รวมทั้งบริษัท", "Company total"), TC_WHITE, SA_LEFT);
			DrawString(ol, col_count, oy, fmt::format("{}", total_vehicles), TC_WHITE, SA_RIGHT);
			DrawString(col_count, orr, oy, CiMoney(total_profit), total_profit < 0 ? TC_RED : TC_YELLOW, SA_RIGHT);
		}

		/* ---- Card 3: the chart of income and costs ---- */
		DrawCard(l.chart, l.hdr, CiText("รายรับ / ค่าใช้จ่าย", "Income / costs"), CiText("รายเดือน (เกม)", "per game month"));
		this->DrawChart(l);

		/* ---- Card 4: the company colour ---- */
		DrawCard(l.colours, l.hdr, CiText("เลือกสีประจำบริษัท", "Company colour"), CiText("Company Colors · 16 สีมาตรฐาน", "Company Colors · 16 standard"));
		{
			const int cur = std::clamp<int>(static_cast<int>(c->colour), 0, 15);
			const int cl = l.colours.left + pad;
			const int cr = l.colours.right - pad;
			const int cy = l.colours.top + l.hdr + ScaleGUITrad(8);
			DrawString(cl, cr, cy, CiText("สีปัจจุบัน", "Current colour"), TC_GREY, SA_LEFT);
			const int sx = cl + ScaleGUITrad(86);
			const int sq = GetCharacterHeight(FS_NORMAL) - ScaleGUITrad(2);
			GfxFillRect(sx, cy, sx + sq, cy + sq, GetColourGradient(static_cast<Colours>(cur), SHADE_NORMAL));
			DrawRectOutline({sx, cy, sx + sq, cy + sq}, line);
			DrawString(sx + sq + ScaleGUITrad(6), cr, cy, COLOUR_NAMES[cur], TC_ORANGE, SA_LEFT);
			DrawString(cl, cr, cy, CiText("ใช้กับยานพาหนะและสีของบริษัท", "Used for the vehicles and the company"), TC_GREY, SA_RIGHT);
			for (int i = 0; i < 16; i++) {
				const Rect &s = l.swatch[i];
				GfxFillRect(s.left, s.top, s.right, s.bottom, GetColourGradient(static_cast<Colours>(i), SHADE_NORMAL));
				DrawRectOutline(s, line);
				if (i == cur) {
					DrawRectOutline({s.left - 1, s.top - 1, s.right + 1, s.bottom + 1}, GetColourGradient(COLOUR_WHITE, SHADE_NORMAL));
					DrawRectOutline({s.left + 1, s.top + 1, s.right - 1, s.bottom - 1}, GetColourGradient(COLOUR_WHITE, SHADE_NORMAL));
				}
				const int ty = s.top + (s.bottom - s.top + 1 - GetCharacterHeight(FS_SMALL)) / 2;
				DrawString(s.left + 1, s.right - 1, ty, COLOUR_NAMES[i], COLOUR_IS_LIGHT[i] ? TC_BLACK : TC_WHITE, SA_HOR_CENTER, false, FS_SMALL);
			}
		}

		/* ---- The buttons ---- */
		for (int i = 0; i < 4; i++) {
			const bool ipo = i == BTN_IPO;
			const bool enabled = i == 2 || mine;
			DrawButton(l.btns[i], CiText(BTN_TH[i], BTN_EN[i]), ipo ? GetColourGradient(COLOUR_ORANGE, SHADE_NORMAL) : box_fill, ipo ? TC_BLACK : TC_WHITE, enabled);
		}

		/* ---- The line at the bottom ---- */
		const bool flash = GameTimeManager::Get().GetTotalGameMinutes() < this->ipo_flash_until;
		if (flash) {
			DrawString(l.footer.left, l.footer.right, l.footer.top, CiText("ระบบ IPO ยังไม่เปิดใช้งาน (ปุ่มนี้เป็นเพียงตัวอย่างหน้าจอ)", "The IPO system is not available yet (this button is only a placeholder)"), TC_ORANGE, SA_LEFT);
		} else {
			DrawString(l.footer.left, l.footer.right, l.footer.top, CiText("ปุ่ม IPO เป็นตัวอย่างหน้าจอ ยังไม่มีระบบเสนอขายหุ้นจริง", "The IPO button is only a placeholder: there is no share offering yet"), TC_GREY, SA_LEFT);
		}
		DrawString(l.footer.left, l.footer.right, l.footer.top, fmt::format("COMPANY {:02}", this->company.base() + 1), TC_GREY, SA_RIGHT);
	}

	/** Bars of income and costs of the last #CHART_MONTHS game months. */
	void DrawChart(const InfoLayout &l) const
	{
		const Rect &r = l.chart;
		const int pad = l.pad;
		const int fh = GetCharacterHeight(FS_NORMAL);
		const int fs = GetCharacterHeight(FS_SMALL);

		/* The legend. */
		const int ly = r.top + l.hdr + ScaleGUITrad(6);
		const int sq = fh - ScaleGUITrad(4);
		const PixelColour col_income = GetColourGradient(COLOUR_YELLOW, SHADE_NORMAL);
		const PixelColour col_cost = GetColourGradient(COLOUR_CREAM, SHADE_NORMAL);
		int lx = r.left + pad;
		GfxFillRect(lx, ly + 2, lx + sq, ly + 2 + sq, col_income);
		lx += sq + ScaleGUITrad(4);
		const std::string t_income = CiText("รายรับ", "Income");
		DrawString(lx, r.right - pad, ly, t_income, TC_WHITE, SA_LEFT);
		lx += GetStringBoundingBox(t_income).width + ScaleGUITrad(12);
		GfxFillRect(lx, ly + 2, lx + sq, ly + 2 + sq, col_cost);
		lx += sq + ScaleGUITrad(4);
		DrawString(lx, r.right - pad, ly, CiText("ค่าใช้จ่าย", "Costs"), TC_WHITE, SA_LEFT);
		DrawString(r.left + pad, r.right - pad, ly, CiText("หน่วย: พัน", "Unit: thousand"), TC_GREY, SA_RIGHT);

		/* The last months: the current one and the four before it. */
		const GameTimeSnapshot snap = GameTimeManager::Get().GetSnapshot();
		const int32_t now_key = companyinfo::MonthKey(snap.year, snap.day);
		const auto &buckets = _month_buckets[this->company.base()];
		MonthBucket shown[CHART_MONTHS];
		int64_t top_value = 0;
		for (int i = 0; i < CHART_MONTHS; i++) {
			const auto it = buckets.find(now_key - (CHART_MONTHS - 1 - i));
			if (it != buckets.end()) shown[i] = it->second;
			top_value = std::max({top_value, shown[i].income, shown[i].expense});
		}
		const int64_t top = companyinfo::NiceCeil(top_value);

		/* The plot area. */
		const int axis_w = ScaleGUITrad(36);
		const int plot_l = r.left + pad + axis_w;
		const int plot_r = r.right - pad;
		const int plot_t = ly + fh + ScaleGUITrad(8);
		const int plot_b = r.bottom - pad - fs - ScaleGUITrad(4);
		if (plot_r <= plot_l || plot_b <= plot_t) return;
		const int plot_h = plot_b - plot_t;

		auto thousands = [](int64_t v) { return fmt::format("{}", v / 1000); };
		for (int g = 0; g <= 2; g++) {
			const int gy = plot_b - plot_h * g / 2;
			GfxDrawLine(plot_l, gy, plot_r, gy, GetColourGradient(COLOUR_GREY, g == 0 ? SHADE_LIGHT : SHADE_NORMAL));
			DrawString(r.left + pad, plot_l - ScaleGUITrad(4), gy - fs / 2, thousands(top * g / 2), TC_GREY, SA_RIGHT, false, FS_SMALL);
		}

		const int group_w = (plot_r - plot_l) / CHART_MONTHS;
		const int bar_w = std::max(2, group_w * 28 / 100);
		for (int i = 0; i < CHART_MONTHS; i++) {
			const int cx = plot_l + group_w * i + group_w / 2;
			const int h_income = static_cast<int>(shown[i].income * plot_h / top);
			const int h_cost = static_cast<int>(shown[i].expense * plot_h / top);
			if (h_income > 0) GfxFillRect(cx - bar_w - 1, plot_b - h_income, cx - 1, plot_b - 1, col_income);
			if (h_cost > 0) GfxFillRect(cx + 1, plot_b - h_cost, cx + bar_w + 1, plot_b - 1, col_cost);
			const int month = companyinfo::MonthOfKey(now_key - (CHART_MONTHS - 1 - i));
			DrawString(cx - group_w / 2, cx + group_w / 2, plot_b + ScaleGUITrad(3), CiText(MONTH_TH[month], MONTH_EN[month]), TC_GREY, SA_HOR_CENTER, false, FS_SMALL);
		}
	}

	/** Open the vehicle list of the first kind of vehicle that the company has (road vehicles when it has none). */
	void OpenVehicleList() const
	{
		const VehicleStats vs = CollectVehicles(this->company);
		VehicleType type = VEH_ROAD;
		if (vs.count[1] == 0) {
			if (vs.count[0] > 0) type = VEH_TRAIN;
			else if (vs.count[2] > 0) type = VEH_SHIP;
			else if (vs.count[3] > 0) type = VEH_AIRCRAFT;
		}
		ShowVehicleListWindow(this->company, type);
	}

	void SetColour(int index) const
	{
		const Colours colour = static_cast<Colours>(index);
		Command<CMD_SET_COMPANY_COLOUR>::Post(STR_COMPANY_INFO_ERROR_COLOUR, LS_DEFAULT, true, colour);
		Command<CMD_SET_COMPANY_COLOUR>::Post(STR_COMPANY_INFO_ERROR_COLOUR, LS_DEFAULT, false, colour);
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_CIW_PANEL) return;
		const Company *c = Company::GetIfValid(this->company);
		if (c == nullptr) return;
		const InfoLayout l = MakeInfoLayout(this->GetWidget<NWidgetBase>(WID_CIW_PANEL)->GetCurrentRect(), GetStringBoundingBox(CiMoney(CompanyNetWorth(c, this->company)), FS_LARGE).width);
		const bool mine = this->IsMine();

		/* Tabs: the first one is this window. */
		if (InRect(l.tabs[1], pt)) { ShowCompanyFinances(this->company); return; }
		if (InRect(l.tabs[2], pt)) { this->OpenVehicleList(); return; }
		if (InRect(l.tabs[3], pt)) { ShowCompanyInfrastructureOfMod(this->company); return; }

		/* Face and headquarters: only the original window can do these. */
		if (InRect(l.face_btn, pt)) {
			if (mine) ShowCompanyFaceOfMod(this->company); // The face window of OpenTTD itself (wrapper in company_gui.cpp), not the original company window.
			return;
		}
		if (InRect(l.hq_btn, pt)) {
			const bool has_hq = c->location_of_HQ != INVALID_TILE;
			if (has_hq && !_ctrl_pressed) {
				ScrollMainWindowToTile(c->location_of_HQ); // Click: look at the headquarters. Ctrl+click: move it.
			} else if (mine) {
				this->StartPlacingHQ();
			}
			return;
		}

		/* The colours. */
		if (mine) {
			for (int i = 0; i < 16; i++) {
				if (InRect(l.swatch[i], pt)) {
					this->SetColour(i);
					this->SetDirty();
					return;
				}
			}
		}

		/* The buttons at the bottom. */
		if (InRect(l.btns[0], pt) && mine) {
			this->query_kind = QueryKind::CompanyName;
			ShowQueryString(GetString(STR_STOCK_SYMBOL, this->company), STR_COMPANY_INFO_QUERY_NAME, 31, this, CS_ALPHANUMERAL, {});
		} else if (InRect(l.btns[1], pt) && mine) {
			this->query_kind = QueryKind::President;
			ShowQueryString(GetString(STR_COMPANY_INFO_PRESIDENT, this->company), STR_COMPANY_INFO_QUERY_PRESIDENT, 31, this, CS_ALPHANUMERAL, {});
		} else if (InRect(l.btns[2], pt)) {
			ShowStockWindow();
		} else if (InRect(l.btns[BTN_IPO], pt)) {
			/* The IPO system does not exist yet: only say so in the line at the bottom. */
			this->ipo_flash_until = GameTimeManager::Get().GetTotalGameMinutes() + 6;
			this->SetDirty();
		}
	}

	/** Start (or stop, when it is already on) the tool that places the headquarters: a 2x2 cursor, like the original window. */
	void StartPlacingHQ()
	{
		if (this->placing_hq) {
			ResetObjectToPlace();
			this->placing_hq = false;
			this->SetDirty();
			return;
		}
		SetObjectToPlaceWnd(SPR_CURSOR_HQ, PAL_NONE, HT_RECT, this);
		SetTileSelectSize(2, 2);
		this->placing_hq = true;
		this->SetDirty();
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		if (!this->IsMine()) return;
		if (Command<CMD_BUILD_OBJECT>::Post(STR_ERROR_CAN_T_BUILD_COMPANY_HEADQUARTERS, tile, OBJECT_HQ, 0) && !_shift_pressed) {
			ResetObjectToPlace();
			this->placing_hq = false;
		}
	}

	void OnPlaceObjectAbort() override
	{
		this->placing_hq = false;
		this->SetDirty();
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		const QueryKind kind = this->query_kind;
		this->query_kind = QueryKind::None;
		if (!str.has_value() || kind == QueryKind::None || !this->IsMine()) return;
		if (kind == QueryKind::CompanyName) {
			Command<CMD_RENAME_COMPANY>::Post(STR_COMPANY_INFO_ERROR_NAME, *str);
		} else {
			Command<CMD_RENAME_PRESIDENT>::Post(STR_COMPANY_INFO_ERROR_PRESIDENT, *str);
		}
	}
};

constexpr std::initializer_list<NWidgetPart> _nested_company_info_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CIW_CAPTION), SetToolTip(STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CIW_PANEL), SetMinimalSize(640, 520), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_RESIZEBOX, COLOUR_GREY),
	EndContainer(),
};

WindowDesc _company_info_desc(
	WDP_AUTO, "company_info", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_company_info_widgets
);

} // namespace

void ShowCompanyInfoWindow(CompanyID company)
{
	if (!Company::IsValidID(company)) return;
	if (BringWindowToFrontById(WC_CENTRAL_BANK, CI_WINDOW_BASE + static_cast<int>(company.base())) != nullptr) return;
	new CompanyInfoWindow(_company_info_desc, company);
}

void ShowOriginalCompanyWindow(CompanyID company)
{
	_company_info_use_original = true;
	ShowCompany(company);
	_company_info_use_original = false;
}
