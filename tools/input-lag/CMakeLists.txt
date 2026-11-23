# input-lag: a Windows test tool timing a button press to the picture that
# shows it, for our window and Supermodel alike (tools/input-lag/README.md).
# Built on its own (cmake -S tools/input-lag -B build-lag) or from the main
# project with -DSPIKEOUT_INPUT_LAG=ON. Windows only: it needs the ViGEmBus
# driver (a virtual Xbox pad) and DXGI desktop duplication.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
    project(input_lag CXX)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "input-lag is a Windows tool")
endif()

include(FetchContent)
# ViGEmClient (MIT, github.com/nefarius/ViGEmClient): the user-mode library
# that plugs a virtual Xbox 360 pad into the ViGEmBus driver.
FetchContent_Declare(vigemclient
    GIT_REPOSITORY https://github.com/nefarius/ViGEmClient.git
    GIT_TAG 9e842ba1c3a6efbb90d9b7e9346a55b1a3d10494 # v1.21.222.0
    GIT_SHALLOW FALSE)
# (Populated by hand: its own CMakeLists builds a resource file only MSVC takes.)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(vigemclient)
if(NOT vigemclient_POPULATED)
    FetchContent_Populate(vigemclient)
endif()
add_library(vigemclient_static STATIC ${vigemclient_SOURCE_DIR}/src/ViGEmClient.cpp)
target_include_directories(vigemclient_static PUBLIC ${vigemclient_SOURCE_DIR}/include)
target_compile_features(vigemclient_static PUBLIC cxx_std_17)
target_link_libraries(vigemclient_static PUBLIC setupapi)

add_executable(input-lag ${CMAKE_CURRENT_LIST_DIR}/input-lag.cpp)
target_compile_features(input-lag PRIVATE cxx_std_17)
target_link_libraries(input-lag PRIVATE vigemclient_static d3d11 dxgi xinput gdi32 user32 winmm)
if(MINGW)
    target_link_options(input-lag PRIVATE -static)
endif()
