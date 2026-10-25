/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file market_news_ai.h The news writer ("AI presenter") of the mod. It turns an event (kind, severity, numbers) into a story: a headline, a body, an
 * analyst line and the name of the presenter.
 *
 * It is a rule based writer, NOT a language model: every kind of event has two headlines and two bodies, the variant of the event chooses one of each
 * and one of three presenters, and the analyst line is built from the real numbers of the event (the push on gold and on the shares). The result
 * depends only on the event, so every client of a network game shows the same story, and a savegame needs to keep only the numbers.
 * Thai text. (English texts are not written yet.)
 * This file needs only the standard library.
 */

#ifndef MARKET_NEWS_AI_H
#define MARKET_NEWS_AI_H

#include <cstdint>
#include <string>

#include "market_events.h"

/** The text of one story. */
struct MarketStory {
	std::string headline;  ///< One line.
	std::string body;      ///< A few lines.
	std::string analysis;  ///< What it means for gold and shares, with the numbers.
	std::string reporter;  ///< The presenter.
	std::string impact;    ///< The size of the news: small, medium or big.
};

/** Texts of one kind of event: two headlines, two bodies. {C} stands for the name of the company. */
struct MarketStoryTexts {
	const char *headline[2];
	const char *body[2];
};

inline const MarketStoryTexts &MarketStoryTextsOf(MarketEventKind kind)
{
	static const MarketStoryTexts table[MARKET_EVENT_KIND_COUNT] = {
		/* EconomicBoom */
		{ { "เศรษฐกิจโตเกินคาด นักลงทุนแห่ซื้อหุ้น", "ดัชนีความเชื่อมั่นพุ่ง ตลาดคึกคักทั่วประเทศ" },
		  { "ตัวเลขการบริโภคและการขนส่งสินค้าขยายตัวแรงกว่าที่นักวิเคราะห์คาดไว้ ทำให้นักลงทุนเทเงินเข้าตลาดหุ้น ขณะที่ความต้องการถือทองคำเพื่อหลบภัยลดลง",
		    "บรรยากาศการค้าคึกคักต่อเนื่อง ผู้ประกอบการหลายรายประกาศขยายกิจการ ราคาหุ้นได้แรงหนุน ส่วนทองคำถูกเทขายบางส่วน" } },
		/* Recession */
		{ { "ผวาเศรษฐกิจถดถอย นักลงทุนเทขายหุ้น", "ตัวเลขการผลิตหดตัว ตลาดหุ้นร่วงหนัก" },
		  { "ข้อมูลล่าสุดชี้ว่ากิจกรรมทางเศรษฐกิจชะลอลงมากกว่าคาด นักลงทุนจึงลดความเสี่ยงโดยขายหุ้นและหันไปถือทองคำ",
		    "ธุรกิจหลายแห่งปรับลดประมาณการกำไร ความกังวลเรื่องการว่างงานทำให้ตลาดหุ้นอ่อนตัว ส่วนทองคำได้แรงซื้อจากผู้ต้องการสินทรัพย์ปลอดภัย" } },
		/* InflationScare */
		{ { "เงินเฟ้อพุ่งเกินเป้า ทองคำได้แรงซื้อ", "ราคาสินค้าทะยานขึ้น นักลงทุนกังวลดอกเบี้ยขึ้น" },
		  { "ดัชนีราคาผู้บริโภคสูงกว่าที่ธนาคารกลางตั้งเป้าไว้ ทำให้นักลงทุนกลัวว่าดอกเบี้ยจะสูงขึ้นและหันมาถือทองคำป้องกันค่าเงินเสื่อม",
		    "ต้นทุนการดำเนินงานของธุรกิจเพิ่มขึ้นทั่วหน้า หุ้นถูกกดดัน ขณะที่ทองคำถูกมองเป็นเครื่องมือป้องกันเงินเฟ้อ" } },
		/* RateCutHope */
		{ { "ลือธนาคารกลางเตรียมลดดอกเบี้ย ตลาดคึกคัก", "ความหวังดอกเบี้ยขาลง ดันหุ้นและทองพร้อมกัน" },
		  { "แหล่งข่าววงในระบุว่าธนาคารกลางอาจผ่อนคลายนโยบายการเงินเร็วๆ นี้ ต้นทุนเงินกู้ที่ลดลงช่วยหนุนหุ้น และทำให้การถือทองคำน่าสนใจขึ้น",
		    "นักลงทุนเริ่มวางตำแหน่งรับดอกเบี้ยที่ต่ำลง ทั้งหุ้นและทองคำได้รับอานิสงส์" } },
		/* WarTension */
		{ { "ความตึงเครียดระหว่างประเทศปะทุ นักลงทุนหนีเข้าทอง", "ข่าวสู้รบกดตลาดหุ้น ทองคำพุ่งรับความเสี่ยง" },
		  { "สถานการณ์ความขัดแย้งที่ทวีความรุนแรงทำให้เส้นทางขนส่งหลายสายเสี่ยงหยุดชะงัก นักลงทุนเทขายหุ้นและซื้อทองคำเป็นสินทรัพย์ปลอดภัย",
		    "ตลาดการเงินผันผวนหนักหลังมีรายงานการปะทะ ผู้จัดการกองทุนปรับพอร์ตลดความเสี่ยง ทองคำเป็นผู้ได้ประโยชน์หลัก" } },
		/* PeaceDeal */
		{ { "ลงนามข้อตกลงสันติภาพ ตลาดหุ้นดีดตัว", "ความตึงเครียดคลี่คลาย ทองคำอ่อนตัวลง" },
		  { "ข้อตกลงที่ได้รับการลงนามช่วยลดความเสี่ยงด้านการค้าและการขนส่ง นักลงทุนกลับมาซื้อหุ้นและขายทองคำที่ถือไว้เพื่อความปลอดภัย",
		    "บรรยากาศการลงทุนกลับมาสดใส เส้นทางขนส่งที่เคยปิดกำลังจะเปิดอีกครั้ง" } },
		/* BankScare */
		{ { "ข่าวลือปัญหาสภาพคล่องธนาคาร ตลาดตื่นตระหนก", "ผวาวิกฤตสถาบันการเงิน หุ้นดิ่ง ทองพุ่ง" },
		  { "มีรายงานว่าสถาบันการเงินบางแห่งมีปัญหาสภาพคล่อง แม้ยังไม่ได้รับการยืนยัน แต่ก็ทำให้นักลงทุนเทขายหุ้นอย่างรวดเร็วและแห่ซื้อทองคำ",
		    "ความเชื่อมั่นต่อระบบการเงินสั่นคลอน ผู้ฝากเงินบางส่วนเริ่มถอนเงิน ตลาดหุ้นเผชิญแรงขายหนัก" } },
		/* GoldMineFound */
		{ { "พบแหล่งแร่ทองคำขนาดใหญ่ ราคาทองอ่อนตัว", "เหมืองทองแห่งใหม่เปิดสำรวจ อุปทานเพิ่มขึ้นมาก" },
		  { "ผลสำรวจชี้ว่ามีแหล่งแร่ทองคำที่ใหญ่กว่าที่เคยประเมินไว้ ตลาดคาดว่าปริมาณทองคำจะเพิ่มขึ้นในอนาคต กดดันราคาทองให้ปรับลง",
		    "นักวิเคราะห์ประเมินว่าอุปทานทองคำที่เพิ่มขึ้นจะทำให้ราคาปรับตัวลงในระยะสั้น" } },
		/* MineStrike */
		{ { "คนงานเหมืองทองประท้วงหยุดงาน ผลผลิตสะดุด", "เหมืองทองหยุดดำเนินงาน ราคาทองขยับขึ้น" },
		  { "การหยุดงานของคนงานเหมืองทำให้ผลผลิตทองคำลดลงชั่วคราว ตลาดกังวลว่าอุปทานจะตึงตัว ราคาจึงปรับขึ้น",
		    "ผู้ประกอบการเหมืองยังเจรจากับตัวแทนคนงานไม่สำเร็จ ผู้ค้าทองคำเร่งซื้อสำรองไว้ก่อน" } },
		/* CentralBanksBuyGold */
		{ { "ธนาคารกลางหลายแห่งเร่งสะสมทองคำ", "แรงซื้อทองระดับประเทศ ดันราคาทะยาน" },
		  { "รายงานระบุว่าธนาคารกลางหลายประเทศเพิ่มสัดส่วนทองคำในทุนสำรอง ทำให้ปริมาณทองในตลาดตึงตัวและราคาปรับขึ้น",
		    "ความต้องการทองคำจากภาครัฐเพิ่มขึ้นต่อเนื่อง นักลงทุนรายย่อยเร่งตามซื้อ" } },
		/* JewelleryDemand */
		{ { "ความต้องการทองรูปพรรณพุ่งรับเทศกาล", "ร้านทองคึกคัก ยอดซื้อสูงกว่าปีก่อน" },
		  { "ยอดขายเครื่องประดับทองคำเพิ่มขึ้นแรงช่วงเทศกาล ผู้ค้ารายใหญ่สั่งซื้อทองเพิ่มเพื่อเตรียมสินค้า ราคาทองจึงปรับขึ้น",
		    "ผู้บริโภคแห่ซื้อทองเป็นของขวัญและเก็บออม ราคาทองในตลาดได้แรงหนุน" } },
		/* FuelSpike */
		{ { "ราคาน้ำมันพุ่ง ต้นทุนขนส่งเพิ่มขึ้น", "น้ำมันแพงกดดันหุ้นกลุ่มขนส่ง" },
		  { "ราคาเชื้อเพลิงที่สูงขึ้นทำให้ต้นทุนการเดินรถ เรือ และเครื่องบินเพิ่มขึ้น นักลงทุนกังวลต่อกำไรของบริษัทขนส่ง ทองคำได้แรงซื้อเล็กน้อยจากความกังวลเรื่องเงินเฟ้อ",
		    "ผู้ประกอบการขนส่งเตรียมปรับขึ้นค่าระวางเพื่อชดเชยต้นทุน หุ้นจึงถูกเทขาย" } },
		/* FuelDrop */
		{ { "ราคาน้ำมันร่วง ต้นทุนขนส่งลดลง", "น้ำมันถูกลง หนุนหุ้นกลุ่มขนส่ง" },
		  { "ราคาเชื้อเพลิงที่ลดลงช่วยลดต้นทุนการเดินรถ นักลงทุนคาดว่ากำไรของบริษัทขนส่งจะดีขึ้น",
		    "ต้นทุนที่ถูกลงทำให้ผู้ประกอบการมีกำไรต่อเที่ยวเพิ่มขึ้น หุ้นปรับตัวขึ้น" } },
		/* TechBreakthrough */
		{ { "เทคโนโลยีขนส่งใหม่ ช่วยประหยัดพลังงานได้มาก", "นวัตกรรมระบบควบคุมการเดินรถ ดึงเงินลงทุนไหลเข้า" },
		  { "การพัฒนาเทคโนโลยีใหม่ในอุตสาหกรรมขนส่งช่วยลดต้นทุนและเพิ่มประสิทธิภาพ นักลงทุนมองว่าจะส่งผลดีต่อกำไรของทุกบริษัทในระยะยาว",
		    "ผู้เชี่ยวชาญระบุว่านวัตกรรมนี้จะทำให้การขนส่งเร็วขึ้นและปลอดภัยขึ้น หุ้นได้รับแรงซื้อ" } },
		/* TourismBoom */
		{ { "นักท่องเที่ยวทะลัก ผู้โดยสารเพิ่มสูงสุดในรอบปี", "ท่องเที่ยวบูม ธุรกิจขนส่งผู้โดยสารคึกคัก" },
		  { "จำนวนนักท่องเที่ยวที่เพิ่มขึ้นทำให้ความต้องการเดินทางสูงขึ้น บริษัทขนส่งผู้โดยสารคาดว่ารายได้จะเติบโต",
		    "โรงแรมและสายการเดินทางเต็มทุกเที่ยว นักลงทุนจึงเข้าซื้อหุ้นขนส่ง" } },
		/* StormDisruption */
		{ { "พายุใหญ่ถล่ม เส้นทางขนส่งหยุดชะงัก", "อากาศแปรปรวนรุนแรง เที่ยวบินและขบวนรถล่าช้า" },
		  { "พายุที่รุนแรงทำให้เส้นทางขนส่งหลายสายต้องปิดชั่วคราว ธุรกิจเสียโอกาสทำรายได้ นักลงทุนจึงลดน้ำหนักหุ้นขนส่ง",
		    "ความเสียหายต่อโครงสร้างพื้นฐานทำให้ต้นทุนซ่อมแซมเพิ่มขึ้น ตลาดหุ้นอ่อนตัวลงชั่วคราว" } },
		/* CompanyContract */
		{ { "{C} คว้าสัญญาขนส่งรายใหญ่", "{C} ได้ลูกค้ารายใหม่ รายได้มีแนวโน้มเพิ่ม" },
		  { "{C} ประกาศชนะการประมูลสัญญาขนส่งระยะยาว นักวิเคราะห์มองว่าจะช่วยเพิ่มรายได้อย่างมั่นคง นักลงทุนจึงเข้าซื้อหุ้น",
		    "ลูกค้ารายใหญ่เลือกใช้บริการของ {C} ความเชื่อมั่นของผู้ถือหุ้นดีขึ้นทันที" } },
		/* CompanyAccident */
		{ { "อุบัติเหตุกับขบวนของ {C} หุ้นร่วง", "{C} หยุดให้บริการบางเส้นทางหลังเกิดอุบัติเหตุ" },
		  { "เกิดอุบัติเหตุกับยานพาหนะของ {C} ทำให้ต้องหยุดให้บริการบางเส้นทางเพื่อตรวจสอบ นักลงทุนกังวลเรื่องค่าใช้จ่ายและชื่อเสียงของบริษัท",
		    "ค่าชดเชยและการซ่อมแซมอาจกระทบกำไรของ {C} ผู้ถือหุ้นบางส่วนเร่งขายออก" } },
		/* CompanyScandal */
		{ { "ข่าวฉาว {C} ถูกตรวจสอบบัญชี", "นักลงทุนเทขายหุ้น {C} หลังมีข่าวทุจริต" },
		  { "มีรายงานว่าหน่วยงานกำกับกำลังตรวจสอบบัญชีของ {C} ความเชื่อมั่นของนักลงทุนลดลงอย่างรวดเร็ว",
		    "ข้อกล่าวหาเรื่องการจัดการงบการเงินของ {C} ทำให้หุ้นถูกเทขายหนัก ทางบริษัทยังไม่ออกมาชี้แจง" } },
		/* CompanyProfitRumor */
		{ { "ลือ {C} กำไรทำสถิติใหม่", "แหล่งข่าวเผย {C} ผลประกอบการเกินคาด" },
		  { "แหล่งข่าวในวงการระบุว่า {C} มีแนวโน้มประกาศกำไรสูงกว่าที่ตลาดคาดไว้ ทำให้มีแรงซื้อเข้ามาล่วงหน้า",
		    "นักวิเคราะห์หลายรายปรับเพิ่มคำแนะนำซื้อหุ้น {C} หลังเห็นปริมาณการขนส่งที่เพิ่มขึ้น" } },
	};
	const size_t i = static_cast<size_t>(kind);
	return table[i < MARKET_EVENT_KIND_COUNT ? i : 0];
}

/** Replace every {C} in a text. */
inline std::string MarketReplaceCompany(std::string text, const std::string &company)
{
	for (size_t pos = text.find("{C}"); pos != std::string::npos; pos = text.find("{C}", pos + company.size())) {
		text.replace(pos, 3, company);
	}
	return text;
}

/** A push in ppm as a signed percentage with two decimals, for example -0.50%. Done by hand so that a value between -1% and 0 keeps its minus sign. */
inline std::string MarketPpmText(int64_t ppm)
{
	const int64_t a = ppm < 0 ? -ppm : ppm;
	const int64_t hundredths = a / 100; // 1 hundredth of a percent = 100 ppm
	std::string text = ppm < 0 ? "-" : "+";
	text += std::to_string(hundredths / 100);
	text += ".";
	if (hundredths % 100 < 10) text += "0";
	text += std::to_string(hundredths % 100);
	text += "%";
	return text;
}

/** The three presenters. The story of an event always has the same one. */
inline const char *MarketReporterName(uint8_t variant)
{
	static const char *const names[3] = { "AI ผู้ประกาศข่าว นภา", "AI ผู้ประกาศข่าว ธีร์", "AI ผู้ประกาศข่าว มินตรา" };
	return names[(variant / 4) % 3];
}

/** The size of the news in words. */
inline const char *MarketImpactText(int severity)
{
	return severity >= 3 ? "ข่าวใหญ่ ผลกระทบรุนแรง" : (severity == 2 ? "ผลกระทบปานกลาง" : "ผลกระทบเล็กน้อย");
}

/**
 * Write the story of an event.
 * @param e The event.
 * @param company The name of the company of a company event (ignored for a world event).
 */
inline MarketStory ComposeMarketStory(const MarketEvent &e, const std::string &company)
{
	const MarketStoryTexts &t = MarketStoryTextsOf(e.kind);
	MarketStory s;
	s.headline = MarketReplaceCompany(t.headline[e.variant & 1], company);
	s.body = MarketReplaceCompany(t.body[(e.variant >> 1) & 1], company);
	s.reporter = MarketReporterName(e.variant);
	s.impact = MarketImpactText(e.severity);

	/* The analyst line is built from the numbers, so it is always true to what the event did. */
	std::string a;
	if (e.gold_ppm != 0) {
		a += std::string("ทองคำ: แรงกดดันราคา ") + MarketPpmText(e.gold_ppm) + (e.gold_ppm > 0 ? " (มีแนวโน้มปรับขึ้น)" : " (มีแนวโน้มปรับลง)");
	}
	if (e.share_ppm != 0) {
		if (!a.empty()) a += "  ";
		if (e.target != MARKET_NO_TARGET) {
			a += std::string("หุ้น ") + company + ": " + MarketPpmText(e.share_ppm) + (e.share_ppm > 0 ? " (มีแนวโน้มปรับขึ้น)" : " (มีแนวโน้มปรับลง)");
		} else {
			a += std::string("หุ้นทุกบริษัท: ") + MarketPpmText(e.share_ppm) + (e.share_ppm > 0 ? " (มีแนวโน้มปรับขึ้น)" : " (มีแนวโน้มปรับลง)");
		}
	}
	if (a.empty()) a = "ยังไม่มีผลต่อราคา";
	s.analysis = a;
	return s;
}

#endif /* MARKET_NEWS_AI_H */
