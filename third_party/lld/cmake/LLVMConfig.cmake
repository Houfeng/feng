# Use the installed SDK without inheriting its preference for a shared LLVM.
# LLD's standalone find_package loads this adapter; the SDK itself is untouched.
if(NOT TEST_LLD_LLVM_CMAKE_DIR)
  message(FATAL_ERROR "TEST_LLD_LLVM_CMAKE_DIR must select the complete LLVM SDK")
endif()
include("${TEST_LLD_LLVM_CMAKE_DIR}/LLVMConfig.cmake")
if(NOT LLVM_PACKAGE_VERSION STREQUAL "22.1.8")
  message(FATAL_ERROR "The test LLD requires LLVM 22.1.8")
endif()
set(LLVM_DIR "${TEST_LLD_LLVM_CMAKE_DIR}")
set(LLVM_LINK_LLVM_DYLIB OFF)
set(LLVM_USE_STATIC_ZSTD ON)

# Prefer the SDK dependency's exported static target when it is available.
# Keep the original dependency if absent; packaging verifies the final closure.
if(TARGET zstd::libzstd_static AND TARGET LLVMSupport)
  get_target_property(support_libraries LLVMSupport INTERFACE_LINK_LIBRARIES)
  list(TRANSFORM support_libraries REPLACE "^zstd::libzstd_shared$" "zstd::libzstd_static")
  set_property(TARGET LLVMSupport PROPERTY INTERFACE_LINK_LIBRARIES "${support_libraries}")
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/test-lld-zstd.txt" CONTENT "$<TARGET_FILE:zstd::libzstd_static>\n")
endif()
