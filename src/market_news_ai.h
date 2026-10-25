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
 * COMPANY stories are written from REAL figures (MarketEvent::fact_ppm printed as {P}); see CompanyEventFact in market_events.h.
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
		/* CompanyRevenueUp ({P} = real change of the revenue) */
		{ { "{C} รายได้ไตรมาสล่าสุดเพิ่มขึ้น {P}", "งบ {C} รายได้โต {P} จากไตรมาสก่อน" },
		  { "ตามงบการเงินไตรมาสล่าสุด รายได้ของ {C} เพิ่มขึ้น {P} เมื่อเทียบกับไตรมาสก่อนหน้า นักลงทุนมองว่าธุรกิจขนส่งของบริษัทเติบโตจริงและเริ่มเข้าซื้อหุ้น",
		    "รายได้จากการขนส่งของ {C} สูงกว่าไตรมาสก่อน {P} ตัวเลขนี้มาจากงบจริงของบริษัท จึงได้รับแรงซื้อจากนักลงทุน" } },
		/* CompanyProfitDown */
		{ { "กำไรไตรมาสล่าสุดของ {C} ลดลง {P}", "{C} กำไรหดตัว {P} นักลงทุนเริ่มระวัง" },
		  { "งบการเงินล่าสุดชี้ว่ากำไรสุทธิของ {C} ลดลง {P} จากไตรมาสก่อน แม้ยังเป็นกำไรอยู่ แต่นักลงทุนกังวลว่าต้นทุนจะกดดันผลประกอบการต่อไป",
		    "ผลประกอบการของ {C} อ่อนลง กำไรสุทธิลดลง {P} เทียบกับไตรมาสก่อน ผู้ถือหุ้นบางส่วนจึงขายออก" } },
		/* CompanyDistress ({P} = debt as a share of the company value) */
		{ { "{C} มีหนี้ธนาคารกลางสูง {P} ของมูลค่าบริษัท", "ภาระหนี้ของ {C} น่ากังวล นักลงทุนลดความเสี่ยง" },
		  { "{C} มีหนี้กับธนาคารกลางคิดเป็น {P} ของมูลค่าบริษัท ยิ่งหนี้สูงความเสี่ยงที่จะผิดนัดชำระยิ่งมาก นักลงทุนจึงเทขายหุ้น",
		    "ธนาคารกลางรายงานว่าหนี้ของ {C} สูงถึง {P} ของมูลค่าบริษัท หากชำระดอกเบี้ยไม่ไหวอาจถูกปรับและถูกยึดทรัพย์" } },
		/* CompanyProfitUp (without a number the story is 'back to a profit') */
		{ { "กำไรไตรมาสล่าสุดของ {C} เพิ่มขึ้น {P}", "{C} ผลงานเยี่ยม กำไรโต {P}" },
		  { "งบการเงินล่าสุดแสดงว่ากำไรสุทธิของ {C} เพิ่มขึ้น {P} จากไตรมาสก่อน นักวิเคราะห์ปรับมุมมองเป็นบวก และมีแรงซื้อหุ้นเข้ามา",
		    "ผลประกอบการไตรมาสล่าสุดของ {C} ดีขึ้นอย่างชัดเจน กำไรสุทธิเพิ่ม {P} เทียบกับไตรมาสก่อน" } },
		/* CompanyRevenueDown */
		{ { "รายได้ไตรมาสล่าสุดของ {C} ลดลง {P}", "{C} รายได้หด {P} จากไตรมาสก่อน" },
		  { "งบการเงินล่าสุดระบุว่ารายได้ของ {C} ลดลง {P} เมื่อเทียบกับไตรมาสก่อนหน้า นักลงทุนกังวลว่าปริมาณงานขนส่งลดลง",
		    "ตัวเลขรายได้ของ {C} อ่อนตัวลง {P} ผู้ถือหุ้นบางส่วนจึงขายหุ้นออก" } },
		/* CompanyLoss ({P} = loss as a share of the revenue) */
		{ { "{C} ขาดทุนไตรมาสล่าสุด คิดเป็น {P} ของรายได้", "งบ {C} ออกมาขาดทุน นักลงทุนผิดหวัง" },
		  { "ไตรมาสล่าสุด {C} มีรายจ่ายมากกว่ารายได้ ขาดทุนสุทธิเท่ากับ {P} ของรายได้ ผู้ถือหุ้นกังวลเรื่องกระแสเงินสดและเทขายหุ้น",
		    "ผลประกอบการไตรมาสล่าสุดของ {C} อยู่ในแดนลบ ขาดทุนสุทธิคิดเป็น {P} ของรายได้ ทำให้ราคาหุ้นถูกกดดัน" } },
		/* CompanyValueUp */
		{ { "มูลค่าบริษัท {C} เพิ่มขึ้น {P}", "สินทรัพย์ของ {C} เติบโต {P} จากไตรมาสก่อน" },
		  { "มูลค่ากิจการของ {C} ณ สิ้นไตรมาสล่าสุดสูงกว่าไตรมาสก่อนหน้า {P} นักลงทุนจึงให้ราคาหุ้นสูงขึ้น",
		    "{C} มีมูลค่าบริษัทเพิ่มขึ้น {P} ตามงบล่าสุด ซึ่งช่วยหนุนความเชื่อมั่นของผู้ถือหุ้น" } },
		/* CompanyValueDown */
		{ { "มูลค่าบริษัท {C} ลดลง {P}", "สินทรัพย์ของ {C} หดตัว {P} จากไตรมาสก่อน" },
		  { "มูลค่ากิจการของ {C} ณ สิ้นไตรมาสล่าสุดต่ำกว่าไตรมาสก่อนหน้า {P} นักลงทุนจึงลดน้ำหนักหุ้นตัวนี้",
		    "{C} มีมูลค่าบริษัทลดลง {P} ตามงบล่าสุด ผู้ถือหุ้นบางส่วนเร่งขายออก" } },
		/* CompanyBankruptcy ({P} = months of bankruptcy) */
		{ { "{C} เข้าสู่ภาวะล้มละลาย", "{C} ล้มละลาย เดือนที่ {P} ของกระบวนการ" },
		  { "{C} อยู่ในภาวะล้มละลายแล้ว {P} เดือน หากยังฟื้นไม่ได้ ธนาคารกลางอาจยึดทรัพย์และเปิดประมูล หุ้นถูกเทขายอย่างหนัก",
		    "ผู้ถือหุ้นของ {C} กังวลอย่างหนักเมื่อบริษัทเข้าสู่ภาวะล้มละลาย ({P} เดือน) ราคาหุ้นดิ่งลง" } },
		/* RateHike */
		{ { "ธนาคารกลางขึ้นดอกเบี้ย ตลาดหุ้นและทองถูกเทขาย", "ดอกเบี้ยขาขึ้น กดดันสินทรัพย์เสี่ยง" },
		  { "การขึ้นดอกเบี้ยทำให้ต้นทุนเงินกู้สูงขึ้น นักลงทุนลดการถือหุ้น และการถือทองคำซึ่งไม่มีดอกเบี้ยน่าสนใจน้อยลง",
		    "ผลตอบแทนเงินฝากที่ดีขึ้นดึงเงินออกจากหุ้นและทองคำ ราคาปรับลงทั้งสองตลาด" } },
		/* CentralBanksSellGold */
		{ { "ธนาคารกลางบางแห่งทยอยขายทองคำสำรอง", "ทองคำถูกเทขายจากทุนสำรอง ราคาอ่อนตัว" },
		  { "รายงานระบุว่าธนาคารกลางบางประเทศลดสัดส่วนทองคำในทุนสำรอง ทำให้อุปทานในตลาดเพิ่มขึ้นและราคาปรับลง",
		    "ปริมาณทองคำที่ออกสู่ตลาดเพิ่มขึ้นจากการขายของภาครัฐ นักลงทุนจึงชะลอการซื้อ" } },
		/* JewelleryDemandDrop */
		{ { "ความต้องการทองรูปพรรณซบเซาหลังเทศกาล", "ร้านทองเงียบ ยอดซื้อต่ำกว่าปกติ" },
		  { "หลังช่วงเทศกาลผ่านไป ยอดขายเครื่องประดับทองคำลดลง ผู้ค้าลดการสั่งซื้อ ราคาทองจึงอ่อนตัว",
		    "ผู้บริโภคชะลอการซื้อทองหลังใช้จ่ายไปมากในช่วงเทศกาล ความต้องการทองในตลาดลดลง" } },
		/* ProfitTaking */
		{ { "นักลงทุนเทขายทำกำไรทองคำ", "แรงขายทำกำไรกดราคาทอง" },
		  { "หลังราคาทองคำปรับขึ้นมาระยะหนึ่ง นักลงทุนบางส่วนขายทำกำไร ทำให้ราคาย่อตัวลง",
		    "ผู้ที่ซื้อทองไว้ก่อนหน้าทยอยขายล็อกกำไร ราคาทองจึงอ่อนตัวลงในระยะสั้น" } },
		/* CoalShortage */
		{ { "ถ่านหินขาดแคลน ราคาค่าขนส่งพุ่ง", "โรงไฟฟ้าแย่งซื้อถ่านหิน ตลาดตึงตัว" },
		  { "ผลผลิตถ่านหินที่ลดลงทำให้โรงไฟฟ้าและโรงงานต้องแย่งกันซื้อ ผู้ประกอบการขนส่งถ่านหินจึงได้ราคาต่อหน่วยสูงขึ้นในช่วงนี้",
		    "คลังถ่านหินของผู้ใช้รายใหญ่เหลือน้อย ความต้องการขนส่งถ่านหินเร่งด่วนดันค่าขนส่งให้สูงขึ้น" } },
		/* CoalGlut */
		{ { "ถ่านหินล้นตลาด ราคาตกต่ำ", "เหมืองถ่านหินผลิตเกินความต้องการ" },
		  { "ปริมาณถ่านหินที่ออกสู่ตลาดมากกว่าที่โรงไฟฟ้าต้องการ ทำให้ราคาต่อหน่วยที่ผู้ประกอบการขนส่งได้รับปรับลดลง",
		    "คลังถ่านหินเต็มทั่วประเทศ ผู้ซื้อต่อรองราคาลง รายได้จากการขนส่งถ่านหินจึงลดลงชั่วคราว" } },
		/* HarvestFailure */
		{ { "ผลผลิตเกษตรเสียหาย อาหารขาดตลาด", "ภัยแล้งกระทบพืชผล ราคาสินค้าเกษตรพุ่ง" },
		  { "ผลผลิตทางการเกษตรลดลงมากกว่าที่คาด ผู้ซื้อต้องจ่ายแพงขึ้นเพื่อให้ได้สินค้า ค่าขนส่งสินค้าเกษตรและอาหารจึงปรับสูงขึ้น",
		    "โรงงานแปรรูปอาหารขาดวัตถุดิบ แย่งซื้อสินค้าเกษตรที่เหลืออยู่ ราคาต่อหน่วยที่ผู้ขนส่งได้รับเพิ่มขึ้น" } },
		/* BumperHarvest */
		{ { "ผลผลิตเกษตรล้นตลาด ราคาตก", "ปีนี้พืชผลดีเกินคาด ราคาสินค้าเกษตรอ่อนตัว" },
		  { "ผลผลิตทางการเกษตรที่มากเกินความต้องการทำให้ราคาสินค้าเกษตรและอาหารปรับลดลง ผู้ประกอบการขนส่งได้ราคาต่อหน่วยต่ำลงชั่วคราว",
		    "โกดังและโรงงานแปรรูปรับของไม่ทัน ผู้ซื้อกดราคาลง ค่าขนส่งสินค้าเกษตรจึงลดลง" } },
		/* ConstructionBoom */
		{ { "ก่อสร้างบูม ไม้ เหล็ก และแร่ราคาพุ่ง", "โครงการก่อสร้างทั่วประเทศ ดันความต้องการวัสดุ" },
		  { "โครงการก่อสร้างขนาดใหญ่ทำให้ความต้องการไม้ เหล็ก และแร่เพิ่มขึ้นมาก ผู้ประกอบการที่ขนส่งวัสดุเหล่านี้ได้ราคาต่อหน่วยสูงขึ้น",
		    "ผู้รับเหมาเร่งสั่งวัสดุก่อสร้างเพื่อให้ทันกำหนด ตลาดวัสดุตึงตัวและค่าขนส่งปรับขึ้น" } },
		/* ConstructionSlump */
		{ { "ก่อสร้างซบเซา ความต้องการวัสดุหดตัว", "โครงการก่อสร้างถูกเลื่อน ไม้ เหล็ก และแร่ราคาอ่อน" },
		  { "การก่อสร้างที่ชะลอตัวทำให้ความต้องการไม้ เหล็ก และแร่ลดลง ผู้ซื้อลดการสั่งซื้อ ราคาต่อหน่วยที่ผู้ขนส่งได้รับจึงปรับลด",
		    "ผู้รับเหมาหลายรายเลื่อนโครงการออกไป สต็อกวัสดุก่อสร้างเหลือมาก ค่าขนส่งวัสดุจึงอ่อนตัว" } },
		/* DiamondMineFound */
		{ { "พบเหมืองเพชรแห่งใหม่ ราคาเพชรอ่อนตัว", "เพชรจากเหมืองใหม่เข้าตลาด อุปทานเพิ่มขึ้น" },
		  { "การค้นพบเหมืองเพชรแห่งใหม่ทำให้ปริมาณเพชรในตลาดเพิ่มขึ้นทันที นักวิเคราะห์คาดว่าราคาเพชรจะปรับลงตามปริมาณที่มากขึ้น",
		    "เพชรจำนวนมากถูกนำออกมาขาย สต็อกเพชรของตลาดเพิ่มขึ้น กดดันราคาต่อกะรัต" } },
		/* DiamondDemandSurge */
		{ { "ความต้องการเพชรพุ่ง สต็อกในตลาดลดฮวบ", "นักลงทุนและร้านเพชรแย่งซื้อ ราคาเพชรขยับขึ้น" },
		  { "ผู้ค้าและนักลงทุนเร่งซื้อเพชรจนสต็อกในตลาดลดลงอย่างรวดเร็ว เมื่อเพชรในตลาดเหลือน้อยลง ราคาต่อกะรัตจึงปรับสูงขึ้น",
		    "ความต้องการเพชรเจียระไนเพิ่มขึ้นแรงช่วงนี้ ผู้ค้าเร่งซื้อสำรอง ปริมาณเพชรในตลาดเหลือน้อยลง" } },
		/* TownBoom ({C} = the town) */
		{ { "{C} เศรษฐกิจคึกคัก เมืองเติบโตเร็วขึ้น", "นักลงทุนแห่เข้า {C} เมืองขยายตัวเร็ว" },
		  { "เศรษฐกิจของ {C} ขยายตัวต่อเนื่อง ธุรกิจและผู้อยู่อาศัยใหม่ย้ายเข้ามา ทำให้เมืองเติบโตเร็วกว่าปกติในช่วงนี้",
		    "การลงทุนใน {C} เพิ่มขึ้นแรง มีการสร้างบ้านและร้านค้าเพิ่ม เมืองจึงขยายตัวเร็วขึ้นชั่วคราว" } },
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

/** A number in ppm as an unsigned percentage with one decimal, for example 12.3%. */
inline std::string MarketPercentText(int64_t ppm)
{
	const int64_t a = ppm < 0 ? -ppm : ppm;
	const int64_t tenths = a / 1'000; // 1 tenth of a percent = 1,000 ppm
	return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "%";
}

/** The real number of a company story as printed: months for a bankruptcy, otherwise a percentage; "" when not known. */
inline std::string MarketFactText(MarketEventKind kind, int64_t fact_ppm)
{
	if (fact_ppm == 0 || !MarketKindIsCompany(kind)) return std::string();
	if (kind == MarketEventKind::CompanyBankruptcy) return std::to_string(fact_ppm);
	return MarketPercentText(fact_ppm);
}

/** Replace every {P}. An unknown number takes its space with it. */
inline std::string MarketReplaceFact(std::string text, const std::string &fact)
{
	for (size_t pos = text.find("{P}"); pos != std::string::npos; pos = text.find("{P}", pos)) {
		size_t from = pos, len = 3;
		if (fact.empty() && from > 0 && text[from - 1] == ' ') { from--; len++; }
		text.replace(from, len, fact);
		pos = from + fact.size();
	}
	return text;
}

/** The real figure as one line of the analyst text of a company story. */
inline std::string MarketFactLine(const MarketEvent &e)
{
	if (!MarketKindIsCompany(e.kind)) return std::string();
	const std::string p = MarketFactText(e.kind, e.fact_ppm);
	const std::string signed_p = p.empty() ? std::string("-") : std::string(e.fact_ppm > 0 ? "+" : "-") + p;
	switch (e.kind) {
		case MarketEventKind::CompanyRevenueUp:
		case MarketEventKind::CompanyRevenueDown: return "ข้อมูลจริงจากงบ: รายได้ไตรมาสล่าสุดเทียบไตรมาสก่อน " + signed_p;
		case MarketEventKind::CompanyProfitUp:
			if (e.fact_ppm == 0) return "ข้อมูลจริงจากงบ: กลับมามีกำไรสุทธิ";
			return "ข้อมูลจริงจากงบ: กำไรสุทธิเทียบไตรมาสก่อน " + signed_p;
		case MarketEventKind::CompanyProfitDown: return "ข้อมูลจริงจากงบ: กำไรสุทธิเทียบไตรมาสก่อน " + signed_p;
		case MarketEventKind::CompanyLoss: return "ข้อมูลจริงจากงบ: ขาดทุนสุทธิ " + (p.empty() ? std::string("-") : p) + " ของรายได้";
		case MarketEventKind::CompanyValueUp:
		case MarketEventKind::CompanyValueDown: return "ข้อมูลจริงจากงบ: มูลค่าบริษัทเทียบไตรมาสก่อน " + signed_p;
		case MarketEventKind::CompanyDistress: return "ข้อมูลจริง: หนี้ธนาคารกลาง " + (p.empty() ? std::string("-") : p) + " ของมูลค่าบริษัท";
		case MarketEventKind::CompanyBankruptcy: return "ข้อมูลจริง: อยู่ในภาวะล้มละลาย " + (p.empty() ? std::string("-") : p) + " เดือน";
		default: return std::string();
	}
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
	const std::string fact = MarketFactText(e.kind, e.fact_ppm);
	if (e.kind == MarketEventKind::CompanyProfitUp && e.fact_ppm == 0) {
		static const char *const heads[2] = { "{C} พลิกกลับมามีกำไรแล้ว", "{C} พ้นจากภาวะขาดทุน" };
		static const char *const bodies[2] = {
			"งบไตรมาสล่าสุดของ {C} เป็นกำไรสุทธิ หลังจากไตรมาสก่อนหน้าที่ขาดทุนหรือยังไม่มีกำไร นักลงทุนมองเป็นสัญญาณบวก",
			"{C} รายงานกำไรสุทธิในไตรมาสล่าสุด ต่างจากไตรมาสก่อนที่ยังไม่มีกำไร ความเชื่อมั่นของผู้ถือหุ้นดีขึ้น" };
		s.headline = MarketReplaceCompany(heads[e.variant & 1], company);
		s.body = MarketReplaceCompany(bodies[(e.variant >> 1) & 1], company);
	} else {
		s.headline = MarketReplaceFact(MarketReplaceCompany(t.headline[e.variant & 1], company), fact);
		s.body = MarketReplaceFact(MarketReplaceCompany(t.body[(e.variant >> 1) & 1], company), fact);
	}
	s.reporter = MarketReporterName(e.variant);
	s.impact = MarketImpactText(e.severity);

	std::string a = MarketFactLine(e);
	if (e.gold_ppm != 0) {
		if (!a.empty()) a += "  ";
		a += std::string("ทองคำ: แรงกดดันราคา ") + MarketPpmText(e.gold_ppm) + (e.gold_ppm > 0 ? " (มีแนวโน้มปรับขึ้น)" : " (มีแนวโน้มปรับลง)");
	}
	if (e.share_ppm != 0) {
		if (!a.empty()) a += "  ";
		if (e.target != MARKET_NO_TARGET) a += std::string("หุ้น ") + company + ": " + MarketPpmText(e.share_ppm) + (e.share_ppm > 0 ? " (มีแนวโน้มปรับขึ้น)" : " (มีแนวโน้มปรับลง)");
		else a += std::string("หุ้นทุกบริษัท: ") + MarketPpmText(e.share_ppm) + (e.share_ppm > 0 ? " (มีแนวโน้มปรับขึ้น)" : " (มีแนวโน้มปรับลง)");
	}
	/* Cargo prices, diamonds, a town, the cost of credit: worked out from the kind and the severity (the same numbers that the game uses). */
	auto add = [&a](const std::string &text) { if (!a.empty()) a += "  "; a += text; };
	const MarketCargoEffect cargo = MarketCargoEffectOf(e.kind, e.severity);
	if (cargo.group != MarketCargoGroup::None) {
		add(std::string("ราคาค่าขนส่ง") + MarketCargoGroupName(cargo.group) + ": " + MarketPpmText(cargo.push_ppm) + (cargo.push_ppm > 0 ? " (มีแนวโน้มปรับขึ้น)" : " (มีแนวโน้มปรับลง)") + " นาน " + std::to_string(MARKET_CARGO_NEWS_DAYS) + " วัน");
	}
	const int64_t diamonds = MarketDiamondStockPpm(e.kind, e.severity);
	if (diamonds != 0) {
		add(std::string("สต็อกเพชรในตลาด ") + MarketPpmText(diamonds) + (diamonds > 0 ? " ของสต็อกเริ่มต้น (ราคาเพชรมีแนวโน้มลง)" : " ของสต็อกเริ่มต้น (ราคาเพชรมีแนวโน้มขึ้น)"));
	}
	const int64_t town_boost = MarketTownBoostPpm(e.kind, e.severity);
	if (town_boost != 0) {
		add(std::string("เมือง ") + company + ": โตเร็วขึ้น " + MarketPpmText(town_boost) + " แล้วค่อยๆ ลดลงใน " + std::to_string(MARKET_TOWN_NEWS_DAYS) + " วัน");
	}
	bool credit_world = false;
	const int64_t credit_bp = MarketCreditBp(e.kind, e.severity, credit_world);
	if (credit_bp != 0) {
		const int64_t a_bp = credit_bp < 0 ? -credit_bp : credit_bp;
		std::string pct = std::to_string(a_bp / 100) + "." + (a_bp % 100 < 10 ? "0" : "") + std::to_string(a_bp % 100) + "%";
		add(std::string("ดอกเบี้ยเงินกู้") + (credit_world ? "ของทุกบริษัท" : std::string(" ") + company) + ": " + (credit_bp > 0 ? "+" : "-") + pct + " (มีแนวโน้ม" + (credit_bp > 0 ? "แพงขึ้น" : "ถูกลง") + " ค่อยๆ ลดลงใน " + std::to_string(MARKET_CREDIT_NEWS_DAYS) + " วัน)");
	}
	if (a.empty()) a = "ยังไม่มีผลต่อราคา";
	s.analysis = a;
	return s;
}

#endif /* MARKET_NEWS_AI_H */
