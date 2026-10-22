cmake_minimum_required(VERSION 3.10)
project(OpenTTD-Stock)

# เพิ่มซอร์สโค้ดเดิมและม็อดของคุณเข้าไป
add_executable(openttd
    src/game_time/game_time_driver.cpp
    src/game_time/game_time_manager.cpp
    src/game_time/sim_time.h
    src/game_time/sim_time_test.cpp
    src/saveload/stock_sl.cpp
    src/widgets/central_bank.cpp
    src/widgets/stock_base.cpp
    src/widgets/stock_cmd.cpp
    src/widgets/stock_gui.cpp
    src/widgets/stock_order_gui.cpp
    # (และไฟล์หลักอื่นๆ ของ OpenTTD ตามโครงสร้างที่คุณดึงมา)
)
