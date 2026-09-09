cmake_minimum_required(VERSION 3.25)

project(
    NPCSpellVarianceVRPatch
    VERSION 1.0.1
    DESCRIPTION "Repair the NPC Spell Variance 2.7.0 UpdateCombat hook on Skyrim VR"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "NPCSpellVarianceVRPatch is a Windows SKSEVR plugin.")
endif()

include(CTest)
include(FetchContent)

set(
    COMMONLIBSSE_NG_SOURCE_DIR
    ""
    CACHE PATH "Optional path to a CommonLibSSE-NG v7.4.0 source checkout")

set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_PATCH_SAFETY OFF CACHE BOOL "" FORCE)

if(COMMONLIBSSE_NG_SOURCE_DIR)
    if(NOT EXISTS "${COMMONLIBSSE_NG_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "COMMONLIBSSE_NG_SOURCE_DIR does not contain CommonLibSSE-NG.")
    endif()
    add_subdirectory(
        "${COMMONLIBSSE_NG_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/CommonLibSSE-NG"
        EXCLUDE_FROM_ALL)
    include("${COMMONLIBSSE_NG_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
else()
    FetchContent_Declare(
        CommonLibSSE_NG
        GIT_REPOSITORY https://github.com/alandtse/CommonLibVR.git
        GIT_TAG 9b17b42fc9db23aea2b60f92690e778784e612b1
        GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(CommonLibSSE_NG)
    include("${commonlibsse_ng_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
endif()

add_commonlibsse_plugin(
    ${PROJECT_NAME}
    NAME "NPC Spell Variance VR Patch"
    AUTHOR "Treatid2"
    VERSION ${PROJECT_VERSION}
    MINIMUM_SKSE_VERSION 2.0.12
    COMPATIBLE_RUNTIMES 1.4.15
    STRUCT_DEPENDENT
    SOURCES
        src/AtomicPointerRead.h
        src/BinaryIdentity.h
        src/main.cpp
        src/PatchPlan.h
        src/PatchTransaction.h)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /EHsc /utf-8 /Zi
    /FI"${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h">)
target_link_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/DEBUG:FULL>)
target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "NPCSpellVarianceVRPatch")

if(BUILD_TESTING)
    add_executable(AtomicPointerReadTests tests/AtomicPointerReadTests.cpp)
    target_compile_features(AtomicPointerReadTests PRIVATE cxx_std_23)
    target_include_directories(AtomicPointerReadTests PRIVATE src)
    target_compile_options(AtomicPointerReadTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /EHsc /utf-8>)
    add_test(NAME AtomicPointerReadTests COMMAND AtomicPointerReadTests)

    add_executable(PatchPlanTests tests/PatchPlanTests.cpp)
    target_compile_features(PatchPlanTests PRIVATE cxx_std_23)
    target_include_directories(PatchPlanTests PRIVATE src)
    target_compile_options(PatchPlanTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /EHsc /utf-8>)
    add_test(NAME PatchPlanTests COMMAND PatchPlanTests)
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION SKSE/Plugins)
install(FILES README.md CHANGELOG.md LICENSE DESTINATION .)
