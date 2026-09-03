# com.apple.security.hypervisor is read from the process's main executable, never from a linked
# dylib: signing only libhvf-emulator.dylib leaves hv_vm_create() returning HV_DENIED (0xfae94007).
# Linking rewrites the signature, so this has to re-run on every link.
function(sogen_enable_hypervisor_entitlement target)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    return()
  endif()

  get_property(target_type TARGET ${target} PROPERTY TYPE)
  if(NOT target_type STREQUAL "EXECUTABLE")
    return()
  endif()

  find_program(SOGEN_CODESIGN_COMMAND codesign)
  if(NOT SOGEN_CODESIGN_COMMAND)
    message(WARNING "codesign was not found; ${target} will not be able to use the Hypervisor.framework backend")
    return()
  endif()

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${SOGEN_CODESIGN_COMMAND}" --force --sign - --entitlements
            "${PROJECT_SOURCE_DIR}/cmake/misc/hypervisor.entitlements" "$<TARGET_FILE:${target}>"
    COMMENT "Signing ${target} with com.apple.security.hypervisor"
    VERBATIM
  )
endfunction()
