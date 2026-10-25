/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file market_news_gui.cpp The news panel of the gold window. See market_news_gui.h. */

#include "stdafx.h"
#include "market_news_gui.h"
#include "cargo_market_gui.h"
#include "market_events.h"
#include "market_news_ai.h"
#include "stock_base.h"
#include <algorithm>
#include <chrono>
#include <vector>
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
	/* Town news and weather have no push on the shares: the colour follows the kind (good for the town green, bad red). */
	switch (e.kind) {
		case MarketEventKind::TownGrew:
		case MarketEventKind::TownMilestone:
		case MarketEventKind::WeatherDroughtEnd: return TC_GREEN;
		case MarketEventKind::TownShrank:
		case MarketEventKind::WeatherStorm:
		case MarketEventKind::WeatherMeteor:
		case MarketEventKind::WeatherDrought: return TC_RED;
		default: break;
	}
	if (e.share_ppm > 0) return TC_GREEN;
	if (e.share_ppm < 0) return TC_RED;
	return TC_YELLOW; // Gold news.
}

static TextColour SeverityColour(int severity)
{
	return severity >= 3 ? TC_RED : (severity == 2 ? TC_ORANGE : TC_GREY);
}

/** The three tabs of the news centre: the economy (world, gold, cargo, diamonds, companies), the weather, and the rest (towns). */
enum NewsTab : uint8_t { NEWS_TAB_ECONOMY, NEWS_TAB_WEATHER, NEWS_TAB_OTHER, NEWS_TAB_COUNT };

static int NewsTabOf(MarketEventKind kind)
{
	switch (kind) {
		case MarketEventKind::WeatherStorm:
		case MarketEventKind::WeatherMeteor:
		case MarketEventKind::WeatherDrought:
		case MarketEventKind::WeatherDroughtEnd: return NEWS_TAB_WEATHER;
		case MarketEventKind::TownBoom:
		case MarketEventKind::TownGrew:
		case MarketEventKind::TownShrank:
		case MarketEventKind::TownMilestone: return NEWS_TAB_OTHER;
		default: return NEWS_TAB_ECONOMY;
	}
}

static const char *NewsTabName(int tab)
{
	static const char *const names[NEWS_TAB_COUNT] = { "ข่าวเศรษฐกิจ", "ข่าวสภาพอากาศ", "อื่น ๆ" };
	return names[std::clamp(tab, 0, static_cast<int>(NEWS_TAB_COUNT) - 1)];
}

/** The events of one tab, newest first. */
static std::vector<const MarketEvent *> NewsOfTab(int tab)
{
	std::vector<const MarketEvent *> list;
	for (const MarketEvent &e : _market_events.log) {
		if (NewsTabOf(e.kind) == tab) list.push_back(&e);
	}
	return list;
}

/** How big a story is, for the bars of the chart: the real number when there is one, else the push on the shares, the gold or the cargo, else the severity. */
static int64_t NewsMagnitude(const MarketEvent &e)
{
	auto absolute = [](int64_t v) { return v < 0 ? -v : v; };
	int64_t m = absolute(e.fact_ppm);
	if (m == 0) m = std::max(absolute(e.share_ppm), absolute(e.gold_ppm));
	if (m == 0) m = absolute(MarketCargoEffectOf(e.kind, e.severity).push_ppm);
	if (m == 0) m = static_cast<int64_t>(e.severity) * 10'000;
	return m;
}

/** Where the parts of the panel are. Worked out from the rectangle only, so Draw and Click always agree. */
struct NewsLayout {
	int left, right;     ///< Text area.
	int title_y;         ///< Row of the heading.
	int row_h;           ///< Height of one line of text.
	int item_h;          ///< Height of one story in the list (two lines).
	int list_top;        ///< Top of the first story of the list.
	int rows;            ///< Stories of the list that fit.
	Rect tabs[NEWS_TAB_COUNT]; ///< The tab buttons.
	Rect list;           ///< The list of stories (left).
	Rect detail;         ///< The open story (right).
};

static NewsLayout MakeNewsLayout(const Rect &r)
{
	NewsLayout l;
	const int pad = ScaleGUITrad(8);
	const int gap = ScaleGUITrad(6);
	const int tab_gap = ScaleGUITrad(2);
	l.row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
	l.item_h = l.row_h * 2 + ScaleGUITrad(6);
	l.left = r.left + pad;
	l.right = r.right - pad;
	l.title_y = r.top + pad;
	const int bottom = r.bottom - pad;
	const int width = l.right - l.left;

	const int tab_top = l.title_y + l.row_h + ScaleGUITrad(4);
	const int tab_h = l.row_h + ScaleGUITrad(6);
	const int tab_w = std::max(1, (width - tab_gap * (NEWS_TAB_COUNT - 1)) / NEWS_TAB_COUNT);
	for (int i = 0; i < NEWS_TAB_COUNT; i++) {
		const int x = l.left + i * (tab_w + tab_gap);
		l.tabs[i] = {x, tab_top, x + tab_w, tab_top + tab_h};
	}

	const int body_top = tab_top + tab_h + gap;
	const int list_w = std::clamp(width * 38 / 100, ScaleGUITrad(150), std::max(ScaleGUITrad(150), width / 2));
	l.list = {l.left, body_top, l.left + list_w, bottom};
	l.detail = {l.list.right + gap, body_top, l.right, bottom};
	l.list_top = l.list.top + l.row_h + gap;
	l.rows = std::max(0, (l.list.bottom - l.list_top) / l.item_h);
	return l;
}

/** The story that is open in a tab: the selected one when it is in the tab, else the newest of the tab. nullptr when the tab is empty. */
static const MarketEvent *OpenEvent(const std::vector<const MarketEvent *> &list, uint32_t selected_id)
{
	for (const MarketEvent *e : list) {
		if (e->id == selected_id) return e;
	}
	return list.empty() ? nullptr : list.front();
}

void MarketNewsPanel::Draw(const Rect &r) const
{
	const NewsLayout l = MakeNewsLayout(r);
	const int width = l.right - l.left;
	if (width <= 0) return;
	const int pad = ScaleGUITrad(8);
	const int gap = ScaleGUITrad(6);
	const int tab = std::clamp<int>(this->tab, 0, NEWS_TAB_COUNT - 1);

	const PixelColour panel_fill = GetColourGradient(COLOUR_GREY, SHADE_DARKER);
	const PixelColour panel_line = GetColourGradient(COLOUR_GREY, SHADE_LIGHT);

	DrawString(l.left, l.right, l.title_y, "ศูนย์ข่าวการขนส่ง (TRANSPORT NEWS)", TC_LIGHT_BLUE, SA_LEFT);
	DrawString(l.left, l.right, l.title_y, fmt::format("{} ข่าว (ข่าวจริงจากเกม ยกเว้นข่าวสงครามกับสันติภาพ)", _market_events.log.size()), TC_GREY, SA_RIGHT);

	/* The tabs, with the number of stories of each. */
	size_t counts[NEWS_TAB_COUNT] = {};
	for (const MarketEvent &e : _market_events.log) counts[NewsTabOf(e.kind)]++;
	for (int i = 0; i < NEWS_TAB_COUNT; i++) {
		const Rect &t = l.tabs[i];
		GfxFillRect(t.left, t.top, t.right, t.bottom, i == tab ? GetColourGradient(COLOUR_BLUE, SHADE_NORMAL) : panel_fill);
		DrawRectOutline(t, panel_line);
		DrawString(t.left + 2, t.right - 2, t.top + ScaleGUITrad(3), fmt::format("{}  {:02}", NewsTabName(i), counts[i]), i == tab ? TC_WHITE : TC_GREY, SA_HOR_CENTER);
	}

	const std::vector<const MarketEvent *> list = NewsOfTab(tab);
	const MarketEvent *open = OpenEvent(list, this->selected_id);

	/* The list (left). */
	GfxFillRect(l.list.left, l.list.top, l.list.right, l.list.bottom, panel_fill);
	DrawRectOutline(l.list, panel_line);
	DrawString(l.list.left + pad, l.list.right - pad, l.list.top + ScaleGUITrad(3), fmt::format("ข่าวล่าสุด · {} รายการ", list.size()), TC_LIGHT_BLUE, SA_LEFT);
	for (int i = 0; i < l.rows && static_cast<size_t>(i) < list.size(); i++) {
		const MarketEvent &e = *list[static_cast<size_t>(i)];
		const int ry = l.list_top + i * l.item_h;
		if (open != nullptr && e.id == open->id) GfxFillRect(l.list.left + 1, ry, l.list.right - 1, ry + l.item_h - 2, GetColourGradient(COLOUR_BLUE, SHADE_DARKER));
		DrawString(l.list.left + pad, l.list.right - pad, ry, NewsTime(e.minute), TC_GREY, SA_LEFT);
		DrawString(l.list.left + pad, l.list.right - pad, ry, std::string(static_cast<size_t>(e.severity), '*'), SeverityColour(e.severity), SA_RIGHT);
		const MarketStory s = ComposeMarketStory(e, NewsCompany(e));
		DrawString(l.list.left + pad, l.list.right - pad, ry + l.row_h, s.headline, NewsColour(e), SA_LEFT);
		GfxFillRect(l.list.left + 1, ry + l.item_h - 1, l.list.right - 1, ry + l.item_h - 1, panel_line);
	}

	/* The open story (right). */
	GfxFillRect(l.detail.left, l.detail.top, l.detail.right, l.detail.bottom, panel_fill);
	DrawRectOutline(l.detail, panel_line);
	const int in_left = l.detail.left + pad;
	const int in_right = l.detail.right - pad;
	const int in_w = in_right - in_left;
	if (open == nullptr) {
		DrawStringMultiLine(in_left, in_right, l.detail.top + pad, l.detail.bottom - pad,
				"ยังไม่มีข่าวในหัวข้อนี้ ข่าวทุกข่าวมาจากเหตุการณ์จริงในเกม: งบการเงินของบริษัท ประชากรของเมือง สภาพอากาศ ราคาสินค้าที่ส่งจริง และข่าวสงครามกับสันติภาพ", TC_GREY, SA_TOP | SA_LEFT);
		return;
	}

	const MarketStory story = ComposeMarketStory(*open, NewsCompany(*open));
	int y = l.detail.top + pad;
	DrawString(in_left, in_right, y, fmt::format("{} · {}", NewsTabName(tab), story.reporter), TC_LIGHT_BLUE, SA_LEFT);
	DrawString(in_left, in_right, y, NewsTime(open->minute), TC_GREY, SA_RIGHT);
	y += l.row_h + ScaleGUITrad(4);

	const int head_h = GetStringHeight(story.headline, in_w);
	DrawStringMultiLine(in_left, in_right, y, std::min(y + head_h, l.detail.bottom - pad), story.headline, NewsColour(*open), SA_TOP | SA_LEFT);
	y += head_h + ScaleGUITrad(6);

	/* From the bottom up: the note about the effect, the chart, the three boxes. Whatever does not fit is left out; the text keeps the rest. */
	const int box_pad = ScaleGUITrad(4);
	const int analysis_h = GetStringHeight(story.analysis, in_w - 2 * box_pad) + 2 * box_pad;
	const int note_top = l.detail.bottom - pad - analysis_h;
	const int boxes_h = l.row_h * 2 + ScaleGUITrad(8);
	const int chart_h = l.row_h * 7;
	bool show_chart = true;
	bool show_boxes = true;
	auto layout_bottom = [&](int &chart_top, int &boxes_top) {
		chart_top = note_top - gap - (show_chart ? chart_h : -gap);
		boxes_top = chart_top - gap - (show_boxes ? boxes_h : -gap);
		return boxes_top - gap;
	};
	int chart_top = 0, boxes_top = 0;
	int body_bottom = layout_bottom(chart_top, boxes_top);
	if (body_bottom - y < l.row_h * 3) { show_chart = false; body_bottom = layout_bottom(chart_top, boxes_top); }
	if (body_bottom - y < l.row_h * 3) { show_boxes = false; body_bottom = layout_bottom(chart_top, boxes_top); }
	if (body_bottom > y) DrawStringMultiLine(in_left, in_right, y, body_bottom, story.body, TC_WHITE, SA_TOP | SA_LEFT);

	if (show_boxes) {
		const int bw = std::max(1, (in_w - 2 * gap) / 3);
		std::string fact = MarketFactText(open->kind, open->fact_ppm);
		if (fact.empty()) fact = "-";
		size_t same_kind = 0;
		for (const MarketEvent &e : _market_events.log) if (e.kind == open->kind) same_kind++;
		const std::string labels[3] = { "ตัวเลขจริง", "ความรุนแรง", "ข่าวชนิดนี้" };
		const std::string values[3] = { fact, story.impact, fmt::format("{} ครั้ง", same_kind) };
		const TextColour colours[3] = { NewsColour(*open), SeverityColour(open->severity), TC_WHITE };
		for (int i = 0; i < 3; i++) {
			const int bx = in_left + i * (bw + gap);
			GfxFillRect(bx, boxes_top, bx + bw, boxes_top + boxes_h, GetColourGradient(COLOUR_GREY, SHADE_NORMAL));
			DrawRectOutline({bx, boxes_top, bx + bw, boxes_top + boxes_h}, panel_line);
			DrawString(bx + box_pad, bx + bw - box_pad, boxes_top + ScaleGUITrad(3), labels[i], TC_GREY, SA_LEFT);
			DrawString(bx + box_pad, bx + bw - box_pad, boxes_top + ScaleGUITrad(3) + l.row_h, values[i], colours[i], SA_LEFT);
		}
	}

	if (show_chart) {
		/* The size of the last stories of the same kind, oldest at the left, the open one in orange. */
		static constexpr size_t BARS = 6;
		std::vector<const MarketEvent *> same;
		for (const MarketEvent &e : _market_events.log) {
			if (e.kind == open->kind) same.push_back(&e);
			if (same.size() >= BARS) break;
		}
		std::reverse(same.begin(), same.end());
		const Rect chart = {in_left, chart_top, in_right, chart_top + chart_h};
		GfxFillRect(chart.left, chart.top, chart.right, chart.bottom, GetColourGradient(COLOUR_GREY, SHADE_NORMAL));
		DrawRectOutline(chart, panel_line);
		DrawString(chart.left + box_pad, chart.right - box_pad, chart.top + ScaleGUITrad(3), "ขนาดของข่าวชนิดนี้ (ครั้งล่าสุด)", TC_WHITE, SA_LEFT);
		const int bars_top = chart.top + l.row_h + box_pad;
		const int bars_bottom = chart.bottom - l.row_h - ScaleGUITrad(2);
		const int slot_w = std::max(1, (chart.right - chart.left - 2 * box_pad) / static_cast<int>(BARS));
		int64_t vmax = 1;
		for (const MarketEvent *e : same) vmax = std::max(vmax, NewsMagnitude(*e));
		const int first_slot = static_cast<int>(BARS) - static_cast<int>(same.size()); // Fewer stories than bars: they sit at the right.
		for (size_t i = 0; i < same.size(); i++) {
			const int sx = chart.left + box_pad + (first_slot + static_cast<int>(i)) * slot_w;
			const int bar_w = std::max(1, slot_w * 6 / 10);
			const int bx = sx + (slot_w - bar_w) / 2;
			const int bar_h = std::max<int>(1, static_cast<int>(static_cast<int64_t>(bars_bottom - bars_top) * NewsMagnitude(*same[i]) / vmax));
			const bool is_open = same[i]->id == open->id;
			GfxFillRect(bx, bars_bottom - bar_h, bx + bar_w - 1, bars_bottom, is_open ? GetColourGradient(COLOUR_ORANGE, SHADE_NORMAL) : GetColourGradient(COLOUR_GREEN, SHADE_NORMAL));
			DrawString(sx, sx + slot_w - 1, bars_bottom + ScaleGUITrad(2), fmt::format("D{}", same[i]->minute / (24 * 60) + 1), TC_GREY, SA_HOR_CENTER);
		}
	}

	/* The note about the effect on the prices. */
	GfxFillRect(in_left, note_top, in_right, note_top + analysis_h, GetColourGradient(COLOUR_GREY, SHADE_NORMAL));
	DrawRectOutline({in_left, note_top, in_right, note_top + analysis_h}, GetColourGradient(COLOUR_ORANGE, SHADE_NORMAL));
	DrawStringMultiLine(in_left + box_pad, in_right - box_pad, note_top + box_pad, note_top + analysis_h - box_pad, story.analysis, TC_ORANGE, SA_TOP | SA_LEFT);
}

bool MarketNewsPanel::Click(const Rect &r, Point pt)
{
	const NewsLayout l = MakeNewsLayout(r);
	for (int i = 0; i < NEWS_TAB_COUNT; i++) {
		const Rect &t = l.tabs[i];
		if (pt.x < t.left || pt.x > t.right || pt.y < t.top || pt.y > t.bottom) continue;
		if (this->tab == i) return false;
		this->tab = static_cast<uint8_t>(i);
		this->selected_id = 0; // The newest story of the new tab.
		return true;
	}
	if (pt.x < l.list.left || pt.x > l.list.right || pt.y < l.list_top) return false;
	const int row = (pt.y - l.list_top) / l.item_h;
	const std::vector<const MarketEvent *> list = NewsOfTab(std::clamp<int>(this->tab, 0, NEWS_TAB_COUNT - 1));
	if (row < 0 || row >= l.rows || static_cast<size_t>(row) >= list.size()) return false;
	const uint32_t id = list[static_cast<size_t>(row)]->id;
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
		if (widget == WID_MN_CARGO) {
			ShowCargoPricesWindow();
			return;
		}
		if (widget != WID_MN_PANEL) return;
		if (this->news.Click(this->GetWidget<NWidgetBase>(WID_MN_PANEL)->GetCurrentRect(), pt)) this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_market_news_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_MN_CAPTION), SetStringTip(STR_MARKET_NEWS_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_MN_CARGO), SetStringTip(STR_MARKET_NEWS_CARGO_BUTTON, STR_MARKET_NEWS_CARGO_TIP),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_MN_PANEL), SetMinimalSize(760, 440), SetFill(1, 1), SetResize(1, 1),
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
