cmake_minimum_required(VERSION 3.20)
project(fvs_core VERSION 6.0.0 LANGUAGES C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MARIADB REQUIRED IMPORTED_TARGET mariadb)
find_package(OpenSSL REQUIRED)

add_library(fvs_core SHARED src/core.c src/util.c src/internal.c src/telemetry.c src/payment_recovery.c)
target_include_directories(fvs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fvs_core PRIVATE PkgConfig::MARIADB OpenSSL::Crypto)
target_compile_definitions(fvs_core PRIVATE FVS_BUILD_DLL)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fvs_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Werror -fstack-protector-strong -fPIC)
  target_link_options(fvs_core PRIVATE -Wl,-z,relro,-z,now)
endif()

add_executable(fvs_core_tests tests/test_core.c)
target_link_libraries(fvs_core_tests PRIVATE fvs_core)
target_include_directories(fvs_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fvs_core_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Werror -fstack-protector-strong)
endif()

add_executable(fvs_migrate tools/migrate.c)
target_link_libraries(fvs_migrate PRIVATE PkgConfig::MARIADB OpenSSL::Crypto)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fvs_migrate PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Werror -fstack-protector-strong)
  target_link_options(fvs_migrate PRIVATE -Wl,-z,relro,-z,now)
endif()

enable_testing()
add_test(NAME core_units COMMAND fvs_core_tests)

install(TARGETS fvs_core fvs_migrate LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES include/fvs.h include/fvs_ffi.h DESTINATION include/fvs)
