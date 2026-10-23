วิธีใช้: คัดลอกไฟล์ในโฟลเดอร์นี้ไปวางทับใน repo (โครงสร้างโฟลเดอร์เหมือนกัน) แล้ว push

  src/central_bank.h            ตรรกะดอกเบี้ย/โทษ + ฟิลด์ใหม่ใน BankAccount
  src/central_bank.cpp          หักดอกเบี้ยรายวัน, วงเงินที่ลดตามโทษ
  src/central_bank_gui.cpp      หน้า "ธนาคารกลาง": อัตราดอกเบี้ย, ข้อความผิดนัด/ระดับโทษ
  src/saveload/stock_sl.cpp     เซฟ/โหลดสถานะโทษ (record ชนิดใหม่ SRK_BANK_RISK = 23)
  apply_overlay.py              ตัวจับเงิน money_trace.log (แก้ให้ build ผ่านแล้ว; ปิดได้ด้วย MONEY_TRACE = False)

ข้อควรระวัง: ถ้าใน repo มีไฟล์ src/stock_sl.cpp (ไม่ใช่ในโฟลเดอร์ saveload) นั่นเป็นสำเนาเก่าที่ไม่ได้ใช้ ไม่ต้องแก้
