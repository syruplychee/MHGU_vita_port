# Public build options. Defaults produce the playable loader VPK.

option(VITA_PORT_BUILD_NATIVE_LOADER
    "Build the native game loader and VPK (requires locked game ExeFS)" ON)
option(VITA_PORT_BUILD_BOOTSTRAP
    "Build the no-game bootstrap VPK used to verify a fresh VitaSDK" ON)
option(VITA_PORT_NATIVE_SDK_DSO
    "Execute the original SDK DSO; required for the production runtime" ON)
option(VITA_PORT_SAVE_SELFTEST
    "Run the save backend self-test at service init (diagnostic only)" OFF)
option(VITA_PORT_ENGINE_CAPTURE
    "Capture one full preparation and SDK encoding, then stop (diagnostic only)" OFF)

set(VITA_PORT_EXEFS "" CACHE PATH
    "Directory containing the locked 1.4.0 ExeFS: rtld, main, subsdk0, sdk, main.npdm")
set(VITA_PORT_NATIVE_TITLE_ID "MHGUVP018" CACHE STRING
    "Title ID of the game loader VPK")
set(VITA_PORT_STATIC_SHADER_PACKAGE_DIR
    "ux0:data/mhgu-vita-port/static-shaders-current" CACHE STRING
    "Runtime directory selected for the VPSHIX1/VPSRES1 static shader package")
set(VITA_PORT_KUBRIDGE_LIBRARY "" CACHE FILEPATH
    "Optional kubridge import stub; requires a matching kernel plugin on hardware")

string(LENGTH "${VITA_PORT_NATIVE_TITLE_ID}" VP_TITLE_ID_LENGTH)
if(NOT VITA_PORT_NATIVE_TITLE_ID MATCHES "^[A-Z0-9]+$" OR NOT VP_TITLE_ID_LENGTH EQUAL 9)
    message(FATAL_ERROR
        "VITA_PORT_NATIVE_TITLE_ID must be nine uppercase letters/digits")
endif()
if(NOT VITA_PORT_STATIC_SHADER_PACKAGE_DIR MATCHES
    "^ux0:data/mhgu-vita-port/[A-Za-z0-9._-]+$")
    message(FATAL_ERROR
        "VITA_PORT_STATIC_SHADER_PACKAGE_DIR must be one child of ux0:data/mhgu-vita-port")
endif()
if(VITA_PORT_STATIC_SHADER_PACKAGE_DIR MATCHES
    "^ux0:data/mhgu-vita-port/(static-shaders|shaders)$")
    message(FATAL_ERROR
        "The compiled package directory must not overwrite the frozen shader bank")
endif()

if(VITA_PORT_BUILD_NATIVE_LOADER)
    if(NOT VITA_PORT_EXEFS)
        message(FATAL_ERROR
            "Set VITA_PORT_EXEFS (or env MHGU_EXEFS) to the locked 1.4.0 ExeFS directory")
    endif()
    foreach(module rtld main subsdk0 sdk main.npdm)
        if(NOT EXISTS "${VITA_PORT_EXEFS}/${module}")
            message(FATAL_ERROR
                "VITA_PORT_EXEFS is missing ${module}: ${VITA_PORT_EXEFS}")
        endif()
    endforeach()
endif()
