cmake_minimum_required(VERSION 3.20)
project(EncodeAll CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(-Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type -ffunction-sections -fdata-sections)
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
if(EA_VERSION_OVERRIDE)  # update test only
  add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:EA_VERSION="${EA_VERSION_OVERRIDE}"> $<$<COMPILE_LANGUAGE:CXX>:EA_VERSION_W=L"${EA_VERSION_OVERRIDE}">)
endif()

set(CORE
  src/core/util.cpp src/core/json.cpp src/core/process.cpp src/core/net.cpp
  src/media/probe.cpp src/media/compat.cpp src/media/encode.cpp src/media/hwenc.cpp
  src/dl/ytdlp.cpp
  src/app/settings.cpp src/app/deps.cpp src/app/queue.cpp src/app/runners.cpp src/app/appupdate.cpp)
set(LIBS winhttp bcrypt shell32 ole32 uuid advapi32 user32 gdi32 shlwapi)

add_library(eacore STATIC ${CORE})
target_link_libraries(eacore PUBLIC ${LIBS})

add_executable(EncodeAll WIN32
  src/ui/main.cpp src/ui/gfx.cpp src/ui/menu.cpp src/ui/window.cpp src/ui/views.cpp src/ui/thumbs.cpp
  src/app.rc)
# windres does not report header dependencies: rebuild the resources when the version, icon or manifest changes.
set_source_files_properties(src/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/version.h;${CMAKE_SOURCE_DIR}/src/app.manifest;${CMAKE_SOURCE_DIR}/assets/encodeall.ico")
target_link_libraries(EncodeAll PRIVATE eacore windowscodecs comctl32 uxtheme dwmapi comdlg32 imm32 gdiplus)
target_link_options(EncodeAll PRIVATE -municode -mwindows -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s)

add_executable(unit_tests tests/unit.cpp)
target_link_libraries(unit_tests PRIVATE eacore)
target_link_options(unit_tests PRIVATE -municode -static -static-libgcc -static-libstdc++)

add_executable(integration_tests tests/integration.cpp)
target_link_libraries(integration_tests PRIVATE eacore)
target_link_options(integration_tests PRIVATE -municode -static -static-libgcc -static-libstdc++)
