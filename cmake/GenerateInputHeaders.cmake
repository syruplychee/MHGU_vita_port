# Configure-time generation and verification of input-bound constants.
#
# Nothing here reads game payload into the repository: target_identity.h,
# target_tls.h and target_startup.h are written to the build directory. The SDK
# contract headers are reviewed, tracked files and are verified against the
# locked ExeFS so a different game build fails before compilation.

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(VITA_PORT_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${VITA_PORT_GENERATED_DIR}")

execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/input/prepare_native_loader.py"
        --output "${VITA_PORT_GENERATED_DIR}/target_identity.h"
    RESULT_VARIABLE VP_GENERATE_IDENTITY_STATUS
    OUTPUT_VARIABLE VP_GENERATE_IDENTITY_OUTPUT
    ERROR_VARIABLE VP_GENERATE_IDENTITY_ERROR)
if(NOT VP_GENERATE_IDENTITY_STATUS EQUAL 0)
    message(FATAL_ERROR
        "Identity header generation failed: ${VP_GENERATE_IDENTITY_OUTPUT}${VP_GENERATE_IDENTITY_ERROR}")
endif()

function(vp_verify_sdk_contract script header)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/input/${script}"
            --verify --exefs "${VITA_PORT_EXEFS}"
            --header "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/${header}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR
            "${script} rejected the locked SDK or stale header ${header}: ${output}${error}")
    endif()
endfunction()

# prepare_sdk_dso.py verifies the reviewed plan against the identity lock.
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/input/prepare_sdk_dso.py" --verify
        --output "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/sdk_dso_contract.h"
    RESULT_VARIABLE VP_VERIFY_DSO_STATUS
    OUTPUT_VARIABLE VP_VERIFY_DSO_OUTPUT
    ERROR_VARIABLE VP_VERIFY_DSO_ERROR)
if(NOT VP_VERIFY_DSO_STATUS EQUAL 0)
    message(FATAL_ERROR
        "SDK DSO contract rejected: ${VP_VERIFY_DSO_OUTPUT}${VP_VERIFY_DSO_ERROR}")
endif()

vp_verify_sdk_contract(prepare_sdk_svc.py sdk_svc_contract.h)
vp_verify_sdk_contract(prepare_sdk_timer.py sdk_timer_contract.h)
vp_verify_sdk_contract(prepare_sdk_barrier.py sdk_barrier_contract.h)
