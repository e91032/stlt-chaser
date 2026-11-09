cmake_minimum_required(VERSION 3.15)

# Resolume loads FFGL plugins as .dll on Windows and as .bundle on macOS (Intel and Apple Silicon).
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Oldest macOS the plugins run on")
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build universal plugins")
endif()

project(STLT-CHASER VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(FFGL_ROOT "${THIRD_PARTY}/ffgl" CACHE PATH "Path to the resolume/ffgl SDK checkout")
set(FFGL_LIB "${FFGL_ROOT}/source/lib")

file(GLOB FFGL_SOURCES
    "${FFGL_LIB}/ffgl/*.cpp"
    "${FFGL_LIB}/ffglex/*.cpp"
    "${FFGL_LIB}/ffglquickstart/*.cpp"
)

# The FFGL SDK and the shared document code, built once for every plugin.
add_library(stilletchaser-common STATIC
    src/ChaseDriver.cpp
    ${FFGL_SOURCES}
    src/ChaseDocument.cpp
    src/ChaseDocument.h
    src/FrameHistory.cpp
    src/FrameHistory.h
    src/Outline.cpp
    src/Outline.h
)
set_target_properties(stilletchaser-common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(stilletchaser-common PUBLIC
    "${FFGL_LIB}"
    "${THIRD_PARTY}/nlohmann"
    src
)

if(WIN32)
    target_include_directories(stilletchaser-common PUBLIC "${FFGL_ROOT}/deps/glew-2.1.0/include")
    target_compile_definitions(stilletchaser-common PUBLIC GLEW_STATIC WIN32 _WINDOWS _USRDLL NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(stilletchaser-common PUBLIC /utf-8)
    target_link_libraries(stilletchaser-common PUBLIC
        "${FFGL_ROOT}/deps/glew-2.1.0/lib/Release/x64/glew32s.lib"
        opengl32
    )
elseif(APPLE)
    # OpenGL is deprecated on macOS but still what FFGL hosts use.
    target_compile_definitions(stilletchaser-common PUBLIC GL_SILENCE_DEPRECATION TARGET_OS_MAC=1)
    target_link_libraries(stilletchaser-common PUBLIC "-framework OpenGL" "-framework AppKit")
else()
    # Linux (STLT-VJ loads the plugins there too): the system's GLEW and OpenGL.
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)
    target_link_libraries(stilletchaser-common PUBLIC GLEW::GLEW OpenGL::GL)
endif()

# plugMain is the only function a host calls, and nothing in the plugins references it, so the linker
# would drop FFGL.cpp from stilletchaser-common. Windows keeps it through FFGLPlugins.def; elsewhere
# the symbols are marked undefined so the linker pulls them in and the plugin exports them.
function(add_stilletchaser_plugin name)
    if(WIN32)
        add_library(${name} SHARED ${ARGN} "${FFGL_ROOT}/build/windows/FFGLPlugins.def")
    elseif(APPLE)
        add_library(${name} MODULE ${ARGN})
        set_target_properties(${name} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION bundle
            MACOSX_BUNDLE_BUNDLE_NAME ${name}
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.stltchaser.${name}"
            MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        )
        target_link_options(${name} PRIVATE "LINKER:-u,_plugMain" "LINKER:-u,_SetLogCallback")
    else()
        add_library(${name} MODULE ${ARGN})
        set_target_properties(${name} PROPERTIES PREFIX "") # STLT-CHASER.so, as the plugin is named
        target_link_options(${name} PRIVATE "LINKER:-u,plugMain" "LINKER:-u,SetLogCallback")
    endif()
    target_link_libraries(${name} PRIVATE stilletchaser-common)
endfunction()

add_stilletchaser_plugin(STLT-CHASER src/Stilletchaser.cpp src/Stilletchaser.h)
add_stilletchaser_plugin(STLT-WIPER src/Wiper.cpp)
add_stilletchaser_plugin(STLT-OUTLINER src/Outliner.cpp)
add_stilletchaser_plugin(STLT-SAMPLER src/Sampler.cpp)
add_stilletchaser_plugin(STLT-TILER src/Tiler.cpp)
add_stilletchaser_plugin(STLT-BLEND src/Blend.cpp)
add_stilletchaser_plugin(STLT-MASK src/Mask.cpp)

# The same Outliner source, built a second time as a source plugin that needs no input.
add_stilletchaser_plugin(STLT-OUTLINER-SOURCE src/Outliner.cpp)
target_compile_definitions(STLT-OUTLINER-SOURCE PRIVATE STLT_OUTLINER_SOURCE)

# `cmake --install build --prefix dist` gathers the plugins that ship with the editor.
set(STLT_PLUGINS STLT-CHASER STLT-WIPER STLT-OUTLINER STLT-OUTLINER-SOURCE
    STLT-SAMPLER STLT-TILER STLT-BLEND STLT-MASK)
if(WIN32)
    foreach(plugin ${STLT_PLUGINS})
        install(FILES $<TARGET_FILE:${plugin}> DESTINATION .)
    endforeach()
else()
    install(TARGETS ${STLT_PLUGINS} LIBRARY DESTINATION . BUNDLE DESTINATION .)
endif()

add_executable(document_test tests/document_test.cpp)
target_link_libraries(document_test PRIVATE stilletchaser-common)

# Loads the built plugin DLLs like a host would and checks what they render (Windows, needs a GPU).
if(WIN32)
    add_executable(host_test tests/host_test.cpp)
    target_include_directories(host_test PRIVATE "${FFGL_LIB}" "${FFGL_ROOT}/deps/glew-2.1.0/include")
    target_compile_definitions(host_test PRIVATE GLEW_STATIC NOMINMAX)
    target_link_libraries(host_test PRIVATE "${FFGL_ROOT}/deps/glew-2.1.0/lib/Release/x64/glew32s.lib" opengl32)
    add_dependencies(host_test STLT-CHASER STLT-WIPER STLT-OUTLINER)
endif()

# The Bumper driver's timing and envelope, without a GPU.
add_executable(bumper_test tests/bumper_test.cpp)
target_link_libraries(bumper_test PRIVATE stilletchaser-common)
