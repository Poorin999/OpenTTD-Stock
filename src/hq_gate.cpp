/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file hq_gate.cpp The headquarters is compulsory (strict). While the local company has no headquarters, a small window keeps the HQ tool (2x2 cursor) in the hand:
 * if the player picks any other building tool it is replaced again within a fraction of a second, so nothing else can be built (tracks, roads, stations, depots,
 * airports, landscaping ... all of them use the cursor tool; without a depot no vehicle can be bought). The zones window opens next to it, with the zone that
 * the HQ will be in. When the HQ stands, the window closes and the zones window says in which zone it is. Local only: it is a rule of the interface of the player. */

#include "stdafx.h"
#include "hq_gate.h"
#include "zone_market.h"
#include "zone_map_gui.h"
#include <chrono>
#include <string>
#include "openttd.h"
#include "network/network.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/format.hpp"
#include "gfx_func.h"
#include "map_func.h"
#include "object_type.h"
#include "object_cmd.h"
#include "palette_func.h"
#include "strings_func.h"
#include "tilehighlight_func.h"
#include "tilehighlight_type.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "viewport_func.h"
#include "widgets/central_bank_widget.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"

#include "table/sprites.h"
#include "table/strings.h"

#include "safeguards.h"

/** Window number inside the class WC_CENTRAL_BANK (9 zones map, 10 headquarters gate). */
static constexpr int HQ_GATE_WINDOW_NUMBER = 10;

bool CompanyNeedsHq(CompanyID company)
{
	return Company::IsValidID(company) && Company::Get(company)->location_of_HQ == INVALID_TILE;
}

/** The window of the compulsory headquarters. */
struct HqGateWindow : Window {
	bool placing = false; ///< True while the HQ cursor is in the hand.

	HqGateWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(HQ_GATE_WINDOW_NUMBER);
	}

	/** Put the HQ tool in the hand when it is not there (also after the player took another tool or pressed the right mouse button). */
	void EnsureTool()
	{
		if (this->placing) return;
		SetObjectToPlaceWnd(SPR_CURSOR_HQ, PAL_NONE, HT_RECT, this);
		SetTileSelectSize(2, 2);
		this->placing = true;
		this->SetDirty();
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		if (!CompanyNeedsHq(_local_company)) return;
		Command<CMD_BUILD_OBJECT>::Post(STR_ERROR_CAN_T_BUILD_COMPANY_HEADQUARTERS, tile, OBJECT_HQ, 0);
	}

	void OnPlaceObjectAbort() override
	{
		this->placing = false; // The timer puts the tool back.
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_HG_PANEL) return;
		const int pad = ScaleGUITrad(8);
		const int row_h = GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3);
		int y = r.top + pad;
		DrawString(r.left + pad, r.right - pad, y, "ต้องวางสำนักงานใหญ่ (HQ) ก่อนเริ่มเล่น", TC_LIGHT_BLUE, SA_LEFT);
		y += row_h + ScaleGUITrad(4);
		DrawString(r.left + pad, r.right - pad, y, "คลิกซ้ายบนแผนที่เกมเพื่อวาง HQ (พื้นที่ 2x2)", TC_WHITE, SA_LEFT);
		y += row_h;
		DrawString(r.left + pad, r.right - pad, y, "ก่อนวางเสร็จ สร้างอย่างอื่นไม่ได้ (ราง ถนน สถานี โรงเก็บ ฯลฯ)", TC_GREY, SA_LEFT);
		y += row_h;
		DrawString(r.left + pad, r.right - pad, y, "HQ อยู่โซนไหน ดูได้จากหน้าต่างแผนที่โซน (เส้นขาวคือเส้นแบ่งโซน)", TC_GREY, SA_LEFT);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_hq_gate_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_HG_CAPTION), SetStringTip(STR_HQ_GATE_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_HG_PANEL), SetMinimalSize(420, 90), SetFill(1, 1),
	EndContainer(),
};

static WindowDesc _hq_gate_desc(
	WDP_AUTO, "hq_gate", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_hq_gate_widgets
);

/** Real time, also while the game is paused: keep the gate in place until the HQ stands. */
static const IntervalTimer<TimerWindow> _hq_gate_timer = {std::chrono::milliseconds(100), [](auto)
{
	if (_network_dedicated || _game_mode != GM_NORMAL) return;
	Window *w = FindWindowById(WC_CENTRAL_BANK, HQ_GATE_WINDOW_NUMBER);
	static bool was_gated = false;
	if (!CompanyNeedsHq(_local_company)) {
		if (w != nullptr) {
			w->Close();
			if (was_gated) ShowZoneMapWindow(); // Now the zones window says in which zone the HQ is.
		}
		was_gated = false;
		return;
	}
	was_gated = true;
	if (_zone_market.count <= 0) return; // The zones are made within a few ticks of the start; the gate waits for them.
	if (w == nullptr) {
		w = new HqGateWindow(_hq_gate_desc);
		ShowZoneMapWindow();
	}
	static_cast<HqGateWindow *>(w)->EnsureTool();
}};
