set(libc_bridge_archive "${CMAKE_CURRENT_BINARY_DIR}/stubs/libSceLibcBridge_stub.a")
add_custom_command(OUTPUT "${libc_bridge_archive}"
  COMMAND ${VITA_LIBS_GEN} "${CMAKE_CURRENT_SOURCE_DIR}/nids.yml" "${CMAKE_CURRENT_BINARY_DIR}/stubs"
  COMMAND make -C "${CMAKE_CURRENT_BINARY_DIR}/stubs"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/nids.yml"
  COMMENT "Building native libc bridge imports"
  VERBATIM
)
add_custom_target(SceLibcBridge DEPENDS "${libc_bridge_archive}")
add_library(SceLibcBridge_stub STATIC IMPORTED GLOBAL)
set_target_properties(SceLibcBridge_stub PROPERTIES IMPORTED_LOCATION "${libc_bridge_archive}")
add_dependencies(SceLibcBridge_stub SceLibcBridge)
