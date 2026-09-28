# w2-062: bench data generator for the w2-041 / w2-045 / w2-052 QTest harnesses (replaces the Python
# docs/evidence/w2-041/tools/make_bench_db.py in this project's workflow). See main.cpp.
#   ..\make-bench-db.bat   (vcvars64 + configure + build + run, Release)
cmake_minimum_required(VERSION 3.21)
project(w2062_make_bench_db LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Sql)
qt_standard_project_setup()
qt_add_executable(make_bench_db main.cpp)
target_link_libraries(make_bench_db PRIVATE Qt6::Core Qt6::Sql)
set_target_properties(make_bench_db PROPERTIES WIN32_EXECUTABLE OFF)
