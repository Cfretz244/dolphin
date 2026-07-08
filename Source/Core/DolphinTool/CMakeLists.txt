add_executable(dolphin-tool
  ToolHeadlessPlatform.cpp
  AotCommand.cpp
  AotCommand.h
  AotCEmitter.cpp
  AotCEmitter.h
  CfgCommand.cpp
  CfgCommand.h
  DiffCommand.cpp
  DiffCommand.h
  PPCMemoryImage.h
  RarcReader.cpp
  RarcReader.h
  RelFile.cpp
  RelFile.h
  RelModules.cpp
  RelModules.h
  Yaz0.cpp
  Yaz0.h
  VertexLoaderCEmitter.cpp
  VertexLoaderCEmitter.h
  VtxAotCommand.cpp
  VtxAotCommand.h
  ExtractCommand.cpp
  ExtractCommand.h
  ConvertCommand.cpp
  ConvertCommand.h
  VerifyCommand.cpp
  VerifyCommand.h
  HeaderCommand.cpp
  HeaderCommand.h
  ToolMain.cpp
)

# The translate command emits a verbatim copy of the checked-in AOT runtime
# header into every generated aot-src tree. Embed the header's bytes at build
# time so the emitted copy can never drift from the file the runtime compiles.
set(AOT_RUNTIME_HEADER "${CMAKE_SOURCE_DIR}/Source/Core/Core/PowerPC/AOT/aot_runtime.h")
set(AOT_RUNTIME_HEADER_GEN "${CMAKE_CURRENT_BINARY_DIR}/AotRuntimeHeaderGen.cpp")
add_custom_command(
  OUTPUT "${AOT_RUNTIME_HEADER_GEN}"
  COMMAND ${CMAKE_COMMAND}
          -DINPUT=${AOT_RUNTIME_HEADER}
          -DOUTPUT=${AOT_RUNTIME_HEADER_GEN}
          -DVARIABLE=s_aot_runtime_header
          -P ${CMAKE_SOURCE_DIR}/CMake/StringifyHeader.cmake
  DEPENDS "${AOT_RUNTIME_HEADER}" "${CMAKE_SOURCE_DIR}/CMake/StringifyHeader.cmake"
  COMMENT "Embedding aot_runtime.h into dolphin-tool"
)
target_sources(dolphin-tool PRIVATE "${AOT_RUNTIME_HEADER_GEN}")

if (WIN32)
  target_sources(dolphin-tool PRIVATE DolphinTool.exe.manifest DolphinTool.rc)

  set_target_properties(dolphin-tool PROPERTIES
    DEBUG_POSTFIX    D
    OUTPUT_NAME      DolphinTool
  )
endif()

target_link_libraries(dolphin-tool
PRIVATE
  core
  $<$<TARGET_EXISTS:dolphin_aot_games>:dolphin_aot_games>
  discio
  uicommon
  cpp-optparse
  fmt::fmt
  sqlite3
)

if(MSVC)
  # Add precompiled header
  target_link_libraries(dolphin-tool PRIVATE use_pch)
endif()

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} dolphin-tool)
install(TARGETS dolphin-tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
