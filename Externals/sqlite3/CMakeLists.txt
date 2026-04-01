add_library(sqlite3 STATIC sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Suppress warnings in third-party code
if(NOT MSVC)
  target_compile_options(sqlite3 PRIVATE -w)
endif()
