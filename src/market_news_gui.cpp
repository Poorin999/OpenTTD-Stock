/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file market_news_gui.cpp The news panel of the gold window. See market_news_gui.h. */

#include "stdafx.h"
#include "market_news_gui.h"
#include "market_events.h"
#include "market_news_ai.h"
#include "stock_base.h"
#include <chrono>
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "widgets/central_bank_widget.h"
#include "core/format.hpp"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "window_gui.h"
#include "zoom_func.h"

#include "table/strings.h"

#include "safeguards.h"

/** Where the parts of the panel are. Worked out from the rectangle only, so Draw and Click always agree. */
struct NewsLayout {
	int left, right;     ///< Text area.
	int title_y;         ///< Row of the heading.
	Rect lead;           ///< The card of the open story.
	int list_top;        ///< Top of the first row of the list.
	int row_h;           ///< Height of a row of the list.
	int rows;            ///< Rows of the list that fit.
};

static NewsLayout MakeNewsLayout(const Rect &r)
{
	NewsLayout l;
	const int pad = ScaleGUITrad(8);
	l.row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
	l.left = r.left + pad;
	l.right = r.right - pad;
	l.title_y = r.top + pad;
	const int top = l.title_y + l.row_h + ScaleGUITrad(4);
	const int bottom = r.bottom - pad;
	const int lead_h = std::max<int>((bottom - top) * 55 / 100, l.row_h * 7);
	l.lead = {l.left, top, l.right, std::min(top + lead_h, bottom)};
	l.list_top = l.lead.bottom + ScaleGUITrad(6) + l.row_h; // one row for the small heading of the list
	l.rows = std::max(0, (bottom - l.list_top) / l.row_h);
	return l;
}

/** The time of an event: game day and clock. */
static std::string NewsTime(int64_t minute)
{
	return fmt::format("D{} {:02}:{:02}", minute / (24 * 60) + 1, (minute / 60) % 24, minute % 60);
}

static std::string NewsCompany(const MarketEvent &e)
{
	return MarketEventSubjectName(e);
}

/** The colour of a story: green when it is good news for the shares, red when bad, otherwise by the gold. */
static TextColour NewsColour(const MarketEvent &e)
{
	if (e.share_ppm > 0) return TC_GREEN;
	if (e.share_ppm < 0) return TC_RED;
	return TC_YELLOW; // Gold news.
}

static TextColour SeverityColour(int severity)
{
	return severity >= 3 ? TC_RED : (severity == 2 ? TC_ORANGE : TC_GREY);
}

/** The event that is open: the selected one, or the newest. nullptr when there is no event. */
static const MarketEvent *OpenEvent(uint32_t selected_id)
{
	if (_market_events.log.empty()) return nullptr;
	for (const MarketEvent &e : _market_events.log) {
		if (e.id == selected_id) return &e;
	}
	return &_market_events.log.front();
}

void MarketNewsPanel::Draw(const Rect &r) const
{
	const NewsLayout l = MakeNewsLayout(r);
	const int width = l.right - l.left;
	if (width <= 0) return;

	DrawString(l.left, l.right, l.title_y, "ข่าว (NEWS)", TC_LIGHT_BLUE, SA_LEFT);
	DrawString(l.left, l.right, l.title_y, fmt::format("ข่าวตลาด {} ข่าว (ข่าวบริษัทอิงตัวเลขจริง)", _market_events.log.size()), TC_GREY, SA_RIGHT);

	const MarketEvent *open = OpenEvent(this->selected_id);
	if (open == nullptr) {
		DrawStringMultiLine(l.left, l.right, l.title_y + l.row_h + ScaleGUITrad(4), r.bottom - ScaleGUITrad(8),
				"ยังไม่มีข่าว ข่าวโลก ทองคำ และภาคธุรกิจเกิดขึ้นแบบสุ่มทุกไม่กี่นาที ส่วนข่าวของแต่ละบริษัทจะออกก็ต่อเมื่อตัวเลขจริงในงบการเงินเป็นเช่นนั้น (รายได้ กำไร ขาดทุน หนี้ มูลค่าบริษัท)", TC_GREY, SA_TOP | SA_LEFT);
		return;
	}

	/* The lead story, as the news writer tells it. */
	const MarketStory story = ComposeMarketStory(*open, NewsCompany(*open));
	GfxFillRect(l.lead.left, l.lead.top, l.lead.right, l.lead.bottom, GetColourGradient(COLOUR_GREY, SHADE_DARKER));
	DrawRectOutline(l.lead, GetColourGradient(COLOUR_GREY, SHADE_LIGHT));
	const int in_left = l.lead.left + ScaleGUITrad(8);
	const int in_right = l.lead.right - ScaleGUITrad(8);
	int y = l.lead.top + ScaleGUITrad(6);
	const int in_bottom = l.lead.bottom - ScaleGUITrad(4);
	const int row_h = l.row_h;

	DrawString(in_left, in_right, y, fmt::format("{}   {}", NewsTime(open->minute), story.reporter), TC_GREY, SA_LEFT);
	DrawString(in_left, in_right, y, story.impact, SeverityColour(open->severity), SA_RIGHT);
	y += row_h;

	const int head_h = GetStringHeight(story.headline, in_right - in_left);
	DrawStringMultiLine(in_left, in_right, y, std::min(y + head_h, in_bottom), story.headline, NewsColour(*open), SA_TOP | SA_LEFT);
	y += head_h + ScaleGUITrad(4);

	const int analysis_h = GetStringHeight(story.analysis, in_right - in_left);
	const int body_bottom = in_bottom - analysis_h - ScaleGUITrad(4);
	if (body_bottom > y) DrawStringMultiLine(in_left, in_right, y, body_bottom, story.body, TC_WHITE, SA_TOP | SA_LEFT);
	if (in_bottom - analysis_h >= y) DrawStringMultiLine(in_left, in_right, in_bottom - analysis_h, in_bottom, story.analysis, TC_LIGHT_BLUE, SA_TOP | SA_LEFT);

	/* The list of the latest events. */
	DrawString(l.left, l.right, l.lead.bottom + ScaleGUITrad(4), "ข่าวล่าสุด (คลิกเพื่ออ่าน)", TC_LIGHT_BLUE, SA_LEFT);
	const int col_stars = l.left + width * 16 / 100;
	const int col_effect = l.left + width * 70 / 100;
	int row = 0;
	for (const MarketEvent &e : _market_events.log) {
		if (row >= l.rows) break;
		const int ry = l.list_top + row * row_h;
		if (e.id == open->id) GfxFillRect(l.left, ry - 1, l.right, ry + row_h - 2, GetColourGradient(COLOUR_BLUE, SHADE_DARKER));
		DrawString(l.left + 2, col_stars - 4, ry, NewsTime(e.minute), TC_GREY, SA_LEFT);
		DrawString(col_stars, col_stars + ScaleGUITrad(30), ry, std::string(static_cast<size_t>(e.severity), '*'), SeverityColour(e.severity), SA_LEFT);
		const MarketStory s = ComposeMarketStory(e, NewsCompany(e));
		DrawString(col_stars + ScaleGUITrad(34), col_effect - 6, ry, s.headline, NewsColour(e), SA_LEFT);
		std::string effect;
		if (e.gold_ppm != 0) effect += "ทอง " + MarketPpmText(e.gold_ppm);
		if (e.share_ppm != 0) effect += std::string(effect.empty() ? "" : " ") + "หุ้น " + MarketPpmText(e.share_ppm);
		DrawString(col_effect, l.right - 2, ry, effect, TC_GREY, SA_RIGHT);
		row++;
	}
}

bool MarketNewsPanel::Click(const Rect &r, Point pt)
{
	const NewsLayout l = MakeNewsLayout(r);
	if (pt.x < l.left || pt.x > l.right || pt.y < l.list_top) return false;
	const int row = (pt.y - l.list_top) / l.row_h;
	if (row < 0 || row >= l.rows || static_cast<size_t>(row) >= _market_events.log.size()) return false;
	const uint32_t id = _market_events.log[static_cast<size_t>(row)].id;
	if (id == this->selected_id) return false;
	this->selected_id = id;
	return true;
}

/** Window number of the market news window inside the class WC_CENTRAL_BANK (0 bank, 1 statement, 2 past statements, 3 buy gold, 4 policy, 5 payroll news, 6 market news). */
static constexpr int MN_WINDOW_NUMBER = 6;

/** The market news window: the news panel in a window of its own. The menu entry Graphs > Market News opens it (it replaces the income graph). */
struct MarketNewsWindow : Window {
	MarketNewsPanel news; ///< Which story is open.

	/** Redraw regularly: the news come by themselves. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(500), [this](auto) {
		this->SetDirty();
	}};

	MarketNewsWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(MN_WINDOW_NUMBER);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_MN_PANEL) this->news.Draw(r);
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_MN_PANEL) return;
		if (this->news.Click(this->GetWidget<NWidgetBase>(WID_MN_PANEL)->GetCurrentRect(), pt)) this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_market_news_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_MN_CAPTION), SetStringTip(STR_MARKET_NEWS_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_MN_PANEL), SetMinimalSize(576, 330), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
};

static WindowDesc _market_news_desc(
	WDP_AUTO, "market_news", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_market_news_widgets
);

void ShowMarketNewsWindow()
{
	if (BringWindowToFrontById(WC_CENTRAL_BANK, MN_WINDOW_NUMBER) != nullptr) return;
	new MarketNewsWindow(_market_news_desc);
}

bool IsMarketNewsWindowOpen()
{
	return FindWindowById(WC_CENTRAL_BANK, MN_WINDOW_NUMBER) != nullptr;
}
