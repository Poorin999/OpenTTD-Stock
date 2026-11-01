/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file contract_gui.cpp The contracts window (step 4): no console needed.
 * TOP: your offers and contracts (click a row). MIDDLE: the form: the industry, the cargo and the terms (amount a month, price of one unit, months), the market price, the free output,
 * the value of a month and the deposit, and the reason of every grey button. BOTTOM: the buttons.
 * Nothing is selected -> NEW OFFER: pick the industry and the cargo with the arrows, change the terms, press Make offer.
 * A contract is selected -> ANSWER: Accept (as it is), Counter-offer (with the terms of the form), Refuse, Withdraw / end.
 * A button is grey when the rules (contract_math.h, tried on a COPY of the table) would refuse it; the reason is written under the form.
 * The window changes nothing itself: every button posts a command of step 3. */

#include "stdafx.h"
#include "contract_gui.h"
#include "contract_screens.h"
#include "contract.h"
#include "contract_math.h"
#include "central_bank.h"
#include "company_role.h"
#include "industry_owner.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "industry.h"
#include "cargotype.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "widgets/central_bank_widget.h"
#include "core/format.hpp"
#include "game_time/game_time_manager.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

/** True: the text drawn by this window is Thai (as in the design). False: English. Change it here and build again. */
static constexpr bool USE_THAI = true;

static std::string CtText(const char *thai, const char *english)
{
	return USE_THAI ? std::string(thai) : std::string(english);
}

static constexpr int CT_WINDOW_NUMBER = 12;     ///< Window number inside the class WC_CENTRAL_BANK (0 to 11 are used by the other windows of the mod).
static constexpr int CT_PRICE_STEP_PERCENT = 5; ///< The price buttons move the price by 5 percent.
static constexpr int CT_DEFAULT_MONTHS = 6;     ///< The length of a new offer.

static std::string StatusWord(contract::Status s)
{
	switch (s) {
		case contract::Status::Offered: return CtText("เสนอแล้ว", "offered");
		case contract::Status::Countered: return CtText("เสนอกลับ", "counter-offer");
		case contract::Status::Active: return CtText("ใช้งานอยู่", "ACTIVE");
		case contract::Status::Refused: return CtText("ถูกปฏิเสธ", "refused");
		case contract::Status::Cancelled: return CtText("ยกเลิก", "cancelled");
		case contract::Status::Expired: return CtText("หมดอายุ", "expired");
		case contract::Status::Completed: return CtText("จบสัญญา", "completed");
	}
	return "?";
}

/** A price in 1/100 of a money unit as text: 4.50 */
static std::string PriceText(int64_t cents)
{
	return fmt::format("{}.{:02}", cents / contract::PRICE_UNITS, cents % contract::PRICE_UNITS);
}

static int64_t Today()
{
	return static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
}

/** The text of an answer of the rules (empty for Ok). */
static std::string ReasonOf(contract::Result r)
{
	if (r == contract::Result::Ok) return std::string();
	return GetString(ContractErrorOf(r).GetErrorMessage());
}

/** The cargoes that an industry produces. */
static std::vector<CargoType> ProducedCargoes(const Industry *i)
{
	std::vector<CargoType> out;
	if (i == nullptr) return out;
	for (const auto &p : i->produced) {
		if (IsValidCargoType(p.cargo)) out.push_back(p.cargo);
	}
	return out;
}

struct ContractWindow : Window {
	uint32_t selected = 0;                 ///< Id of the selected contract, 0 = new offer.
	uint16_t industry = 0xFFFF;            ///< The industry of the form (new offer).
	CargoType cargo = INVALID_CARGO;       ///< The cargo of the form (new offer).
	contract::Terms draft{};               ///< The terms on the form.

	/* Worked out in OnPaint, drawn in DrawWidget (DrawWidget can not change the window). */
	std::string why_offer, why_accept, why_counter, why_refuse, why_cancel;
	bool offer_mode = true;

	/** The window redraws every second: the days left and the answer of the NPC change without a command. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(1000), [this](auto) {
		this->SetDirty();
	}};

	ContractWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(CT_WINDOW_NUMBER);
		this->ChooseIndustry(+1); // start with the first industry that can be offered to, so the form is never empty
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	/* ---------------- which contracts are mine ---------------- */

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

	/** The contracts of the local company: waiting for me, waiting for the other side, active, closed (newest first inside a group). */
	static std::vector<const contract::Contract *> MyContracts()
	{
		std::vector<const contract::Contract *> rows;
		for (const auto &[id, c] : _contracts.rows) {
			contract::Side side;
			if (SideOfLocal(c, side)) rows.push_back(&c);
		}
		auto rank = [](const contract::Contract *c) {
			contract::Side side = contract::Side::Buyer;
			SideOfLocal(*c, side);
			if (contract::IsOpen(c->status)) return c->turn == side ? 0 : 1;
			return c->status == contract::Status::Active ? 2 : 3;
		};
		std::stable_sort(rows.begin(), rows.end(), [&](const contract::Contract *a, const contract::Contract *b) {
			const int ra = rank(a);
			const int rb = rank(b);
			return ra != rb ? ra < rb : a->id > b->id;
		});
		return rows;
	}

	int RowHeight() const { return GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(3); }

	/** How many rows of the list fit in the panel (the same number for drawing and clicking). */
	int MaxRows(const Rect &r) const
	{
		const int pad = ScaleGUITrad(8);
		return std::max(0, (r.Height() - 2 * pad - this->RowHeight()) / this->RowHeight());
	}

	/* ---------------- the form ---------------- */

	/** Industries that can be offered to: they produce something and the local company does not own them. */
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

	/** The industry and the cargo that the form talks about: the selected contract, or the choice for a new offer. nullptr when there is none. */
	const Industry *FormIndustry(CargoType &cg) const
	{
		if (this->selected != 0) {
			const contract::Contract *c = _contracts.Get(this->selected);
			if (c == nullptr) return nullptr;
			const IndustryID iid{c->industry};
			if (!Industry::IsValidID(iid)) return nullptr;
			cg = c->cargo;
			return Industry::Get(iid);
		}
		if (this->industry == 0xFFFF || !Industry::IsValidID(IndustryID{this->industry})) return nullptr;
		cg = this->cargo;
		return Industry::Get(IndustryID{this->industry});
	}

	/** The terms of a fresh offer: half of what one buyer may still book (at least 1), the market price, 6 months. */
	void ResetDraft()
	{
		this->draft = contract::Terms{1, 1, CT_DEFAULT_MONTHS};
		if (this->industry == 0xFFFF || !Industry::IsValidID(IndustryID{this->industry}) || !IsValidCargoType(this->cargo)) return;
		const Industry *i = Industry::Get(IndustryID{this->industry});
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
		this->industry = list[pos].base();
		const std::vector<CargoType> cargoes = ProducedCargoes(Industry::Get(list[pos]));
		this->cargo = cargoes.empty() ? INVALID_CARGO : cargoes.front();
		this->selected = 0;
		this->ResetDraft();
	}

	void ChooseCargo(int dir)
	{
		if (this->industry == 0xFFFF || !Industry::IsValidID(IndustryID{this->industry})) return;
		const std::vector<CargoType> cargoes = ProducedCargoes(Industry::Get(IndustryID{this->industry}));
		if (cargoes.empty()) return;
		size_t pos = 0;
		for (size_t k = 0; k < cargoes.size(); k++) if (cargoes[k] == this->cargo) pos = k;
		pos = (pos + cargoes.size() + (dir > 0 ? 1 : cargoes.size() - 1)) % cargoes.size();
		this->cargo = cargoes[pos];
		this->selected = 0;
		this->ResetDraft();
	}

	/** The buyer's free cash, 0 when there is no local company. */
	static int64_t LocalCash()
	{
		return Company::IsValidID(_local_company) ? static_cast<int64_t>(GetUnborrowedLiquidCash(_local_company)) : 0;
	}

	/** Build the deal for a new offer of the local company. False when it can not be built. */
	bool BuildDeal(const Industry *i, CargoType cg, contract::Deal &d) const
	{
		if (!Company::IsValidID(_local_company) || i == nullptr) return false;
		d.buyer = _local_company.base();
		d.buyer_year = CompanyRoleYear(_local_company);
		d.industry = i->index.base();
		d.cargo = cg;
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

	/** Which buttons work now, and the reason when one does not: every rule is tried on a COPY of the table, so the window and the command agree. */
	void Evaluate()
	{
		const std::string none_company = CtText("ต้องมีบริษัทของคุณก่อน", "You need a company.");
		const std::string pick = CtText("เลือกสัญญาในรายการก่อน", "Select a contract in the list first.");
		this->why_offer = this->why_accept = this->why_counter = this->why_refuse = this->why_cancel = std::string();
		if (!Company::IsValidID(_local_company)) {
			this->why_offer = this->why_accept = this->why_counter = this->why_refuse = this->why_cancel = none_company;
			return;
		}
		const int64_t today = Today();
		this->offer_mode = this->selected == 0 || _contracts.Get(this->selected) == nullptr;
		if (this->offer_mode) {
			this->selected = 0;
			this->why_accept = this->why_counter = this->why_refuse = this->why_cancel = pick;
			CargoType cg = INVALID_CARGO;
			const Industry *i = this->FormIndustry(cg);
			contract::Deal d;
			if (i == nullptr || !this->BuildDeal(i, cg, d)) {
				this->why_offer = CtText("เลือกโรงงานด้วยปุ่มลูกศรก่อน", "Pick an industry with the arrows first.");
				return;
			}
			contract::Table scratch = _contracts;
			uint32_t id = 0;
			this->why_offer = ReasonOf(scratch.Offer(d, this->draft, ContractMonthlyOutput(i, cg), LocalCash(), today, id));
			return;
		}

		this->why_offer = CtText("กดลูกศรเลือกโรงงานเพื่อเริ่มข้อเสนอใหม่", "Use the industry arrows to start a new offer.");
		const contract::Contract *real = _contracts.Get(this->selected);
		contract::Side side = contract::Side::Buyer;
		const IndustryID iid{real->industry};
		if (!SideOfLocal(*real, side) || !Industry::IsValidID(iid)) {
			this->why_accept = this->why_counter = this->why_refuse = this->why_cancel = CtText("สัญญานี้ไม่ใช่ของคุณหรือโรงงานหายไปแล้ว", "Not your contract, or the industry is gone.");
			return;
		}
		const Industry *i = Industry::Get(iid);
		const int64_t output = ContractMonthlyOutput(i, real->cargo);
		const int64_t buyer_cash = (real->buyer < MAX_COMPANIES && Company::IsValidID(CompanyID{real->buyer})) ? static_cast<int64_t>(GetUnborrowedLiquidCash(CompanyID{real->buyer})) : 0;
		const uint32_t id = real->id;
		{
			contract::Table t = _contracts;
			ContractCurrentSeller(*t.Get(id), t.Get(id)->seller, t.Get(id)->seller_year);
			this->why_accept = ReasonOf(t.Accept(id, side, output, buyer_cash, today));
		}
		{
			contract::Table t = _contracts;
			ContractCurrentSeller(*t.Get(id), t.Get(id)->seller, t.Get(id)->seller_year);
			this->why_counter = ReasonOf(t.Counter(id, side, this->draft, output, buyer_cash, today));
		}
		{
			contract::Table t = _contracts;
			ContractCurrentSeller(*t.Get(id), t.Get(id)->seller, t.Get(id)->seller_year);
			this->why_refuse = ReasonOf(t.Refuse(id, side, today));
		}
		{
			contract::Table t = _contracts;
			ContractCurrentSeller(*t.Get(id), t.Get(id)->seller, t.Get(id)->seller_year);
			contract::Settlement st;
			this->why_cancel = ReasonOf(t.Cancel(id, side, today, st));
		}
	}

	void OnPaint() override
	{
		this->Evaluate();
		this->SetWidgetDisabledState(WID_CT_OFFER, !this->why_offer.empty());
		this->SetWidgetDisabledState(WID_CT_ACCEPT, !this->why_accept.empty());
		this->SetWidgetDisabledState(WID_CT_COUNTER, !this->why_counter.empty());
		this->SetWidgetDisabledState(WID_CT_REFUSE, !this->why_refuse.empty());
		this->SetWidgetDisabledState(WID_CT_CANCEL, !this->why_cancel.empty());
		const bool terms_ok = Company::IsValidID(_local_company);
		for (const WidgetID w : {WID_CT_IND_PREV, WID_CT_IND_NEXT, WID_CT_CARGO_PREV, WID_CT_CARGO_NEXT}) this->SetWidgetDisabledState(w, !terms_ok);
		for (const WidgetID w : {WID_CT_QTY_DOWN, WID_CT_QTY_UP, WID_CT_PRICE_DOWN, WID_CT_PRICE_UP, WID_CT_MONTHS_DOWN, WID_CT_MONTHS_UP}) this->SetWidgetDisabledState(w, !terms_ok);
		this->DrawWidgets();
	}

	/* ---------------- drawing ---------------- */

	void DrawList(const Rect &r) const
	{
		const int pad = ScaleGUITrad(8);
		const int row_h = this->RowHeight();
		const int left = r.left + pad;
		const int right = r.right - pad;
		int y = r.top + pad;
		if (!Company::IsValidID(_local_company)) {
			DrawString(left, right, y, CtText("ต้องมีบริษัทจึงจะทำสัญญาได้", "Only a company can make contracts."), TC_GREY);
			return;
		}
		DrawString(left, right, y, CtText("ข้อเสนอและสัญญาของคุณ (สีเหลือง = รอคุณตอบ) คลิกแถวเพื่อเลือก คลิกซ้ำเพื่อเลิกเลือก", "Your offers and contracts (yellow = waiting for you). Click a row to select it, click again to unselect."), TC_LIGHT_BLUE, SA_LEFT, false, FS_SMALL);
		y += row_h;
		const std::vector<const contract::Contract *> rows = MyContracts();
		if (rows.empty()) {
			DrawString(left, right, y, CtText("ยังไม่มีข้อเสนอหรือสัญญา ใช้ฟอร์มด้านล่างเพื่อยื่นข้อเสนอ", "No offers or contracts yet. Use the form below to make an offer."), TC_GREY);
			return;
		}
		const int64_t today = Today();
		const int max_rows = this->MaxRows(r);
		for (int k = 0; k < static_cast<int>(rows.size()) && k < max_rows; k++) {
			const contract::Contract *c = rows[k];
			contract::Side side = contract::Side::Buyer;
			SideOfLocal(*c, side);
			const bool mine_to_answer = contract::IsOpen(c->status) && c->turn == side;
			if (c->id == this->selected) GfxFillRect(r.left + 1, y, r.right - 1, y + row_h - 1, GREY_SCALE(4));
			const IndustryID iid{c->industry};
			const std::string industry = Industry::IsValidID(iid) ? GetString(STR_INDUSTRY_NAME, iid) : std::string("(gone)");
			const std::string cargo_name = IsValidCargoType(c->cargo) ? GetString(CargoSpec::Get(c->cargo)->name) : std::string("?");
			std::string extra;
			if (contract::IsOpen(c->status)) {
				extra = fmt::format(" {} {}/{}  {} {}", CtText("รอบ", "round"), c->round, contract::MAX_ROUNDS, CtText("เหลือวัน", "days left"), std::max<int64_t>(0, c->expires_day - today));
			} else if (c->status == contract::Status::Active) {
				extra = fmt::format(" {} {}  {} {}/{}", CtText("เหลือเดือน", "months left"), c->months_left, CtText("ส่งแล้ว", "delivered"), c->delivered, c->terms.qty);
			}
			const std::string line = fmt::format("#{}  {}  {} {}  {}/{} @ {} x{}  {}{}", c->id, side == contract::Side::Buyer ? CtText("ซื้อ", "BUY") : CtText("ขาย", "SELL"), industry, cargo_name,
				c->terms.qty, CtText("เดือน", "month"), PriceText(c->terms.price_cents), c->terms.months, StatusWord(c->status), extra);
			DrawString(left, right, y, line, mine_to_answer ? TC_YELLOW : (c->status == contract::Status::Active ? TC_GREEN : (contract::IsClosed(c->status) ? TC_GREY : TC_WHITE)));
			y += row_h;
		}
	}

	void DrawForm(const Rect &r) const
	{
		const int pad = ScaleGUITrad(8);
		const int row_h = this->RowHeight();
		const int left = r.left + pad;
		const int right = r.right - pad;
		int y = r.top + pad;
		auto line = [&](const std::string &text, auto colour, bool small = false) {
			if (y + row_h > r.bottom) return;
			DrawString(left, right, y, text, colour, SA_LEFT, false, small ? FS_SMALL : FS_NORMAL);
			y += row_h;
		};
		if (!Company::IsValidID(_local_company)) return;

		CargoType cg = INVALID_CARGO;
		const Industry *i = this->FormIndustry(cg);
		if (i == nullptr || !IsValidCargoType(cg)) {
			line(CtText("ยังไม่ได้เลือกโรงงาน: กดปุ่ม \"โรงงาน >\" เพื่อเลือกโรงงานที่จะยื่นข้อเสนอ", "No industry yet: press \"Industry >\" to choose the industry for your offer."), TC_YELLOW);
			return;
		}
		int64_t ident = 0;
		int32_t built = 0;
		ContractIndustryIdent(i, ident, built);
		const int64_t output = ContractMonthlyOutput(i, cg);
		const int64_t reserved = _contracts.Reserved(i->index.base(), ident, built, cg);
		const int64_t free_units = contract::FreeOf(output, reserved);
		const int64_t cap = contract::BuyerCapOf(output);
		const int64_t market = GetCargoUnitPriceCents(cg);

		const CompanyID owner = GetIndustryOwner(i->index);
		std::string owner_text = CtText("ระบบ (NPC)", "System (NPC)");
		if (owner != CompanyID::Invalid()) owner_text = owner == _local_company ? CtText("คุณ", "You") : fmt::format("{} {}", CtText("บริษัท", "Company"), owner.base() + 1);

		line(fmt::format("{}: {} (#{})   {}: {}   {}: {}", this->offer_mode ? CtText("ข้อเสนอใหม่ถึง", "New offer to") : CtText("สัญญา", "Contract"), GetString(STR_INDUSTRY_NAME, i->index), i->index.base(),
			CtText("เจ้าของ", "owner"), owner_text, CtText("สินค้า", "cargo"), GetString(CargoSpec::Get(cg)->name)), TC_WHITE);
		line(fmt::format("{}: {} {}   {}: {}   {}: {}   {}: {} ({} {})", CtText("ราคาตลาด", "Market price"), PriceText(market), CtText("ต่อหน่วย", "per unit"), CtText("ผลผลิตต่อเดือน", "Output a month"), output,
			CtText("จองแล้ว", "reserved"), reserved, CtText("ว่าง", "free"), free_units, CtText("ผู้ซื้อคนเดียวไม่เกิน", "one buyer max"), cap), TC_GREY);

		const contract::Terms &t = this->draft;
		const int64_t month_value = contract::MonthValue(t.qty, t.price_cents);
		line(fmt::format("{}: {} / {}   {}: {}   {} {}   {}: {}%", CtText("เงื่อนไขในฟอร์ม", "Terms on the form"), t.qty, CtText("เดือน", "month"), CtText("ราคา", "price"), PriceText(t.price_cents), t.months, CtText("เดือน", "months"),
			CtText("ราคาเทียบตลาด", "price vs market"), market > 0 ? t.price_cents * 100 / market : 0), TC_YELLOW);
		line(fmt::format("{}: {}   {}: {}", CtText("มูลค่าต่อเดือน", "Value of a month"), GetString(STR_JUST_CURRENCY_LONG, month_value), CtText("เงินมัดจำ (จากเงินสดไม่กู้ของผู้ซื้อ)", "Deposit (from the unborrowed cash of the buyer)"), GetString(STR_JUST_CURRENCY_LONG, contract::EscrowOf(t))), TC_WHITE);

		if (!this->offer_mode) {
			const contract::Contract *c = _contracts.Get(this->selected);
			if (c != nullptr && contract::IsOpen(c->status)) {
				contract::Side side = contract::Side::Buyer;
				SideOfLocal(*c, side);
				const int64_t days = std::max<int64_t>(0, c->expires_day - Today());
				line(c->turn == side ? fmt::format("{} {} {}/{}  {}", CtText("ถึงตาคุณตอบ", "YOUR turn."), CtText("รอบ", "Round"), c->round, contract::MAX_ROUNDS, fmt::format("{} {} {}", CtText("เหลือ", ""), days, CtText("วันเกม", "game days left")))
					: fmt::format("{} {}/{}  {} {} {}", CtText("รออีกฝ่ายตอบ รอบ", "Waiting for the other side. Round"), c->round, contract::MAX_ROUNDS, CtText("เหลือ", ""), days, CtText("วันเกม", "game days left")),
					c->turn == side ? TC_YELLOW : TC_WHITE);
				line(fmt::format("{}: {} / {}  {} {}  x {}", CtText("เงื่อนไขล่าสุดบนโต๊ะ", "Last terms on the table"), c->terms.qty, CtText("เดือน", "month"), PriceText(c->terms.price_cents), "", c->terms.months), TC_GREY);
			} else if (c != nullptr && c->status == contract::Status::Active) {
				line(fmt::format("{} {}/{}  {} {}  {} {}  {} {}", CtText("เหลือเดือน", "Months left"), c->months_left, c->months_total, CtText("เงินมัดจำที่ถืออยู่", "escrow held"), GetString(STR_JUST_CURRENCY_LONG, c->escrow),
					CtText("ส่งแล้ว", "delivered"), c->delivered, CtText("ขาดส่งติดกัน", "short months in a row"), c->miss_streak), TC_GREEN);
			} else {
				line(CtText("สัญญานี้ปิดแล้ว จะถูกลืมหลังปิด 30 วัน", "Closed. It is forgotten 30 days after the close."), TC_GREY);
			}
		}

		/* The reason of every grey button. */
		y += ScaleGUITrad(3);
		auto why = [&](const char *name_th, const char *name_en, const std::string &reason) {
			if (!reason.empty()) line(fmt::format("{}: {}", CtText(name_th, name_en), reason), TC_GREY, true);
		};
		why("ยื่นข้อเสนอไม่ได้เพราะ", "Make offer is grey", this->why_offer);
		if (!this->offer_mode) {
			why("ตกลงไม่ได้เพราะ", "Accept is grey", this->why_accept);
			why("เสนอใหม่ไม่ได้เพราะ", "Counter-offer is grey", this->why_counter);
			why("ปฏิเสธไม่ได้เพราะ", "Refuse is grey", this->why_refuse);
			why("ถอน/ยกเลิกไม่ได้เพราะ", "Withdraw / end is grey", this->why_cancel);
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_CT_PANEL) this->DrawList(r);
		if (widget == WID_CT_FORM) this->DrawForm(r);
	}

	/* ---------------- clicks ---------------- */

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_CT_PANEL: {
				const Rect panel = this->GetWidget<NWidgetBase>(WID_CT_PANEL)->GetCurrentRect();
				const int pad = ScaleGUITrad(8);
				const int row_h = this->RowHeight();
				const int first = panel.top + pad + row_h; // one header row
				if (pt.y < first) return;
				const size_t index = static_cast<size_t>((pt.y - first) / row_h);
				const std::vector<const contract::Contract *> rows = MyContracts();
				if (index >= rows.size() || static_cast<int>(index) >= this->MaxRows(panel)) return;
				const contract::Contract *c = rows[index];
				if (c->id == this->selected) {
					this->selected = 0; // click again: back to a new offer
					this->ResetDraft();
				} else {
					this->selected = c->id;
					this->draft = c->terms; // the form starts with the terms on the table
				}
				this->SetDirty();
				return;
			}
			case WID_CT_IND_PREV: this->ChooseIndustry(-1); break;
			case WID_CT_IND_NEXT: this->ChooseIndustry(+1); break;
			case WID_CT_CARGO_PREV: this->ChooseCargo(-1); break;
			case WID_CT_CARGO_NEXT: this->ChooseCargo(+1); break;
			case WID_CT_QTY_DOWN:
			case WID_CT_QTY_UP: {
				const int64_t step = std::max<int64_t>(1, this->draft.qty / 10);
				this->draft.qty = std::clamp<int64_t>(this->draft.qty + (widget == WID_CT_QTY_UP ? step : -step), 1, contract::MAX_QTY);
				break;
			}
			case WID_CT_PRICE_DOWN:
			case WID_CT_PRICE_UP: {
				const int64_t step = std::max<int64_t>(1, this->draft.price_cents * CT_PRICE_STEP_PERCENT / 100);
				this->draft.price_cents = std::clamp<int64_t>(this->draft.price_cents + (widget == WID_CT_PRICE_UP ? step : -step), 1, contract::MAX_PRICE_CENTS);
				break;
			}
			case WID_CT_MONTHS_DOWN:
			case WID_CT_MONTHS_UP:
				this->draft.months = std::clamp<int64_t>(this->draft.months + (widget == WID_CT_MONTHS_UP ? 1 : -1), contract::MIN_MONTHS, contract::MAX_MONTHS);
				break;
			case WID_CT_OFFER: {
				if (!this->why_offer.empty() || this->industry == 0xFFFF) break;
				Command<CMD_CONTRACT_OFFER>::Post(STR_CONTRACT_ERROR_CAPTION, IndustryID{this->industry}, static_cast<uint8_t>(this->cargo), static_cast<uint32_t>(this->draft.qty),
					static_cast<uint32_t>(this->draft.price_cents), static_cast<uint8_t>(this->draft.months));
				break;
			}
			case WID_CT_ACCEPT: if (this->why_accept.empty()) Command<CMD_CONTRACT_ACCEPT>::Post(STR_CONTRACT_ERROR_CAPTION, this->selected); break;
			case WID_CT_REFUSE: if (this->why_refuse.empty()) Command<CMD_CONTRACT_REFUSE>::Post(STR_CONTRACT_ERROR_CAPTION, this->selected); break;
			case WID_CT_CANCEL: if (this->why_cancel.empty()) Command<CMD_CONTRACT_CANCEL>::Post(STR_CONTRACT_ERROR_CAPTION, this->selected); break;
			case WID_CT_COUNTER:
				if (this->why_counter.empty()) {
					Command<CMD_CONTRACT_COUNTER>::Post(STR_CONTRACT_ERROR_CAPTION, this->selected, static_cast<uint32_t>(this->draft.qty), static_cast<uint32_t>(this->draft.price_cents), static_cast<uint8_t>(this->draft.months));
				}
				break;
			default: return;
		}
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_contract_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CT_CAPTION), SetStringTip(STR_CONTRACT_WINDOW_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CT_PANEL), SetMinimalSize(820, 130), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CT_FORM), SetMinimalSize(820, 190), SetFill(1, 0), SetResize(1, 0),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_IND_PREV), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_IND_PREV, STR_CONTRACT_TIP_IND),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_IND_NEXT), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_IND_NEXT, STR_CONTRACT_TIP_IND),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_CARGO_PREV), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_CARGO_PREV, STR_CONTRACT_TIP_CARGO),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_CARGO_NEXT), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_CARGO_NEXT, STR_CONTRACT_TIP_CARGO),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_QTY_DOWN), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_QTY_DOWN, STR_CONTRACT_TIP_TERMS),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_QTY_UP), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_QTY_UP, STR_CONTRACT_TIP_TERMS),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_PRICE_DOWN), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_PRICE_DOWN, STR_CONTRACT_TIP_TERMS),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_PRICE_UP), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_PRICE_UP, STR_CONTRACT_TIP_TERMS),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_MONTHS_DOWN), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_MONTHS_DOWN, STR_CONTRACT_TIP_TERMS),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_MONTHS_UP), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_MONTHS_UP, STR_CONTRACT_TIP_TERMS),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_OFFER), SetMinimalSize(100, 16), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_OFFER, STR_CONTRACT_TIP_OFFER),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_ACCEPT), SetMinimalSize(100, 16), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_ACCEPT, STR_CONTRACT_TIP_ACCEPT),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_COUNTER), SetMinimalSize(100, 16), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_COUNTER, STR_CONTRACT_TIP_COUNTER),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_REFUSE), SetMinimalSize(100, 16), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_REFUSE, STR_CONTRACT_TIP_REFUSE),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CT_CANCEL), SetMinimalSize(100, 16), SetFill(1, 0), SetStringTip(STR_CONTRACT_BTN_CANCEL, STR_CONTRACT_TIP_CANCEL),
	EndContainer(),
};

static WindowDesc _contract_desc(
	WDP_AUTO, "contracts", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_contract_widgets
);

void ShowContractWindow()
{
	/* The contracts window of the design: My Contracts (it opens the Negotiate Contract window). The first window of step 4 above is not opened any more. */
	ShowMyContractsWindow();
}
