cmake_minimum_required(VERSION 3.20)
project(FengDsymutil LANGUAGES C CXX)

find_package(LLVM 23.1.3 EXACT REQUIRED CONFIG)
if(NOT DSYMUTIL_SOURCE_ROOT OR NOT DSYMUTIL_PATCH_REVISION)
  message(FATAL_ERROR "Source root and patch revision are required")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
if(NOT LLVM_ENABLE_EH)
  add_compile_options(-fno-exceptions)
endif()
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

# Reuse the SDK's static dependency closure without inheriting shared LLVM.
if(NOT TARGET zstd::libzstd_static)
  message(FATAL_ERROR "The SDK must provide static zstd")
endif()
get_target_property(support_libraries LLVMSupport INTERFACE_LINK_LIBRARIES)
list(TRANSFORM support_libraries REPLACE "^zstd::libzstd_shared$" "zstd::libzstd_static")
set_property(TARGET LLVMSupport PROPERTY INTERFACE_LINK_LIBRARIES "${support_libraries}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/zstd-library.txt"
  CONTENT "$<TARGET_FILE:zstd::libzstd_static>\n")

# Only the patched component is rebuilt; never link its unpatched SDK archive.
set(parallel "${DSYMUTIL_SOURCE_ROOT}/lib/DWARFLinker/Parallel")
add_library(FengDWARFLinkerParallel STATIC
  ${parallel}/AcceleratorRecordsSaver.cpp
  ${parallel}/DependencyTracker.cpp
  ${parallel}/DIEAttributeCloner.cpp
  ${parallel}/DWARFEmitterImpl.cpp
  ${parallel}/DWARFLinker.cpp
  ${parallel}/DWARFLinkerCompileUnit.cpp
  ${parallel}/DWARFLinkerTypeUnit.cpp
  ${parallel}/DWARFLinkerImpl.cpp
  ${parallel}/DWARFLinkerUnit.cpp
  ${parallel}/OutputSections.cpp
  ${parallel}/SyntheticTypeNameBuilder.cpp)
target_include_directories(FengDWARFLinkerParallel PRIVATE "${parallel}")

# Generate the upstream command-line table using the matching SDK TableGen.
set(driver "${DSYMUTIL_SOURCE_ROOT}/tools/dsymutil")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Options.inc"
  COMMAND "${LLVM_TOOLS_BINARY_DIR}/llvm-tblgen" -I "${LLVM_INCLUDE_DIRS}"
    -gen-opt-parser-defs "${driver}/Options.td"
    -o "${CMAKE_CURRENT_BINARY_DIR}/Options.inc"
  DEPENDS "${driver}/Options.td" VERBATIM)
add_executable(FengDsymutil main.cpp "${CMAKE_CURRENT_BINARY_DIR}/Options.inc"
  ${driver}/dsymutil.cpp
  ${driver}/BinaryHolder.cpp
  ${driver}/CFBundle.cpp
  ${driver}/DebugMap.cpp
  ${driver}/DwarfLinkerForBinary.cpp
  ${driver}/MachODebugMapParser.cpp
  ${driver}/MachOUtils.cpp
  ${driver}/Reproducer.cpp
  ${driver}/RelocationMap.cpp
  ${driver}/SwiftModule.cpp)
set_target_properties(FengDsymutil PROPERTIES OUTPUT_NAME dsymutil)
target_include_directories(FengDsymutil PRIVATE "${driver}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(FengDsymutil PRIVATE
  FENG_DSYMUTIL_PATCH_REVISION="${DSYMUTIL_PATCH_REVISION}")
llvm_map_components_to_libnames(llvm_libraries
  AllTargetsCodeGens AllTargetsDescs AllTargetsInfos AsmPrinter BitReader
  BitStreamReader CodeGen CodeGenTypes DWARFLinker DWARFLinkerClassic
  DebugInfoDWARF DebugInfoDWARFLowLevel MC Object Option Remarks Support
  Target TargetParser)
target_link_libraries(FengDsymutil PRIVATE FengDWARFLinkerParallel ${llvm_libraries}
  "-framework CoreFoundation")
