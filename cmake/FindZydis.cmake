# Fedora's zydis-devel ships only a pkg-config file (no ZydisConfig.cmake).
# Provides the Zydis::Zydis target that bbport / shadPS4 CMake expects.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_ZYDIS QUIET IMPORTED_TARGET zydis)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Zydis REQUIRED_VARS PC_ZYDIS_FOUND VERSION_VAR PC_ZYDIS_VERSION)
if(Zydis_FOUND AND NOT TARGET Zydis::Zydis)
    add_library(Zydis::Zydis INTERFACE IMPORTED)
    target_link_libraries(Zydis::Zydis INTERFACE PkgConfig::PC_ZYDIS)
endif()
