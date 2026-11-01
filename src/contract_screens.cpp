/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file contract_screens.cpp The two contract windows of the design (the mockups "Negotiate Contract", "No title bar" and "My Contracts").
 *
 * NEGOTIATE CONTRACT: the industry (arrows), owner / market price / output / free output, your offer (amount a month, price of a unit, months), the value and the deposit,
 * the state of the negotiation and the buttons Cancel / Refuse / Offer again / Accept. With no open contract of the company for the industry it makes a NEW OFFER;
 * with one it ANSWERS it. A button is grey when the rules (contract_math.h, tried on a COPY of the table) would refuse it; the reason is written in the negotiation box.
 * MY CONTRACTS: the numbers on top, one row for every open offer and active contract, the line of the selected one and the buttons Cancel contract / Renew / New contract.
 * Both windows are drawn in one panel (like the Buy gold window): the rectangles come from a layout function that is used for drawing and for the clicks.
 * Nothing here changes the game: every button posts a command of step 3 (contract_cmd.cpp). */

#include "stdafx.h"
#include "contract_screens.h"
#include "contract_gui.h"
#include "contract.h"
#include "contract_math.h"
#include "central_bank.h"
#include "industry_owner.h"
#include "company_role.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "industry.h"
#include "town.h"
#include "cargotype.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "textbuf_gui.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "timer/timer_game_calendar.h"
#include "core/format.hpp"
#include "game_time/game_time_manager.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

/** True: the Negotiate Contract window has the slim drag bar and its title inside the window ("No title bar" design). False: a normal title bar. */
static constexpr bool NEG_SLIM_BAR = true;

static constexpr int NEG_WINDOW_NUMBER = 14; ///< Window numbers inside the class WC_CENTRAL_BANK (0 to 12 are used by the other windows of the mod).
static constexpr int MC_WINDOW_NUMBER = 13;
static constexpr WidgetID WID_SCR_CAPTION = 0;
static constexpr WidgetID WID_SCR_PANEL = 1;

/** Thai and English together, as the design writes every label. */
static std::string Bi(const char *thai, const char *english)
{
	return std::string(thai) + " \xC2\xB7 " + english;
}

static std::string PriceText(int64_t cents)
{
	if (cents % contract::PRICE_UNITS == 0) return fmt::format("{}", cents / contract::PRICE_UNITS);
	return fmt::format("{}.{:02}", cents / contract::PRICE_UNITS, cents % contract::PRICE_UNITS);
}

static std::string MoneyText(int64_t v)
{
	return GetString(STR_JUST_CURRENCY_LONG, v);
}

static int64_t Today()
{
	return static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
}

static int64_t LocalCash()
{
	return Company::IsValidID(_local_company) ? static_cast<int64_t>(GetUnborrowedLiquidCash(_local_company)) : 0;
}

static std::string ReasonOf(contract::Result r)
{
	if (r == contract::Result::Ok) return std::string();
	return GetString(ContractErrorOf(r).GetErrorMessage());
}

/** "80 tonnes" for a cargo: the units of the cargo, as the game writes them. */
static std::string Units(CargoType cargo, int64_t qty)
{
	if (!IsValidCargoType(cargo)) return fmt::format("{}", qty);
	return GetString(CargoSpec::Get(cargo)->units_volume, qty);
}

static std::vector<CargoType> ProducedCargoes(const Industry *i)
{
	std::vector<CargoType> out;
	if (i == nullptr) return out;
	for (const auto &p : i->produced) {
		if (IsValidCargoType(p.cargo)) out.push_back(p.cargo);
	}
	return out;
}

/** Which side of a contract the local company is on. False when the contract is not the company's. */
static bool SideOfLocal(const contract::Contract &c, contract::Side &side)
{
	if (!Company::IsValidID(_local_company)) return false;
	const int32_t year = CompanyRoleYear(_local_company);
	if (c.buyer == _local_company.base() && c.buyer_year == year) {
		side = contract::Side::Buyer;
		return true;
	}
	uint8_t seller = contract::SYSTEM;
	int32_t seller_year = 0;
	ContractCurrentSeller(c, seller, seller_year);
	if (seller != contract::SYSTEM && seller == _local_company.base() && seller_year == year) {
		side = contract::Side::Seller;
		return true;
	}
	return false;
}

static bool Hit(const Rect &r, int x, int y)
{
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

static Rect Shift(const Rect &r, int dx, int dy)
{
	return {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
}

/** The colours of the design (the palette of the game has no free RGB, so these are the nearest shades). */
struct Skin {
	PixelColour face = GetColourGradient(COLOUR_GREY, SHADE_LIGHT);        ///< Window background (#8f9bad).
	PixelColour inset = GetColourGradient(COLOUR_GREY, SHADE_DARKEST);     ///< Dark boxes (#27303d).
	PixelColour inset_edge = GetColourGradient(COLOUR_GREY, SHADE_DARKER);
	PixelColour title = GetColourGradient(COLOUR_DARK_BLUE, SHADE_NORMAL); ///< Title bar (#3e4c60).
	PixelColour line = GetColourGradient(COLOUR_GREY, SHADE_DARK);         ///< Thin lines (#6a7686).
	PixelColour selected = GetColourGradient(COLOUR_GREY, SHADE_DARKER);   ///< Selected row (#3a4658).
	PixelColour track = GetColourGradient(COLOUR_GREY, SHADE_DARKEST);       ///< Empty part of a bar (#131922).
	PixelColour green = GetColourGradient(COLOUR_GREEN, SHADE_LIGHT);      ///< On track (#7bd88f).
	PixelColour amber = GetColourGradient(COLOUR_ORANGE, SHADE_LIGHT);     ///< Behind (#f0b84a).
	PixelColour red = GetColourGradient(COLOUR_RED, SHADE_LIGHT);          ///< Missed (#ff8a80).
	PixelColour mint = GetColourGradient(COLOUR_PALE_GREEN, SHADE_LIGHTER);///< The main button (#a8d3b0).
	PixelColour grey_pill = GetColourGradient(COLOUR_GREY, SHADE_LIGHTER); ///< Offer sent (#aab7c8).
};

/** A raised button. `accent` paints it with the colour of the main action. */
static void DrawButton(const Rect &r, const std::string &text, bool enabled, bool accent = false, FontSize size = FS_NORMAL)
{
	DrawFrameRect(r.left, r.top, r.right, r.bottom, enabled ? (accent ? COLOUR_PALE_GREEN : COLOUR_GREY) : COLOUR_GREY, {});
	const int h = GetCharacterHeight(size);
	DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, h), text, enabled ? TC_BLACK : TC_GREY, SA_HOR_CENTER, false, size);
}

/** A dark box (the "inset" of the design). */
static void DrawInset(const Rect &r)
{
	DrawFrameRect(r.left, r.top, r.right, r.bottom, COLOUR_GREY, {FrameFlag::Lowered, FrameFlag::Darkened});
}

static void DrawSectionLabel(const Rect &r, const std::string &text)
{
	DrawString(r.left, r.right, r.top, text, TC_BLACK, SA_LEFT, false, FS_SMALL);
}

/* ============================================================================================================================================================ */
/* Negotiate Contract                                                                                                                                           */
/* ============================================================================================================================================================ */

/** Unscaled design coordinates of the Negotiate Contract window; the panel is NG_WIDTH wide at the least. */
static constexpr int NG_WIDTH = 450;
static constexpr int NG_TOP = NEG_SLIM_BAR ? 30 : 8;
static constexpr int NG_HEIGHT = NG_TOP + 458;

struct NgLayout {
	Rect heading;
	Rect ind_label, ind_prev, ind_box, ind_next;
	Rect tile_owner, tile_price, tile_output, tile_free;
	Rect offer_label;
	Rect qty_label, qty_minus, qty_field, qty_plus, qty_q10, qty_q50, qty_max;
	Rect price_label, price_minus, price_field, price_plus, price_note;
	Rect months_label, months_minus, months_field, months_plus;
	Rect summary;
	Rect nego_label, nego_round, nego_box;
	Rect b_cancel, b_refuse, b_again, b_accept;
};

static NgLayout NgMakeLayout(int width)
{
	auto s = [](int v) { return ScaleGUITrad(v); };
	NgLayout l;
	const int m = s(10);
	const int w = width;
	int y = s(NG_TOP);
	l.heading = {m, s(6), w - m, s(28)};
	l.ind_label = {m, y, w - m, y + s(12)};
	const int ry = y + s(14);
	l.ind_prev = {m, ry, m + s(32), ry + s(32)};
	l.ind_next = {w - m - s(32), ry, w - m, ry + s(32)};
	l.ind_box = {l.ind_prev.right + s(4), ry, l.ind_next.left - s(4), ry + s(32)};

	const int ty = y + s(54);
	const int tw = (w - 2 * m - s(8)) / 2;
	l.tile_owner = {m, ty, m + tw, ty + s(34)};
	l.tile_price = {w - m - tw, ty, w - m, ty + s(34)};
	l.tile_output = {m, ty + s(40), m + tw, ty + s(74)};
	l.tile_free = {w - m - tw, ty + s(40), w - m, ty + s(74)};

	l.offer_label = {m, y + s(136), w - m, y + s(148)};
	const int lw = s(112);
	const int bw = s(26);
	const int fw = s(76);
	const int rh = s(26);
	auto row = [&](int top, Rect &label, Rect &minus, Rect &field, Rect &plus) {
		label = {m, top, m + lw, top + rh};
		minus = {m + lw + s(4), top, m + lw + s(4) + bw, top + rh};
		field = {minus.right + s(4), top, minus.right + s(4) + fw, top + rh};
		plus = {field.right + s(4), top, field.right + s(4) + bw, top + rh};
	};
	row(y + s(152), l.qty_label, l.qty_minus, l.qty_field, l.qty_plus);
	l.qty_q10 = {l.qty_plus.right + s(6), l.qty_plus.top, l.qty_plus.right + s(6) + s(34), l.qty_plus.bottom};
	l.qty_q50 = {l.qty_q10.right + s(4), l.qty_plus.top, l.qty_q10.right + s(4) + s(34), l.qty_plus.bottom};
	l.qty_max = {l.qty_q50.right + s(4), l.qty_plus.top, l.qty_q50.right + s(4) + s(42), l.qty_plus.bottom};
	row(y + s(184), l.price_label, l.price_minus, l.price_field, l.price_plus);
	l.price_note = {l.price_plus.right + s(8), l.price_plus.top, w - m, l.price_plus.bottom};
	row(y + s(216), l.months_label, l.months_minus, l.months_field, l.months_plus);

	l.summary = {m, y + s(250), w - m, y + s(250) + s(68)};
	l.nego_label = {m, y + s(326), w - m - s(90), y + s(338)};
	l.nego_round = {w - m - s(90), y + s(326), w - m, y + s(338)};
	l.nego_box = {m, y + s(342), w - m, y + s(342) + s(70)};

	const int by = y + s(420);
	const int bh = s(28);
	const int gap = s(6);
	const int total = w - 2 * m;
	const int bwid[4] = {total * 19 / 100, total * 19 / 100, total * 25 / 100, 0};
	l.b_cancel = {m, by, m + bwid[0], by + bh};
	l.b_refuse = {l.b_cancel.right + gap, by, l.b_cancel.right + gap + bwid[1], by + bh};
	l.b_again = {l.b_refuse.right + gap, by, l.b_refuse.right + gap + bwid[2], by + bh};
	l.b_accept = {l.b_again.right + gap, by, w - m, by + bh};
	return l;
}

struct NegotiateWindow : Window {
	uint16_t industry = 0xFFFF;      ///< The industry of the form.
	CargoType cargo = INVALID_CARGO; ///< Its cargo.
	contract::Terms draft{};         ///< The terms on the form.
	uint32_t shown_contract = 0;     ///< The contract whose terms were put on the form (so they are put once, not every frame).

	/* Worked out in OnPaint, drawn in DrawWidget. */
	uint32_t contract_id = 0;        ///< The open or active contract of the company for this industry, 0 = new offer.
	std::string why_offer, why_accept, why_refuse;
	uint64_t quantity_query = 0;     ///< 1 = the amount field is being typed, 2 = the price field.

	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		this->SetDirty();
	}};

	NegotiateWindow(WindowDesc &desc, uint16_t ind, int64_t qty, int64_t price_cents, int64_t months) : Window(desc)
	{
		this->InitNested(NEG_WINDOW_NUMBER);
		this->Retarget(ind, qty, price_cents, months);
	}

	/** Point the form at an industry (and at terms when given). */
	void Retarget(uint16_t ind, int64_t qty, int64_t price_cents, int64_t months)
	{
		if (ind == 0xFFFF || !Industry::IsValidID(IndustryID{ind})) {
			const std::vector<IndustryID> list = Candidates();
			ind = list.empty() ? 0xFFFF : list.front().base();
			/* Prefer an industry that already has a contract of the company. */
			for (const auto &[id, c] : _contracts.rows) {
				contract::Side side;
				if (SideOfLocal(c, side) && !contract::IsClosed(c.status) && Industry::IsValidID(IndustryID{c.industry})) {
					ind = c.industry;
					break;
				}
			}
		}
		this->industry = ind;
		this->shown_contract = 0;
		this->PickCargo();
		this->ResetDraft();
		if (qty > 0) {
			this->draft = contract::Terms{qty, std::clamp<int64_t>(price_cents, 1, contract::MAX_PRICE_CENTS), std::clamp<int64_t>(months, contract::MIN_MONTHS, contract::MAX_MONTHS)};
			this->shown_contract = this->FindContract().id; // the open contract of this industry, if any (the preset terms stay on the form)
		}
		this->SetDirty();
	}

	/* ---------------- the form ---------------- */

	static std::vector<IndustryID> Candidates()
	{
		std::vector<IndustryID> out;
		for (const Industry *i : Industry::Iterate()) {
			if (ProducedCargoes(i).empty()) continue;
			if (Company::IsValidID(_local_company) && IsIndustryOwnedBy(i->index, _local_company)) continue;
			out.push_back(i->index);
		}
		return out;
	}

	const Industry *FormIndustry() const
	{
		if (this->industry == 0xFFFF || !Industry::IsValidID(IndustryID{this->industry})) return nullptr;
		return Industry::Get(IndustryID{this->industry});
	}

	/** The open contract (or else the active one) of the company with this industry, or a dummy with id 0. */
	struct Found {
		uint32_t id = 0;
	};
	Found FindContract() const
	{
		Found f;
		const contract::Contract *best = nullptr;
		for (const auto &[id, c] : _contracts.rows) {
			contract::Side side;
			if (c.industry != this->industry || !SideOfLocal(c, side) || contract::IsClosed(c.status)) continue;
			if (best == nullptr || (contract::IsOpen(c.status) && !contract::IsOpen(best->status))) best = &c;
		}
		if (best != nullptr) f.id = best->id;
		return f;
	}

	void PickCargo()
	{
		const Industry *i = this->FormIndustry();
		const std::vector<CargoType> cargoes = ProducedCargoes(i);
		this->cargo = cargoes.empty() ? INVALID_CARGO : cargoes.front();
		const contract::Contract *c = _contracts.Get(this->FindContract().id);
		if (c != nullptr) this->cargo = c->cargo;
	}

	void ResetDraft()
	{
		this->draft = contract::Terms{1, 1, 6};
		const Industry *i = this->FormIndustry();
		if (i == nullptr || !IsValidCargoType(this->cargo)) return;
		int64_t ident = 0;
		int32_t built = 0;
		ContractIndustryIdent(i, ident, built);
		const int64_t output = ContractMonthlyOutput(i, this->cargo);
		const int64_t room = std::min(_contracts.FreeOutput(this->industry, ident, built, this->cargo, output), contract::BuyerCapOf(output));
		this->draft.qty = std::max<int64_t>(1, room / 2);
		this->draft.price_cents = std::clamp<int64_t>(GetCargoUnitPriceCents(this->cargo), 1, contract::MAX_PRICE_CENTS);
	}

	void ChooseIndustry(int dir)
	{
		const std::vector<IndustryID> list = Candidates();
		if (list.empty()) return;
		size_t pos = 0;
		bool found = false;
		for (size_t k = 0; k < list.size(); k++) {
			if (list[k].base() == this->industry) { pos = k; found = true; }
		}
		pos = found ? (pos + list.size() + (dir > 0 ? 1 : list.size() - 1)) % list.size() : 0;
		this->Retarget(list[pos].base(), 0, 0, 0);
	}

	void ChooseCargo()
	{
		const std::vector<CargoType> cargoes = ProducedCargoes(this->FormIndustry());
		if (cargoes.size() < 2 || this->contract_id != 0) return;
		size_t pos = 0;
		for (size_t k = 0; k < cargoes.size(); k++) if (cargoes[k] == this->cargo) pos = k;
		this->cargo = cargoes[(pos + 1) % cargoes.size()];
		this->ResetDraft();
	}

	int64_t MaxQty() const
	{
		const Industry *i = this->FormIndustry();
		if (i == nullptr || !IsValidCargoType(this->cargo)) return 1;
		int64_t ident = 0;
		int32_t built = 0;
		ContractIndustryIdent(i, ident, built);
		const int64_t output = ContractMonthlyOutput(i, this->cargo);
		return std::max<int64_t>(1, std::min(_contracts.FreeOutput(this->industry, ident, built, this->cargo, output), contract::BuyerCapOf(output)));
	}

	bool BuildDeal(const Industry *i, contract::Deal &d) const
	{
		if (!Company::IsValidID(_local_company) || i == nullptr) return false;
		d.buyer = _local_company.base();
		d.buyer_year = CompanyRoleYear(_local_company);
		d.industry = i->index.base();
		d.cargo = this->cargo;
		if (!ContractIndustryIdent(i, d.ident, d.built)) return false;
		const CompanyID owner = GetIndustryOwner(i->index);
		if (owner == CompanyID::Invalid() || owner.base() >= contract::MAX_COMPANIES_N || !Company::IsValidID(owner)) {
			d.seller = contract::SYSTEM;
			d.seller_year = 0;
		} else {
			d.seller = owner.base();
			d.seller_year = CompanyRoleYear(owner);
		}
		return true;
	}

	/** Which buttons work, and why not. Every rule is tried on a COPY of the table, so the window and the command agree. */
	void Evaluate()
	{
		this->why_offer = this->why_accept = this->why_refuse = std::string();
		this->contract_id = this->FindContract().id;
		const contract::Contract *real = _contracts.Get(this->contract_id);
		if (real != nullptr && this->shown_contract != real->id) {
			this->shown_contract = real->id;
			this->cargo = real->cargo;
			if (contract::IsOpen(real->status)) this->draft = real->terms;
		}
		if (!Company::IsValidID(_local_company)) {
			this->why_offer = this->why_accept = this->why_refuse = Bi("ต้องมีบริษัทของคุณก่อน", "You need a company.");
			return;
		}
		const int64_t today = Today();
		const Industry *i = this->FormIndustry();
		if (i == nullptr) {
			this->why_offer = this->why_accept = this->why_refuse = Bi("ไม่มีโรงงานให้เสนอ", "There is no industry to offer to.");
			return;
		}
		if (real == nullptr) {
			this->why_accept = this->why_refuse = Bi("ยังไม่มีสัญญาให้ตอบ", "No contract to answer yet.");
			contract::Deal d;
			if (!this->BuildDeal(i, d)) {
				this->why_offer = Bi("โรงงานนี้ทำสัญญาไม่ได้", "This industry can not take a contract.");
				return;
			}
			contract::Table scratch = _contracts;
			uint32_t id = 0;
			this->why_offer = ReasonOf(scratch.Offer(d, this->draft, ContractMonthlyOutput(i, this->cargo), LocalCash(), today, id));
			return;
		}

		contract::Side side = contract::Side::Buyer;
		if (!SideOfLocal(*real, side)) {
			this->why_offer = this->why_accept = this->why_refuse = Bi("สัญญานี้ไม่ใช่ของคุณ", "Not your contract.");
			return;
		}
		const int64_t output = ContractMonthlyOutput(i, real->cargo);
		const int64_t buyer_cash = (real->buyer < MAX_COMPANIES && Company::IsValidID(CompanyID{real->buyer})) ? static_cast<int64_t>(GetUnborrowedLiquidCash(CompanyID{real->buyer})) : 0;
		const uint32_t id = real->id;
		auto copy = [&]() {
			contract::Table t = _contracts;
			ContractCurrentSeller(*t.Get(id), t.Get(id)->seller, t.Get(id)->seller_year);
			return t;
		};
		{
			contract::Table t = copy();
			this->why_accept = ReasonOf(t.Accept(id, side, output, buyer_cash, today));
		}
		{
			contract::Table t = copy();
			this->why_offer = ReasonOf(t.Counter(id, side, this->draft, output, buyer_cash, today));
		}
		{
			contract::Table t = copy();
			this->why_refuse = ReasonOf(t.Refuse(id, side, today));
		}
	}

	void OnPaint() override
	{
		this->Evaluate();
		this->DrawWidgets();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	/* ---------------- drawing ---------------- */

	void DrawTile(const Rect &r, const std::string &label, const std::string &value, TextColour colour = TC_WHITE) const
	{
		DrawInset(r);
		const int pad = ScaleGUITrad(8);
		DrawString(r.left + pad, r.right - pad, r.top + ScaleGUITrad(3), label, TC_LIGHT_BLUE, SA_LEFT, false, FS_SMALL);
		DrawString(r.left + pad, r.right - pad, r.top + ScaleGUITrad(3) + GetCharacterHeight(FS_SMALL), value, colour, SA_LEFT, false, FS_NORMAL);
	}

	void DrawField(const Rect &r, const std::string &text) const
	{
		DrawInset(r);
		DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), text, TC_WHITE, SA_HOR_CENTER);
	}

	void DrawRowLabel(const Rect &r, const char *thai, const char *english) const
	{
		DrawString(r.left, r.right, r.top, thai, TC_BLACK, SA_LEFT, false, FS_NORMAL);
		DrawString(r.left, r.right, r.top + GetCharacterHeight(FS_NORMAL) - ScaleGUITrad(1), english, TC_BLACK, SA_LEFT, false, FS_SMALL);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_SCR_PANEL) return;
		const Skin skin;
		const NgLayout base = NgMakeLayout(r.Width());
		auto at = [&](const Rect &x) { return Shift(x, r.left, r.top); };
		GfxFillRect(r.left, r.top, r.right, r.bottom, skin.face);

		if (NEG_SLIM_BAR) {
			const Rect h = at(base.heading);
			DrawString(h.left, h.right, h.top, Bi("ต่อรองสัญญา", "Negotiate Contract"), TC_BLACK, SA_LEFT, false, FS_LARGE);
		}

		const Industry *i = this->FormIndustry();
		const contract::Contract *real = _contracts.Get(this->contract_id);
		DrawSectionLabel(at(base.ind_label), Bi("โรงงาน", "Industry"));
		DrawButton(at(base.ind_prev), "<", true);
		DrawButton(at(base.ind_next), ">", true);
		{
			const Rect b = at(base.ind_box);
			DrawInset(b);
			const int pad = ScaleGUITrad(10);
			if (i == nullptr) {
				DrawString(b.left + pad, b.right - pad, CentreBounds(b.top, b.bottom, GetCharacterHeight(FS_NORMAL)), Bi("ไม่มีโรงงาน", "No industry"), TC_YELLOW);
			} else {
				DrawString(b.left + pad, b.right - pad, b.top + ScaleGUITrad(3), GetString(STR_INDUSTRY_NAME, i->index), TC_WHITE, SA_LEFT, false, FS_NORMAL);
				std::string sub = i->town != nullptr ? GetString(STR_TOWN_NAME, i->town->index) : std::string();
				if (Company::IsValidID(_local_company)) {
					const Company *c = Company::Get(_local_company);
					if (c->location_of_HQ != INVALID_TILE) sub += fmt::format(" \xC2\xB7 {} {}", DistanceManhattan(c->location_of_HQ, i->location.tile), Bi("ช่องจากสำนักงานใหญ่", "tiles from your HQ"));
				}
				DrawString(b.left + pad, b.right - pad, b.top + ScaleGUITrad(3) + GetCharacterHeight(FS_NORMAL), sub, TC_LIGHT_BLUE, SA_LEFT, false, FS_SMALL);
			}
		}

		int64_t output = 0, reserved = 0, free_units = 0, market = 0;
		if (i != nullptr && IsValidCargoType(this->cargo)) {
			int64_t ident = 0;
			int32_t built = 0;
			ContractIndustryIdent(i, ident, built);
			output = ContractMonthlyOutput(i, this->cargo);
			reserved = _contracts.Reserved(i->index.base(), ident, built, this->cargo);
			free_units = contract::FreeOf(output, reserved);
			market = GetCargoUnitPriceCents(this->cargo);
		}
		{
			std::string owner = Bi("ระบบ", "System (NPC)");
			TextColour oc = TC_WHITE;
			if (i != nullptr) {
				const CompanyID o = GetIndustryOwner(i->index);
				if (o != CompanyID::Invalid()) {
					owner = o == _local_company ? Bi("คุณ", "You") : GetString(STR_COMPANY_NAME, o);
					oc = TC_YELLOW;
				}
			}
			this->DrawTile(at(base.tile_owner), Bi("เจ้าของ", "Owner"), owner, oc);
			const std::string cname = IsValidCargoType(this->cargo) ? GetString(CargoSpec::Get(this->cargo)->name) : std::string("?");
			this->DrawTile(at(base.tile_price), Bi("ราคาตลาด", "Market price"), fmt::format("{} / {}", PriceText(market), cname));
			this->DrawTile(at(base.tile_output), Bi("ผลผลิต", "Output"), fmt::format("{} / {}", Units(this->cargo, output), Bi("เดือน", "month")));
			this->DrawTile(at(base.tile_free), Bi("จองแล้ว · ว่าง", "Reserved / free"), fmt::format("{} / {}", reserved, free_units), free_units > 0 ? TC_GREEN : TC_RED);
		}

		DrawSectionLabel(at(base.offer_label), Bi("ข้อเสนอของคุณ", "Your offer"));
		this->DrawRowLabel(at(base.qty_label), "ปริมาณ / เดือน", "Amount per month");
		DrawButton(at(base.qty_minus), "-", true);
		this->DrawField(at(base.qty_field), Units(this->cargo, this->draft.qty));
		DrawButton(at(base.qty_plus), "+", true);
		DrawButton(at(base.qty_q10), "10", true);
		DrawButton(at(base.qty_q50), "50", true);
		DrawButton(at(base.qty_max), "Max", true);
		this->DrawRowLabel(at(base.price_label), "ราคา / หน่วย", "Price per unit");
		DrawButton(at(base.price_minus), "-", true);
		this->DrawField(at(base.price_field), PriceText(this->draft.price_cents));
		DrawButton(at(base.price_plus), "+", true);
		if (market > 0) {
			const int64_t pct = this->draft.price_cents * 100 / market - 100;
			const Rect n = at(base.price_note);
			DrawString(n.left, n.right, CentreBounds(n.top, n.bottom, GetCharacterHeight(FS_SMALL)),
				pct == 0 ? Bi("เท่าราคาตลาด", "at market price") : fmt::format("{}% {}", std::abs(pct), pct < 0 ? Bi("ต่ำกว่าตลาด", "below market") : Bi("สูงกว่าตลาด", "above market")), TC_BLACK, SA_LEFT, false, FS_SMALL);
		}
		this->DrawRowLabel(at(base.months_label), "ระยะเวลา", "Duration");
		DrawButton(at(base.months_minus), "-", true);
		this->DrawField(at(base.months_field), fmt::format("{} {}", this->draft.months, Bi("เดือน", "months")));
		DrawButton(at(base.months_plus), "+", true);

		/* The money box. */
		{
			const Rect b = at(base.summary);
			DrawInset(b);
			const int pad = ScaleGUITrad(10);
			const int rowh = (b.Height() - ScaleGUITrad(8)) / 4;
			const int64_t month_value = contract::MonthValue(this->draft.qty, this->draft.price_cents);
			const int64_t escrow = contract::EscrowOf(this->draft);
			const int64_t cash = LocalCash();
			const int64_t after = cash - escrow;
			auto line = [&](int k, const std::string &label, const std::string &value, TextColour colour) {
				const int top = b.top + ScaleGUITrad(4) + k * rowh;
				DrawString(b.left + pad, b.right - pad, top, label, TC_WHITE, SA_LEFT, false, FS_NORMAL);
				DrawString(b.left + pad, b.right - pad, top, value, colour, SA_RIGHT, false, FS_NORMAL);
			};
			line(0, Bi("มูลค่าต่อเดือน", "Value per month"), MoneyText(month_value), TC_WHITE);
			line(1, Bi("เงินมัดจำ (escrow)", "Deposit"), MoneyText(escrow), TC_WHITE);
			line(2, Bi("เงินสดที่ใช้ได้", "Usable cash"), MoneyText(cash), TC_WHITE);
			line(3, Bi("เงินสดหลังมัดจำ", "Cash after deposit"), MoneyText(after), after >= 0 ? TC_GREEN : TC_RED);
		}

		/* The negotiation. */
		{
			DrawSectionLabel(at(base.nego_label), Bi("การเจรจา", "Negotiation"));
			const Rect rr = at(base.nego_round);
			DrawString(rr.left, rr.right, rr.top, real != nullptr && contract::IsOpen(real->status) ? fmt::format("Round {} of {}", real->round, contract::MAX_ROUNDS) : std::string("Round - of 3"), TC_BLACK, SA_RIGHT, false, FS_SMALL);
			const Rect b = at(base.nego_box);
			DrawInset(b);
			const int pad = ScaleGUITrad(10);
			const int rowh = GetCharacterHeight(FS_SMALL) + ScaleGUITrad(3);
			int y = b.top + ScaleGUITrad(4);
			auto line = [&](const std::string &left, const std::string &right, TextColour colour) {
				if (y + rowh > b.bottom) return;
				DrawString(b.left + pad, b.right - pad, y, left, TC_LIGHT_BLUE, SA_LEFT, false, FS_SMALL);
				DrawString(b.left + pad, b.right - pad, y, right, colour, SA_RIGHT, false, FS_SMALL);
				y += rowh;
			};
			if (real == nullptr) {
				line(Bi("ยังไม่มีการเจรจา", "No negotiation yet"), Bi("กด เสนอใหม่ เพื่อส่งข้อเสนอ", "Press Offer again to send your terms"), TC_WHITE);
			} else {
				contract::Side side = contract::Side::Buyer;
				SideOfLocal(*real, side);
				const std::string t = fmt::format("{} {} {}", Units(real->cargo, real->terms.qty), Bi("ที่", "at"), PriceText(real->terms.price_cents));
				if (contract::IsOpen(real->status)) {
					const int64_t days = std::max<int64_t>(0, real->expires_day - Today());
					if (real->turn == side) {
						line(Bi("อีกฝ่าย", "Other side"), fmt::format("{}: {}", real->round > 1 ? Bi("เสนอกลับ", "Counter") : Bi("ข้อเสนอ", "Offer"), t), TC_ORANGE);
						line(Bi("ถึงตาคุณตอบ", "Your turn"), fmt::format("{} {} {}", Bi("หมดเวลาใน", "expires in"), days, Bi("วัน", "days")), TC_ORANGE);
					} else {
						line(Bi("คุณ", "You"), t, TC_WHITE);
						line(Bi("รออีกฝ่ายตอบ", "Waiting for the other side"), fmt::format("{} {} {}", Bi("หมดเวลาใน", "expires in"), days, Bi("วัน", "days")), TC_ORANGE);
					}
				} else {
					line(Bi("สัญญาที่ใช้งานอยู่", "Active contract"), t, TC_GREEN);
					line(Bi("เหลือเดือน", "Months left"), fmt::format("{} / {}", real->months_left, real->months_total), TC_WHITE);
					line(Bi("ส่งแล้วเดือนนี้", "Delivered this month"), Units(real->cargo, real->delivered), TC_WHITE);
				}
			}
			const std::string &why = real != nullptr && !contract::IsOpen(real->status) ? this->why_offer : (!this->why_offer.empty() ? this->why_offer : (real != nullptr ? this->why_accept : std::string()));
			if (!why.empty()) {
				DrawString(b.left + pad, b.right - pad, std::max(y, b.bottom - rowh - ScaleGUITrad(2)), why, TC_GREY, SA_LEFT, false, FS_SMALL);
			}
		}

		/* The buttons. */
		{
			const bool open = real != nullptr && contract::IsOpen(real->status);
			std::string accept = Bi("ตกลง", "Accept");
			if (open) accept = fmt::format("{} {} \xC2\xB7 Accept", Bi("ตกลง", "").substr(0, 9), PriceText(real->terms.price_cents));
			DrawButton(at(base.b_cancel), Bi("ยกเลิก", "Cancel"), true);
			DrawButton(at(base.b_refuse), Bi("ปฏิเสธ", "Refuse"), this->why_refuse.empty() && open);
			DrawButton(at(base.b_again), Bi("เสนอใหม่", "Offer again"), this->why_offer.empty());
			DrawButton(at(base.b_accept), accept, this->why_accept.empty() && open, true);
		}
	}

	/* ---------------- clicks ---------------- */

	void Step(int64_t &value, int64_t delta, int64_t lo, int64_t hi)
	{
		value = std::clamp<int64_t>(value + delta, lo, hi);
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_SCR_PANEL) return;
		const Rect panel = this->GetWidget<NWidgetBase>(WID_SCR_PANEL)->GetCurrentRect();
		const NgLayout l = NgMakeLayout(panel.Width());
		const int x = pt.x - panel.left;
		const int y = pt.y - panel.top;
		const int64_t max_qty = this->MaxQty();
		const bool open = _contracts.Get(this->contract_id) != nullptr && contract::IsOpen(_contracts.Get(this->contract_id)->status);

		if (Hit(l.ind_prev, x, y)) {
			this->ChooseIndustry(-1);
		} else if (Hit(l.ind_next, x, y)) {
			this->ChooseIndustry(+1);
		} else if (Hit(l.tile_price, x, y)) {
			this->ChooseCargo();
		} else if (Hit(l.qty_minus, x, y)) {
			this->Step(this->draft.qty, -std::max<int64_t>(1, this->draft.qty / 10), 1, contract::MAX_QTY);
		} else if (Hit(l.qty_plus, x, y)) {
			this->Step(this->draft.qty, std::max<int64_t>(1, this->draft.qty / 10), 1, contract::MAX_QTY);
		} else if (Hit(l.qty_q10, x, y)) {
			this->draft.qty = std::min<int64_t>(10, max_qty);
		} else if (Hit(l.qty_q50, x, y)) {
			this->draft.qty = std::min<int64_t>(50, max_qty);
		} else if (Hit(l.qty_max, x, y)) {
			this->draft.qty = max_qty;
		} else if (Hit(l.qty_field, x, y)) {
			this->quantity_query = 1;
			ShowQueryString({}, STR_JUST_INT, 10, this, CS_NUMERAL, {});
		} else if (Hit(l.price_minus, x, y)) {
			this->Step(this->draft.price_cents, -std::max<int64_t>(1, this->draft.price_cents * 5 / 100), 1, contract::MAX_PRICE_CENTS);
		} else if (Hit(l.price_plus, x, y)) {
			this->Step(this->draft.price_cents, std::max<int64_t>(1, this->draft.price_cents * 5 / 100), 1, contract::MAX_PRICE_CENTS);
		} else if (Hit(l.months_minus, x, y)) {
			this->Step(this->draft.months, -1, contract::MIN_MONTHS, contract::MAX_MONTHS);
		} else if (Hit(l.months_plus, x, y)) {
			this->Step(this->draft.months, +1, contract::MIN_MONTHS, contract::MAX_MONTHS);
		} else if (Hit(l.b_cancel, x, y)) {
			this->Close();
			return;
		} else if (Hit(l.b_refuse, x, y)) {
			if (open && this->why_refuse.empty()) Command<CMD_CONTRACT_REFUSE>::Post(STR_CONTRACT_ERROR_CAPTION, this->contract_id);
		} else if (Hit(l.b_again, x, y)) {
			if (this->why_offer.empty()) {
				if (open) {
					Command<CMD_CONTRACT_COUNTER>::Post(STR_CONTRACT_ERROR_CAPTION, this->contract_id, static_cast<uint32_t>(this->draft.qty), static_cast<uint32_t>(this->draft.price_cents), static_cast<uint8_t>(this->draft.months));
				} else if (this->industry != 0xFFFF) {
					Command<CMD_CONTRACT_OFFER>::Post(STR_CONTRACT_ERROR_CAPTION, IndustryID{this->industry}, static_cast<uint8_t>(this->cargo), static_cast<uint32_t>(this->draft.qty),
						static_cast<uint32_t>(this->draft.price_cents), static_cast<uint8_t>(this->draft.months));
				}
			}
		} else if (Hit(l.b_accept, x, y)) {
			if (open && this->why_accept.empty()) Command<CMD_CONTRACT_ACCEPT>::Post(STR_CONTRACT_ERROR_CAPTION, this->contract_id);
		}
		this->SetDirty();
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value() || str->empty() || this->quantity_query != 1) return;
		int64_t v = 0;
		for (const char ch : *str) {
			if (ch < '0' || ch > '9') return;
			v = std::min<int64_t>(v * 10 + (ch - '0'), contract::MAX_QTY);
		}
		this->draft.qty = std::clamp<int64_t>(v, 1, contract::MAX_QTY);
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_negotiate_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_SCR_CAPTION), SetStringTip(STR_EMPTY, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_SCR_PANEL), SetMinimalSize(NG_WIDTH, NG_HEIGHT), SetFill(1, 1),
	EndContainer(),
};

static WindowDesc _negotiate_desc(
	WDP_CENTER, "contract_negotiate", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_negotiate_widgets
);

void ShowNegotiateWindow(uint16_t industry, int64_t qty, int64_t price_cents, int64_t months)
{
	if (!Company::IsValidID(_local_company)) return;
	Window *w = FindWindowById(WC_CENTRAL_BANK, NEG_WINDOW_NUMBER);
	if (w != nullptr) {
		static_cast<NegotiateWindow *>(w)->Retarget(industry, qty, price_cents, months);
		BringWindowToFrontById(WC_CENTRAL_BANK, NEG_WINDOW_NUMBER);
		return;
	}
	new NegotiateWindow(_negotiate_desc, industry, qty, price_cents, months);
}

bool IsNegotiateWindowOpen()
{
	return FindWindowById(WC_CENTRAL_BANK, NEG_WINDOW_NUMBER) != nullptr;
}

/* ============================================================================================================================================================ */
/* My Contracts                                                                                                                                                 */
/* ============================================================================================================================================================ */

static constexpr int MC_WIDTH = 640;
static constexpr int MC_ROWS = 6;
static constexpr int MC_ROW_H = 28;
static constexpr int MC_HEIGHT = 10 + 36 + 8 + 22 + MC_ROWS * MC_ROW_H + 8 + 30 + 8 + 44;

struct McLayout {
	Rect tile[4];
	Rect table;
	Rect head;
	Rect rows[MC_ROWS];
	int col_x[7]; ///< Left edges of the six columns, and the right edge of the table.
	Rect detail;
	Rect b_cancel, b_renew, b_new;
};

static McLayout McMakeLayout(int width)
{
	auto s = [](int v) { return ScaleGUITrad(v); };
	McLayout l;
	const int m = s(10);
	const int w = width;
	const int gap = s(8);
	const int tw = (w - 2 * m - 3 * gap) / 4;
	int y = s(10);
	for (int k = 0; k < 4; k++) l.tile[k] = {m + k * (tw + gap), y, m + k * (tw + gap) + tw, y + s(36)};
	y += s(44);
	l.table = {m, y, w - m, y + s(22) + MC_ROWS * s(MC_ROW_H)};
	l.head = {m, y, w - m, y + s(22)};
	for (int k = 0; k < MC_ROWS; k++) l.rows[k] = {m, y + s(22) + k * s(MC_ROW_H), w - m, y + s(22) + (k + 1) * s(MC_ROW_H)};
	/* Weights of the design: 1.7 .9 .8 1.1 .8 .9 */
	const int weights[6] = {17, 9, 8, 11, 8, 9};
	const int inner = (w - 2 * m) - s(20);
	int x = m + s(10);
	for (int k = 0; k < 6; k++) {
		l.col_x[k] = x;
		x += inner * weights[k] / 62;
	}
	l.col_x[6] = w - m - s(10);
	y = l.table.bottom + s(8);
	l.detail = {m, y, w - m, y + s(30)};
	const int by = l.detail.bottom + s(8) + s(2);
	const int bh = s(28);
	l.b_new = {w - m - s(150), by, w - m, by + bh};
	l.b_renew = {l.b_new.left - s(6) - s(120), by, l.b_new.left - s(6), by + bh};
	l.b_cancel = {l.b_renew.left - s(6) - s(160), by, l.b_renew.left - s(6), by + bh};
	return l;
}

struct MyContractsWindow : Window {
	uint32_t selected = 0; ///< The selected contract id, 0 = none.

	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		this->SetDirty();
	}};

	MyContractsWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(MC_WINDOW_NUMBER);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	/** The open offers and the active contracts of the company: your turn first, then active, then the offers that wait for the other side. */
	static std::vector<const contract::Contract *> Rows()
	{
		std::vector<const contract::Contract *> rows;
		for (const auto &[id, c] : _contracts.rows) {
			contract::Side side;
			if (SideOfLocal(c, side) && !contract::IsClosed(c.status)) rows.push_back(&c);
		}
		auto rank = [](const contract::Contract *c) {
			contract::Side side = contract::Side::Buyer;
			SideOfLocal(*c, side);
			if (contract::IsOpen(c->status)) return c->turn == side ? 0 : 2;
			return 1;
		};
		std::stable_sort(rows.begin(), rows.end(), [&](const contract::Contract *a, const contract::Contract *b) {
			const int ra = rank(a);
			const int rb = rank(b);
			return ra != rb ? ra < rb : a->id > b->id;
		});
		return rows;
	}

	enum class State : uint8_t { OnTrack, Behind, Missed, OfferSent, YourTurn };

	static State StateOf(const contract::Contract &c)
	{
		if (contract::IsOpen(c.status)) {
			contract::Side side = contract::Side::Buyer;
			SideOfLocal(c, side);
			return c.turn == side ? State::YourTurn : State::OfferSent;
		}
		if (c.miss_streak > 0) return State::Missed;
		/* How far into the month are we (30 days of the game clock)? The delivery of the month must keep up with it. */
		const int64_t day = Today() % 30 + 1;
		return c.delivered * 30 < c.terms.qty * day ? State::Behind : State::OnTrack;
	}

	/** Why Cancel is grey (empty = it works), tried on a copy of the table like the command does. */
	std::string WhyCancel(const contract::Contract &c) const
	{
		contract::Side side = contract::Side::Buyer;
		if (!SideOfLocal(c, side)) return Bi("ไม่ใช่สัญญาของคุณ", "Not your contract.");
		contract::Table t = _contracts;
		ContractCurrentSeller(*t.Get(c.id), t.Get(c.id)->seller, t.Get(c.id)->seller_year);
		contract::Settlement st;
		return ReasonOf(t.Cancel(c.id, side, Today(), st));
	}

	void OnPaint() override
	{
		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_SCR_PANEL) return;
		const Skin skin;
		const McLayout base = McMakeLayout(r.Width());
		auto at = [&](const Rect &x) { return Shift(x, r.left, r.top); };
		GfxFillRect(r.left, r.top, r.right, r.bottom, skin.face);
		const int pad = ScaleGUITrad(10);
		const std::vector<const contract::Contract *> rows = Rows();

		/* The numbers on top. */
		int64_t active = 0, due = 0, delivered = 0, escrow = 0;
		for (const contract::Contract *c : rows) {
			if (c->status != contract::Status::Active) continue;
			active++;
			due += c->terms.qty;
			delivered += c->delivered;
			escrow += c->escrow;
		}
		const std::string labels[4] = {Bi("สัญญาที่ใช้งาน", "Active"), Bi("ต้องส่งเดือนนี้", "Due this month"), Bi("ส่งแล้ว", "Delivered"), Bi("เงินมัดจำ", "Held in escrow")};
		const std::string values[4] = {fmt::format("{}", active), fmt::format("{}", due), fmt::format("{}", delivered), MoneyText(static_cast<int64_t>(escrow))};
		for (int k = 0; k < 4; k++) {
			const Rect t = at(base.tile[k]);
			DrawInset(t);
			DrawString(t.left + pad, t.right - pad, t.top + ScaleGUITrad(3), labels[k], TC_LIGHT_BLUE, SA_LEFT, false, FS_SMALL);
			DrawString(t.left + pad, t.right - pad, t.top + ScaleGUITrad(3) + GetCharacterHeight(FS_SMALL), values[k], TC_WHITE, SA_LEFT, false, FS_NORMAL);
		}

		/* The table. */
		const Rect tb = at(base.table);
		DrawInset(tb);
		const std::string heads[6] = {Bi("โรงงาน", "Industry"), Bi("สินค้า", "Cargo"), Bi("ราคา", "Price"), Bi("ส่งแล้ว / ตกลง", "Delivered"), Bi("เหลือ", "Left"), Bi("สถานะ", "Status")};
		const Rect hd = at(base.head);
		for (int k = 0; k < 6; k++) {
			const int left = r.left + base.col_x[k];
			const int right = r.left + base.col_x[k + 1] - ScaleGUITrad(4);
			DrawString(left, right, hd.top + ScaleGUITrad(4), heads[k], TC_LIGHT_BLUE, SA_LEFT, false, FS_SMALL);
		}
		GfxFillRect(hd.left + 1, hd.bottom - 1, hd.right - 1, hd.bottom - 1, skin.line);

		if (!Company::IsValidID(_local_company)) {
			DrawString(tb.left + pad, tb.right - pad, tb.top + ScaleGUITrad(30), Bi("ต้องมีบริษัทจึงจะทำสัญญาได้", "Only a company can make contracts."), TC_GREY);
		} else if (rows.empty()) {
			DrawString(tb.left + pad, tb.right - pad, tb.top + ScaleGUITrad(30), Bi("ยังไม่มีสัญญา กด สัญญาใหม่ เพื่อเริ่ม", "No contracts yet. Press New contract to start."), TC_GREY);
		}
		const int shown = std::min<int>(MC_ROWS, static_cast<int>(rows.size()));
		for (int k = 0; k < shown; k++) {
			const contract::Contract &c = *rows[k];
			const Rect row = at(base.rows[k]);
			if (c.id == this->selected) GfxFillRect(row.left + 1, row.top, row.right - 1, row.bottom - 1, skin.selected);
			const int top = CentreBounds(row.top, row.bottom, GetCharacterHeight(FS_SMALL));
			auto cell = [&](int col, const std::string &text, TextColour colour = TC_WHITE, FontSize size = FS_SMALL) {
				DrawString(r.left + base.col_x[col], r.left + base.col_x[col + 1] - ScaleGUITrad(4), CentreBounds(row.top, row.bottom, GetCharacterHeight(size)), text, colour, SA_LEFT, false, size);
			};
			const IndustryID iid{c.industry};
			cell(0, Industry::IsValidID(iid) ? GetString(STR_INDUSTRY_NAME, iid) : std::string("(gone)"), TC_WHITE, FS_NORMAL);
			cell(1, IsValidCargoType(c.cargo) ? GetString(CargoSpec::Get(c.cargo)->name) : std::string("?"));
			cell(2, PriceText(c.terms.price_cents));
			const State st = StateOf(c);
			const bool is_open = contract::IsOpen(c.status);
			if (is_open) {
				cell(3, st == State::YourTurn ? Bi("รอคุณตอบ", "waiting for you") : Bi("รอเจ้าของตอบ", "waiting for owner"), TC_GREY);
				cell(4, "-");
			} else {
				cell(3, fmt::format("{} / {}", c.delivered, c.terms.qty));
				const int bx0 = r.left + base.col_x[3];
				const int bx1 = r.left + base.col_x[4] - ScaleGUITrad(8);
				const int by = row.bottom - ScaleGUITrad(7);
				GfxFillRect(bx0, by, bx1, by + ScaleGUITrad(3), skin.track);
				const int64_t pct = c.terms.qty > 0 ? std::clamp<int64_t>(c.delivered * 100 / c.terms.qty, 0, 100) : 0;
				const PixelColour bar = st == State::OnTrack ? skin.green : (st == State::Behind ? skin.amber : skin.red);
				if (pct > 0) GfxFillRect(bx0, by, bx0 + (bx1 - bx0) * static_cast<int>(pct) / 100, by + ScaleGUITrad(3), bar);
				cell(4, fmt::format("{} {}", c.months_left, Bi("เดือน", "mo")));
			}
			/* The pill. */
			std::string pill;
			PixelColour pc = skin.grey_pill;
			switch (st) {
				case State::OnTrack: pill = "On track"; pc = skin.green; break;
				case State::Behind: pill = "Behind"; pc = skin.amber; break;
				case State::Missed: pill = "Missed"; pc = skin.red; break;
				case State::OfferSent: pill = "Offer sent"; pc = skin.grey_pill; break;
				case State::YourTurn: pill = "Your turn"; pc = skin.amber; break;
			}
			const int px0 = r.left + base.col_x[5];
			const int px1 = std::min<int>(r.left + base.col_x[6], px0 + GetStringBoundingBox(pill, FS_SMALL).width + ScaleGUITrad(14));
			GfxFillRect(px0, top - ScaleGUITrad(2), px1, top + GetCharacterHeight(FS_SMALL) + ScaleGUITrad(1), pc);
			DrawString(px0, px1, top, pill, TC_BLACK, SA_HOR_CENTER, false, FS_SMALL);
		}
		if (static_cast<int>(rows.size()) > MC_ROWS) {
			DrawString(tb.left + pad, tb.right - pad, tb.bottom - GetCharacterHeight(FS_SMALL) - 1, fmt::format("+{}", rows.size() - MC_ROWS), TC_GREY, SA_RIGHT, false, FS_SMALL);
		}

		/* The line of the selected contract. */
		const Rect dt = at(base.detail);
		DrawInset(dt);
		const contract::Contract *sel = _contracts.Get(this->selected);
		std::string detail = Bi("เลือกสัญญาในตารางเพื่อดูรายละเอียด", "Select a contract in the table for its details.");
		if (sel != nullptr) {
			const IndustryID iid{sel->industry};
			const std::string name = Industry::IsValidID(iid) ? GetString(STR_INDUSTRY_NAME, iid) : std::string("(gone)");
			if (contract::IsOpen(sel->status)) {
				detail = fmt::format("{}: {} {} {}  {} {} {}", name, Units(sel->cargo, sel->terms.qty), Bi("ที่", "at"), PriceText(sel->terms.price_cents), Bi("หมดเวลาใน", "expires in"),
					std::max<int64_t>(0, sel->expires_day - Today()), Bi("วัน", "days"));
			} else {
				const int64_t left = std::max<int64_t>(0, sel->terms.qty - sel->delivered);
				detail = fmt::format("{}: {} {} {}. {} {}% {}", name, Units(sel->cargo, left), Bi("ที่ต้องส่งอีกเดือนนี้", "left to deliver this month"), "", Bi("ส่งไม่ครบเสียค่าปรับ", "Missing the amount costs a penalty of"), contract::PENALTY_PERCENT, Bi("ของส่วนที่ขาด", "of the shortfall"));
			}
		}
		DrawString(dt.left + pad, dt.right - pad, CentreBounds(dt.top, dt.bottom, GetCharacterHeight(FS_SMALL)), detail, TC_WHITE, SA_LEFT, false, FS_SMALL);

		/* The buttons. */
		const bool can_cancel = sel != nullptr && this->WhyCancel(*sel).empty();
		const bool can_renew = sel != nullptr && Industry::IsValidID(IndustryID{sel->industry});
		DrawButton(at(base.b_cancel), Bi("ยกเลิกสัญญา", "Cancel contract"), can_cancel);
		DrawButton(at(base.b_renew), Bi("ต่อสัญญา", "Renew"), can_renew);
		DrawButton(at(base.b_new), Bi("สัญญาใหม่", "New contract"), Company::IsValidID(_local_company), true);
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_SCR_PANEL) return;
		const Rect panel = this->GetWidget<NWidgetBase>(WID_SCR_PANEL)->GetCurrentRect();
		const McLayout l = McMakeLayout(panel.Width());
		const int x = pt.x - panel.left;
		const int y = pt.y - panel.top;
		const std::vector<const contract::Contract *> rows = Rows();
		const contract::Contract *sel = _contracts.Get(this->selected);

		if (Hit(l.table, x, y)) {
			for (int k = 0; k < MC_ROWS && k < static_cast<int>(rows.size()); k++) {
				if (!Hit(l.rows[k], x, y)) continue;
				this->selected = rows[k]->id == this->selected ? 0 : rows[k]->id;
				if (click_count > 1 && this->selected != 0) ShowNegotiateWindow(rows[k]->industry);
			}
		} else if (Hit(l.b_cancel, x, y)) {
			if (sel != nullptr && this->WhyCancel(*sel).empty()) Command<CMD_CONTRACT_CANCEL>::Post(STR_CONTRACT_ERROR_CAPTION, sel->id);
		} else if (Hit(l.b_renew, x, y)) {
			if (sel != nullptr) ShowNegotiateWindow(sel->industry, sel->terms.qty, sel->terms.price_cents, sel->terms.months);
		} else if (Hit(l.b_new, x, y)) {
			ShowNegotiateWindow();
		}
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_my_contracts_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_SCR_CAPTION), SetStringTip(STR_CONTRACT_WINDOW_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_SCR_PANEL), SetMinimalSize(MC_WIDTH, MC_HEIGHT), SetFill(1, 1),
	EndContainer(),
};

static WindowDesc _my_contracts_desc(
	WDP_AUTO, "my_contracts", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_my_contracts_widgets
);

void ShowMyContractsWindow()
{
	Window *w = FindWindowById(WC_CENTRAL_BANK, MC_WINDOW_NUMBER);
	if (w != nullptr) {
		w->SetDirty();
		BringWindowToFrontById(WC_CENTRAL_BANK, MC_WINDOW_NUMBER);
		return;
	}
	new MyContractsWindow(_my_contracts_desc);
}
