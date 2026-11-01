/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file radial_menu.cpp The radial menu of the design ("วงล้อ: คลิกขวาที่โรงงาน" and "วงล้อ: คลิกขวาที่พื้นว่าง").
 *
 * OPEN: a right click that does not move (so right-drag scrolling of the map still works) on an industry or on open land. No file of OpenTTD is patched: a timer watches the
 * right mouse button (press and release, how far the mouse and the map moved) and calls ShowRadialMenu().
 * THE RING: six slices of 60 degrees, the first on top, clockwise. INDUSTRY: Negotiate, Contracts, Warehouse (not built yet), Manage, Station, Info.
 * OPEN LAND: Station, Industry, Warehouse (not built yet), Road, Rail (not for an industry owner), Land info.
 * Click a slice (left button). Esc, a click outside or a second right click closes it. A grey slice does nothing; the box under the ring says why.
 * Drawing only: the window changes nothing in the game itself; every action opens a window or a tool that exists already. */

#include "stdafx.h"
#include "radial_menu.h"
#include "contract.h"
#include "contract_math.h"
#include "contract_screens.h"
#include "company_role.h"
#include "industry_owner.h"
#include "company_func.h"
#include "industry.h"
#include "industry_map.h"
#include "tile_map.h"
#include "town.h"
#include "cargotype.h"
#include "gui.h"
#include "road_gui.h"
#include "rail_gui.h"
#include "viewport_func.h"
#include "tilehighlight_func.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "core/format.hpp"
#include "core/math_func.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

extern RailType _last_built_railtype; // toolbar_gui.cpp
void ShowIndustryViewWindow(IndustryID industry); // industry_gui.cpp

static constexpr int RM_WINDOW_NUMBER = 15; ///< Window number inside the class WC_CENTRAL_BANK (0 to 14 are used by the other windows of the mod).
static constexpr WidgetID WID_RM_AREA = 0;

static constexpr int RM_OUTER = 118; ///< Outer radius of the ring (unscaled pixels).
static constexpr int RM_INNER = 48;  ///< Inner radius of the ring.
static constexpr int RM_HUB = 43;    ///< Radius of the centre circle.
static constexpr int RM_HINT_H = 38; ///< Height of the box under the ring.

enum class RmKind : uint8_t { Industry, Land };

struct RmSlice {
	const char *thai;
	const char *english;
};

static constexpr std::array<RmSlice, 6> RM_INDUSTRY = {{
	{"ต่อรองสัญญา", "Negotiate"}, {"สัญญาของฉัน", "Contracts"}, {"คลังสินค้า", "Warehouse"},
	{"จัดการโรงงาน", "Manage"}, {"สร้างสถานี", "Station"}, {"ข้อมูล", "Info"},
}};
static constexpr std::array<RmSlice, 6> RM_LAND = {{
	{"สร้างสถานี", "Station"}, {"สร้างโรงงาน", "Industry"}, {"สร้างคลัง", "Warehouse"},
	{"สร้างถนน", "Road"}, {"สร้างราง", "Rail"}, {"ข้อมูลพื้นที่", "Land info"},
}};

static std::string MoneyText(int64_t v)
{
	return GetString(STR_JUST_CURRENCY_LONG, v);
}

struct RadialWindow : Window {
	RmKind kind;
	TileIndex tile;
	IndustryID industry{};
	int hover = -1;                 ///< Slice under the mouse, -1 = none.
	int diameter = 0;               ///< Diameter of the ring in screen pixels.
	int hint_h = 0;
	std::vector<int8_t> map;        ///< For every pixel of the ring: slice 0..5, -1 outside, -2 centre, -3 hole, -4 line.
	Point at{};                     ///< Position of the mouse when the menu opened (the centre of the ring).

	RadialWindow(WindowDesc &desc, TileIndex t, Point cursor) : Window(desc), tile(t), at(cursor)
	{
		this->kind = IsTileType(t, MP_INDUSTRY) ? RmKind::Industry : RmKind::Land;
		if (this->kind == RmKind::Industry) this->industry = GetIndustryIndex(t);
		this->diameter = 2 * ScaleGUITrad(RM_OUTER) + 2;
		this->hint_h = ScaleGUITrad(RM_HINT_H);
		this->BuildMap();
		this->InitNested(RM_WINDOW_NUMBER);
	}

	Point OnInitialPosition(int16_t sm_width, int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		Point pt;
		pt.x = Clamp(this->at.x - this->diameter / 2, 0, std::max<int>(0, _screen.width - sm_width));
		pt.y = Clamp(this->at.y - this->diameter / 2, 0, std::max<int>(0, _screen.height - sm_height));
		return pt;
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_RM_AREA) return;
		size.width = this->diameter;
		size.height = this->diameter + ScaleGUITrad(6) + this->hint_h;
	}

	/** Which part of the ring a pixel belongs to (offsets from the centre, in pixels). */
	void BuildMap()
	{
		const int d = this->diameter;
		const double outer = ScaleGUITrad(RM_OUTER);
		const double inner = ScaleGUITrad(RM_INNER);
		const double hub = ScaleGUITrad(RM_HUB);
		this->map.assign(static_cast<size_t>(d) * d, -1);
		for (int y = 0; y < d; y++) {
			for (int x = 0; x < d; x++) {
				const double dx = x + 0.5 - d / 2.0;
				const double dy = y + 0.5 - d / 2.0;
				const double r = std::sqrt(dx * dx + dy * dy);
				int8_t code = -1;
				if (r <= outer) {
					if (r < hub - 1.0) {
						code = -2;
					} else if (r < hub) {
						code = -4;
					} else if (r < inner) {
						code = -3;
					} else if (r > outer - 1.0 || r < inner + 1.0) {
						code = -4;
					} else {
						double a = std::atan2(dx, -dy) * 180.0 / 3.14159265358979; // 0 = top, clockwise
						if (a < 0) a += 360.0;
						const double shifted = a + 30.0;
						const int slice = static_cast<int>(shifted / 60.0) % 6;
						const double off = std::fmod(shifted, 60.0);
						const double edge = std::min(off, 60.0 - off) * 3.14159265358979 / 180.0 * r; // distance to the line in pixels
						code = edge < 1.0 ? -4 : static_cast<int8_t>(slice);
					}
				}
				this->map[static_cast<size_t>(y) * d + x] = code;
			}
		}
	}

	int SliceAt(int x, int y) const
	{
		if (x < 0 || y < 0 || x >= this->diameter || y >= this->diameter) return -1;
		const int8_t c = this->map[static_cast<size_t>(y) * this->diameter + x];
		return c >= 0 ? c : -1;
	}

	/* ---------------- what each slice does ---------------- */

	bool IsOwnerRole() const
	{
		return Company::IsValidID(_local_company) && GetCompanyRole(_local_company) == crole::Role::IndustryOwner;
	}

	/** Why a slice is grey (empty = it works). */
	std::string Why(int s) const
	{
		if (!Company::IsValidID(_local_company)) return "ต้องมีบริษัทก่อน / You need a company first";
		if (this->kind == RmKind::Industry) {
			if (s == 2) return "คลังสินค้า: ยังไม่เปิดให้ใช้ / Warehouse: not built yet";
		} else {
			if (s == 2) return "สร้างคลัง: ยังไม่เปิดให้ใช้ / Warehouse: not built yet";
			if (s == 4 && this->IsOwnerRole()) return "สร้างราง: เจ้าของโรงงานสร้างรางไม่ได้ / Rail: an industry owner can not build rail";
		}
		return std::string();
	}

	std::string Hint(int s) const
	{
		if (s < 0) {
			return this->kind == RmKind::Industry ? "คลิกซ้ายเลือกคำสั่ง · Esc ปิด / Left click an action, Esc closes" : "คลิกซ้ายเลือกคำสั่ง · Esc ปิด / Left click an action, Esc closes";
		}
		const std::string why = this->Why(s);
		if (!why.empty()) return why;
		if (this->kind == RmKind::Industry) {
			const Industry *i = Industry::Get(this->industry);
			const std::string name = GetString(STR_INDUSTRY_NAME, this->industry);
			switch (s) {
				case 0: {
					std::string extra;
					for (const auto &p : i->produced) {
						if (!IsValidCargoType(p.cargo)) continue;
						int64_t ident = 0;
						int32_t built = 0;
						ContractIndustryIdent(i, ident, built);
						const int64_t output = ContractMonthlyOutput(i, p.cargo);
						const int64_t free_units = contract::FreeOf(output, _contracts.Reserved(i->index.base(), ident, built, p.cargo));
						const int64_t price = GetCargoUnitPriceCents(p.cargo);
						extra = fmt::format(" · {} {}/{} · {} {} {}", "ราคาตลาด", price / contract::PRICE_UNITS, "หน่วย", "ว่าง", free_units, "หน่วย");
						break;
					}
					const CompanyID owner = GetIndustryOwner(i->index);
					return fmt::format("ต่อรองสัญญากับ {}{} · {}", name, extra, owner == CompanyID::Invalid() ? "เจ้าของ: ระบบ (NPC)" : fmt::format("เจ้าของ: บริษัทที่ {}", owner.base() + 1));
				}
				case 1: return "เปิดหน้าต่างสัญญาของฉัน / Open My Contracts";
				case 3: {
					const CompanyID owner = GetIndustryOwner(i->index);
					const int64_t price = GetIndustryOwnershipPrice(i);
					if (owner == CompanyID::Invalid() && price > 0) return fmt::format("จัดการ {} · ซื้อสิทธิ์เป็นเจ้าของ {}", name, MoneyText(price));
					return fmt::format("จัดการ {}", name);
				}
				case 4: return "สร้างสถานีรถบรรทุกใกล้โรงงาน / Build a truck station near the industry";
				case 5: return fmt::format("ข้อมูลของ {}", name);
				default: break;
			}
		} else {
			switch (s) {
				case 0: return "สร้างสถานีรถบรรทุกที่นี่ / Build a truck station here";
				case 1: return "สร้างโรงงานใหม่ (เมนู Fund new industry) / Fund a new industry";
				case 3: return "สร้างถนน / Build a road";
				case 4: return "สร้างทางรถไฟ / Build a railway";
				case 5: return "ข้อมูลของที่ดินแปลงนี้ / Land area information";
				default: break;
			}
		}
		return std::string();
	}

	void Act(int s)
	{
		if (!this->Why(s).empty()) return;
		const TileIndex t = this->tile;
		const IndustryID iid = this->industry;
		const RmKind k = this->kind;
		this->Close(); // the window is gone after this line: only the copies above are used
		if (k == RmKind::Industry) {
			if (!Industry::IsValidID(iid)) return;
			switch (s) {
				case 0: ShowNegotiateWindow(iid.base()); break;
				case 1: ShowMyContractsWindow(); break;
				case 3: {
					ShowIndustryViewWindow(iid);
					Window *view = FindWindowById(WC_INDUSTRY_VIEW, iid.base());
					const Industry *i = Industry::Get(iid);
					if (view != nullptr && GetIndustryOwner(iid) == CompanyID::Invalid() && GetIndustryOwnershipPrice(i) > 0) ShowIndustryBuyPrompt(view, iid);
					break;
				}
				case 4: ShowBuildRoadToolbar(ROADTYPE_ROAD); break;
				case 5: ShowIndustryViewWindow(iid); break;
				default: break;
			}
		} else {
			switch (s) {
				case 0: ShowBuildRoadToolbar(ROADTYPE_ROAD); break;
				case 1: ShowBuildIndustryWindow(); break;
				case 3: ShowBuildRoadToolbar(ROADTYPE_ROAD); break;
				case 4: ShowBuildRailToolbar(_last_built_railtype); break;
				case 5: ShowLandInfo(t); break;
				default: break;
			}
		}
	}

	/* ---------------- drawing ---------------- */

	void DrawWidget(const Rect &r, [[maybe_unused]] WidgetID widget) const override
	{
		const int d = this->diameter;
		const PixelColour line = PC_BLACK;
		const PixelColour normal = GetColourGradient(COLOUR_GREY, SHADE_DARK);
		const PixelColour hover = GetColourGradient(COLOUR_GREEN, SHADE_LIGHTER);
		const PixelColour off = GetColourGradient(COLOUR_GREY, SHADE_DARKER);
		const PixelColour hub = GetColourGradient(COLOUR_GREY, SHADE_DARKEST);
		std::array<PixelColour, 6> fill{};
		for (int s = 0; s < 6; s++) fill[s] = !this->Why(s).empty() ? off : (s == this->hover ? hover : normal);

		/* Scan lines: runs of the same part of the ring are drawn as one rectangle. */
		for (int y = 0; y < d; y++) {
			int x = 0;
			while (x < d) {
				const int8_t c = this->map[static_cast<size_t>(y) * d + x];
				int x2 = x;
				while (x2 + 1 < d && this->map[static_cast<size_t>(y) * d + x2 + 1] == c) x2++;
				if (c != -1 && c != -3) {
					const PixelColour col = c == -4 ? line : (c == -2 ? hub : fill[c]);
					GfxFillRect(r.left + x, r.top + y, r.left + x2, r.top + y, col);
				}
				x = x2 + 1;
			}
		}

		/* The labels, in the middle of the ring of every slice. */
		const std::array<RmSlice, 6> &labels = this->kind == RmKind::Industry ? RM_INDUSTRY : RM_LAND;
		const double mid = (ScaleGUITrad(RM_OUTER) + ScaleGUITrad(RM_INNER)) / 2.0 + ScaleGUITrad(2);
		const int cx = r.left + d / 2;
		const int cy = r.top + d / 2;
		const int half_w = ScaleGUITrad(38);
		for (int s = 0; s < 6; s++) {
			const double ang = s * 60.0 * 3.14159265358979 / 180.0;
			const int x = cx + static_cast<int>(std::lround(std::sin(ang) * mid));
			const int y = cy - static_cast<int>(std::lround(std::cos(ang) * mid));
			const bool grey = !this->Why(s).empty();
			const TextColour col = grey ? TC_GREY : (s == this->hover ? TC_BLACK : TC_WHITE);
			const int h1 = GetCharacterHeight(FS_NORMAL);
			const int h2 = GetCharacterHeight(FS_SMALL);
			const int top = y - (h1 + h2) / 2;
			DrawString(x - half_w, x + half_w, top, labels[s].thai, col, SA_HOR_CENTER, false, FS_NORMAL);
			DrawString(x - half_w, x + half_w, top + h1, labels[s].english, col, SA_HOR_CENTER, false, FS_SMALL);
		}

		/* The centre: what was clicked. */
		const int hub_w = ScaleGUITrad(RM_HUB) - ScaleGUITrad(3);
		std::string l1, l2;
		if (this->kind == RmKind::Industry && Industry::IsValidID(this->industry)) {
			const Industry *i = Industry::Get(this->industry);
			l1 = GetString(GetIndustrySpec(i->type)->name);
			l2 = i->town != nullptr ? GetString(STR_TOWN_NAME, i->town->index) : std::string();
		} else {
			l1 = "พื้นที่ว่าง";
			l2 = fmt::format("{} x {}", TileX(this->tile), TileY(this->tile));
		}
		DrawString(cx - hub_w, cx + hub_w, cy - GetCharacterHeight(FS_NORMAL), l1, TC_WHITE, SA_HOR_CENTER, false, FS_SMALL);
		DrawString(cx - hub_w, cx + hub_w, cy, l2, TC_LIGHT_BLUE, SA_HOR_CENTER, false, FS_SMALL);

		/* The box under the ring. */
		const int top = r.top + d + ScaleGUITrad(6);
		GfxFillRect(r.left, top, r.right, r.bottom, GetColourGradient(COLOUR_GREY, SHADE_DARKER));
		DrawStringMultiLine(r.left + ScaleGUITrad(6), r.right - ScaleGUITrad(6), top + ScaleGUITrad(3), r.bottom - ScaleGUITrad(2), this->Hint(this->hover),
			(this->hover >= 0 && !this->Why(this->hover).empty()) ? TC_ORANGE : TC_WHITE, SA_TOP | SA_LEFT, false, FS_SMALL);
	}

	/* ---------------- mouse and keys ---------------- */

	void OnMouseOver(Point pt, WidgetID widget) override
	{
		int s = -1;
		if (widget == WID_RM_AREA) {
			const Rect area = this->GetWidget<NWidgetBase>(WID_RM_AREA)->GetCurrentRect();
			s = this->SliceAt(pt.x - area.left, pt.y - area.top);
		}
		if (s != this->hover) {
			this->hover = s;
			this->SetDirty();
		}
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_RM_AREA) return;
		const Rect area = this->GetWidget<NWidgetBase>(WID_RM_AREA)->GetCurrentRect();
		const int s = this->SliceAt(pt.x - area.left, pt.y - area.top);
		if (s < 0) {
			this->Close();
			return;
		}
		this->Act(s);
	}

	bool OnRightClick([[maybe_unused]] Point pt, [[maybe_unused]] WidgetID widget) override
	{
		this->Close();
		return true;
	}

	EventState OnKeyPress([[maybe_unused]] char32_t key, uint16_t keycode) override
	{
		if (keycode == WKC_ESC) {
			this->Close();
			return ES_HANDLED;
		}
		return ES_NOT_HANDLED;
	}

	/** A click outside the menu closes it (the window gets no event for that, so the button is watched here). */
	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		if (!_left_button_down && !_right_button_down) return;
		const int x = _cursor.pos.x, y = _cursor.pos.y;
		if (x < this->left || y < this->top || x >= this->left + this->width || y >= this->top + this->height) this->Close();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_radial_widgets = {
	NWidget(WWT_EMPTY, INVALID_COLOUR, WID_RM_AREA), SetMinimalSize(240, 240), SetFill(0, 0),
};

static WindowDesc _radial_desc(
	WDP_MANUAL, nullptr, 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	WindowDefaultFlag::NoFocus,
	_nested_radial_widgets
);

void ShowRadialMenu(TileIndex tile)
{
	if (!Company::IsValidID(_local_company) || _game_mode != GM_NORMAL) return;
	if (!IsTileType(tile, MP_INDUSTRY) && !IsTileType(tile, MP_CLEAR) && !IsTileType(tile, MP_TREES)) return;
	CloseWindowById(WC_CENTRAL_BANK, RM_WINDOW_NUMBER);
	new RadialWindow(_radial_desc, tile, _cursor.pos);
}

/* ---------------- the right click without a drag ---------------- */

namespace {
struct RmWatch {
	bool down = false;      ///< The right button was down at the last look.
	Point press{};          ///< Where it went down.
	int32_t scroll_x = 0;   ///< Position of the map when it went down.
	int32_t scroll_y = 0;
	int ticks = 0;          ///< How many looks it has been down (20 ms each).
	bool valid = false;     ///< The press started on the map, with nothing in the way.
};
RmWatch _rm_watch;
}

static void RmLook()
{
	if (_game_mode != GM_NORMAL) {
		_rm_watch.down = false;
		return;
	}
	Window *main = GetMainWindow();
	if (main == nullptr || main->viewport == nullptr) return;
	if (_right_button_down) {
		if (!_rm_watch.down) {
			_rm_watch.down = true;
			_rm_watch.press = _cursor.pos;
			_rm_watch.ticks = 0;
			_rm_watch.scroll_x = main->viewport->scrollpos_x;
			_rm_watch.scroll_y = main->viewport->scrollpos_y;
			_rm_watch.valid = FindWindowFromPt(_cursor.pos.x, _cursor.pos.y) == main && _thd.place_mode == HT_NONE && !_ctrl_pressed && !_shift_pressed;
		}
		_rm_watch.ticks++;
		return;
	}
	if (!_rm_watch.down) return;
	_rm_watch.down = false;
	if (!_rm_watch.valid || _rm_watch.ticks > 20) return; // longer than about 400 ms: a drag or a hold, not a click
	const int moved = std::abs(_cursor.pos.x - _rm_watch.press.x) + std::abs(_cursor.pos.y - _rm_watch.press.y);
	if (moved > 3 || main->viewport->scrollpos_x != _rm_watch.scroll_x || main->viewport->scrollpos_y != _rm_watch.scroll_y) return;
	const Point p = GetTileBelowCursor();
	if (p.x == -1) return;
	ShowRadialMenu(TileVirtXY(p.x, p.y));
}

static const IntervalTimer<TimerWindow> _rm_interval = {std::chrono::milliseconds(20), [](auto) { RmLook(); }};
