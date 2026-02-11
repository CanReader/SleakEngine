cmake_minimum_required(VERSION 3.31)

project(Engine)

set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendors)

# --- Vendor build options ---
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FREEGLUT_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(FREEGLUT_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

# --- Build all vendor dependencies ---
add_subdirectory(vendors/glm)
add_subdirectory(vendors/freeglut)
add_subdirectory(vendors/glad)
add_subdirectory(vendors/fmt)
add_subdirectory(vendors/SDL3)
add_subdirectory(vendors/spdlog)
add_subdirectory(vendors/json)
add_subdirectory(vendors/yaml-cpp)

# --- Find system packages ---
find_package(OpenGL REQUIRED)

# --- Vulkan SDK (vendored) ---
set(VULKAN_SDK "${VENDOR_DIR}/VulkanSdk")
set(Vulkan_INCLUDE_DIR "${VULKAN_SDK}/Include")
if(WIN32)
    set(Vulkan_LIBRARIES "${VULKAN_SDK}/Lib/vulkan-1.lib")
elseif(UNIX AND NOT APPLE)
    set(Vulkan_LIBRARIES "${VULKAN_SDK}/Lib/libvulkan.so.1")
endif()

if (NOT EXISTS "${Vulkan_LIBRARIES}")
    message(FATAL_ERROR "Could not find Vulkan library at ${Vulkan_LIBRARIES}")
endif()

# --- Collect source files ---
file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.c"
    "src/*.cpp"
)

file(GLOB_RECURSE IMGUI_SOURCES
    ${VENDOR_DIR}/imgui/imgui.cpp
    ${VENDOR_DIR}/imgui/imgui_draw.cpp
    ${VENDOR_DIR}/imgui/imgui_tables.cpp
    ${VENDOR_DIR}/imgui/imgui_widgets.cpp
    ${VENDOR_DIR}/imgui/backends/imgui_impl_sdl3.cpp
    ${VENDOR_DIR}/imgui/backends/imgui_impl_dx11.cpp
    ${VENDOR_DIR}/imgui/backends/imgui_impl_dx12.cpp
    ${VENDOR_DIR}/imgui/backends/imgui_impl_vulkan.cpp
    ${VENDOR_DIR}/imgui/backends/imgui_impl_opengl3.cpp
)

# --- Create Engine shared library ---
add_library(Engine SHARED ${ENGINE_SOURCES} ${IMGUI_SOURCES})

target_compile_definitions(Engine PRIVATE SLEAK_ENGINE)

# --- Include directories ---
target_include_directories(Engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include/public
        ${VENDOR_DIR}/spdlog/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/private
        ${Vulkan_INCLUDE_DIR}
        ${OPENGL_INCLUDE_DIR}
        ${VENDOR_DIR}/imgui
        ${VENDOR_DIR}/glad/include
        ${VENDOR_DIR}/json/include
        ${VENDOR_DIR}/stb
)

# --- Link libraries ---
target_link_libraries(Engine PRIVATE
    SDL3::SDL3
    ${Vulkan_LIBRARIES}
    ${OPENGL_LIBRARIES}
    freeglut
    yaml-cpp::yaml-cpp
)

# --- DirectX SDK (Windows only) ---
if(WIN32 OR USE_MINGW)
    set(DX_SDK_DIR ${VENDOR_DIR}/DirectX-SDK)
    set(DirectX_INCLUDE_DIR ${DX_SDK_DIR}/Include)
    set(DirectX_LIB_DIR ${DX_SDK_DIR}/Lib)

    if(NOT EXISTS ${DX_SDK_DIR})
        message(FATAL_ERROR "DirectX SDK not found at: ${DX_SDK_DIR}")
    endif()

    target_link_libraries(Engine PRIVATE
        "${DirectX_LIB_DIR}/d3d11.lib"
        "${DirectX_LIB_DIR}/dxgi.lib"
        "${DirectX_LIB_DIR}/dxguid.lib"
        d3d12.lib
        d3dcompiler.lib
        pdh.lib
    )
endif()

# --- SPIR-V shader compilation (optional, requires glslc) ---
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "${VULKAN_SDK}/Bin")
if(GLSLC)
    set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
    set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")

    file(GLOB VULKAN_SHADERS
        "${SHADER_SOURCE_DIR}/default_shader.vert"
        "${SHADER_SOURCE_DIR}/default_shader.frag"
    )

    foreach(SHADER ${VULKAN_SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")
        add_custom_command(
            OUTPUT ${SPV_OUTPUT}
            COMMAND ${GLSLC} ${SHADER} -o ${SPV_OUTPUT}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME} to SPIR-V"
        )
        list(APPEND SPV_SHADERS ${SPV_OUTPUT})
    endforeach()

    add_custom_target(CompileShaders ALL DEPENDS ${SPV_SHADERS})
    add_dependencies(Engine CompileShaders)
else()
    message(STATUS "glslc not found, SPIR-V shaders will not be auto-compiled")
endif()
