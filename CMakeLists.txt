cmake_minimum_required(VERSION 3.31)

project(Engine)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    if(WIN32)
    foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin)
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin)
    endforeach()
    endif()

    if(MSVC)
        add_compile_options(/utf-8)
    endif()

    option(USE_MINGW "Build using MinGW for Windows" OFF)
    if(USE_MINGW)
        message(STATUS "Configuring MinGW build for Windows")
        set(CMAKE_SYSTEM_NAME Windows)
    endif()
endif()

set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendors)

# --- Vendor build options ---
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FREEGLUT_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(FREEGLUT_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

# --- Assimp build options ---
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)

# Propagate /utf-8 into CMAKE_CXX_FLAGS so assimp's flag check sees it
# and doesn't add the conflicting /source-charset:utf-8
if(MSVC)
  string(FIND "${CMAKE_CXX_FLAGS}" "/utf-8" _idx)
  if(_idx EQUAL -1)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /utf-8")
  endif()
endif()

# --- Build all vendor dependencies ---
add_subdirectory(vendors/freeglut)
add_subdirectory(vendors/SDL3)
add_subdirectory(vendors/spdlog)
add_subdirectory(vendors/json)
add_subdirectory(vendors/yaml-cpp)
add_subdirectory(vendors/assimp)

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

# --- Engine module sources ---
set(ANIMATION_SOURCES
    src/Animation/AnimationStateMachine.cpp
    src/Animation/AnimatorComponent.cpp
)

set(ASSETS_SOURCES
    src/Assets/ModelLoader.cpp
    src/Assets/Skybox.cpp
    src/Assets/stb_image_impl.cpp
)

set(CORE_SOURCES
    src/Core/Application.cpp
    src/Core/CommandLine.cpp
    src/Core/File.cpp
    src/Core/Logger.cpp
    src/Core/ScopedTimer.cpp
    src/Core/Serializable.cpp
    src/Core/Timer.cpp
    src/Core/Window.cpp
)

set(CULLING_SOURCES
    src/Culling/CullingSystem.cpp
)

set(DEBUG_SOURCES
    src/Debug/Benchmark.cpp
    src/Debug/DebugLineRenderer.cpp
    src/Debug/DebugOverlay.cpp
    src/Debug/SystemMetrics.cpp
)

set(GRAPHICS_COMMON_SOURCES
    src/Graphics/Common/GraphicsConfig.cpp
    src/Graphics/Common/Material.cpp
    src/Graphics/Common/MeshBatch.cpp
    src/Graphics/Common/RenderCommandQueue.cpp
    src/Graphics/Common/RenderCommands.cpp
    src/Graphics/Common/RendererFactory.cpp
    src/Graphics/Common/ResourceManager.cpp
)

set(GRAPHICS_OPENGL_SOURCES
    src/Graphics/OpenGL/glad.c
    src/Graphics/OpenGL/OpenGLBuffer.cpp
    src/Graphics/OpenGL/OpenGLCubemapTexture.cpp
    src/Graphics/OpenGL/OpenGLIBL.cpp
    src/Graphics/OpenGL/OpenGLRenderer.cpp
    src/Graphics/OpenGL/OpenGLShader.cpp
    src/Graphics/OpenGL/OpenGLTexture.cpp
)

set(GRAPHICS_VULKAN_SOURCES
    src/Graphics/Vulkan/VulkanBuffer.cpp
    src/Graphics/Vulkan/VulkanCubemapTexture.cpp
    src/Graphics/Vulkan/VulkanDevice.cpp
    src/Graphics/Vulkan/VulkanRenderer.cpp
    src/Graphics/Vulkan/VulkanShader.cpp
    src/Graphics/Vulkan/VulkanSwapchain.cpp
    src/Graphics/Vulkan/VulkanTexture.cpp
)

set(GRAPHICS_DIRECTX11_SOURCES
    src/Graphics/DirectX11/DirectX11Buffer.cpp
    src/Graphics/DirectX11/DirectX11CubemapTexture.cpp
    src/Graphics/DirectX11/DirectX11Renderer.cpp
    src/Graphics/DirectX11/DirectX11Shader.cpp
    src/Graphics/DirectX11/DirectX11Texture.cpp
)

set(GRAPHICS_DIRECTX12_SOURCES
    src/Graphics/DirectX12/DirectX12Buffer.cpp
    src/Graphics/DirectX12/DirectX12CubemapTexture.cpp
    src/Graphics/DirectX12/DirectX12Renderer.cpp
    src/Graphics/DirectX12/DirectX12Shader.cpp
    src/Graphics/DirectX12/DirectX12Texture.cpp
)

set(LIGHTING_SOURCES
    src/Lighting/AreaLight.cpp
    src/Lighting/DirectionalLight.cpp
    src/Lighting/Light.cpp
    src/Lighting/LightManager.cpp
    src/Lighting/PointLight.cpp
    src/Lighting/SpotLight.cpp
)

set(MATH_SOURCES
    src/Math/Color.cpp
    src/Math/Math.cpp
    src/Math/Quaternion.cpp
)

set(PHYSICS_SOURCES
    src/Physics/ColliderComponent.cpp
    src/Physics/CollisionDetection.cpp
    src/Physics/DynamicAABBTree.cpp
    src/Physics/PhysicsWorld.cpp
    src/Physics/RigidbodyComponent.cpp
)

set(SCENE_SOURCES
    src/Scene/Camera.cpp
    src/Scene/FirstPersonController.cpp
    src/Scene/FreeLookCameraControllerComponent.cpp
    src/Scene/GameObject.cpp
    src/Scene/MaterialComponent.cpp
    src/Scene/MeshComponent.cpp
    src/Scene/SceneBase.cpp
    src/Scene/Scene.cpp
    src/Scene/TransformComponent.cpp
)

set(UI_SOURCES
    src/UI/UI.cpp
)

set(ENGINE_SOURCES
    ${ANIMATION_SOURCES}
    ${ASSETS_SOURCES}
    ${CORE_SOURCES}
    ${CULLING_SOURCES}
    ${DEBUG_SOURCES}
    ${GRAPHICS_COMMON_SOURCES}
    ${GRAPHICS_OPENGL_SOURCES}
    ${GRAPHICS_VULKAN_SOURCES}
    ${LIGHTING_SOURCES}
    ${MATH_SOURCES}
    ${PHYSICS_SOURCES}
    ${SCENE_SOURCES}
    ${UI_SOURCES}
)

# --- DirectX module sources (Windows only) ---
if(WIN32 OR USE_MINGW)
    list(APPEND ENGINE_SOURCES
        ${GRAPHICS_DIRECTX11_SOURCES}
        ${GRAPHICS_DIRECTX12_SOURCES}
    )
endif()

# --- Header files (for IDE source groups) ---
file(GLOB_RECURSE ENGINE_PUBLIC_HEADERS "include/public/*.hpp")
file(GLOB_RECURSE ENGINE_PRIVATE_HEADERS "include/private/*.hpp")

file(GLOB_RECURSE IMGUI_SOURCES
    ${VENDOR_DIR}/imgui/imgui.cpp
    ${VENDOR_DIR}/imgui/imgui_draw.cpp
    ${VENDOR_DIR}/imgui/imgui_tables.cpp
    ${VENDOR_DIR}/imgui/imgui_widgets.cpp
    ${VENDOR_DIR}/imgui/backends/imgui_impl_sdl3.cpp
    ${VENDOR_DIR}/imgui/backends/imgui_impl_vulkan.cpp
    ${VENDOR_DIR}/imgui/backends/imgui_impl_opengl3.cpp
    ${VENDOR_DIR}/imgui/misc/cpp/imgui_stdlib.cpp
)

# --- DirectX ImGui backends (Windows only) ---
if(WIN32 OR USE_MINGW)
    list(APPEND IMGUI_SOURCES
        ${VENDOR_DIR}/imgui/backends/imgui_impl_dx11.cpp
        ${VENDOR_DIR}/imgui/backends/imgui_impl_dx12.cpp
    )
endif()

# --- Combined source list ---
set(ENGINE_ALL_SOURCES_AND_HEADERS
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
    ${ENGINE_PUBLIC_HEADERS}
    ${ENGINE_PRIVATE_HEADERS}
)

# --- Create Engine shared library ---
add_library(Engine SHARED ${ENGINE_ALL_SOURCES_AND_HEADERS})

# --- Source groups (mirror disk layout in IDEs) ---
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_ALL_SOURCES_AND_HEADERS})

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
    assimp::assimp
)
target_link_libraries(Engine PUBLIC
    yaml-cpp::yaml-cpp
)

# --- DirectX SDK (Windows only) ---
if(WIN32 OR USE_MINGW)
    target_link_libraries(Engine PRIVATE
        d3d11.lib
        dxgi.lib
        dxguid.lib
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

    # --- Vulkan-targeted shaders only; OpenGL "_gl" variants are not SPIR-V ---
    file(GLOB VULKAN_SHADERS
        "${SHADER_SOURCE_DIR}/*.vert"
        "${SHADER_SOURCE_DIR}/*.frag"
    )
    list(FILTER VULKAN_SHADERS EXCLUDE REGEX "_gl\\.(vert|frag)$")

    set(BIN_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin/assets/shaders")

    foreach(SHADER ${VULKAN_SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")
        set(BIN_SPV    "${BIN_SHADER_DIR}/${SHADER_NAME}.spv")
        add_custom_command(
            OUTPUT ${SPV_OUTPUT}
            COMMAND ${GLSLC} ${SHADER} -o ${SPV_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPV_OUTPUT} ${BIN_SPV}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME} to SPIR-V and syncing to bin/"
        )
        list(APPEND SPV_SHADERS ${SPV_OUTPUT})
    endforeach()

    add_custom_target(CompileShaders ALL DEPENDS ${SPV_SHADERS})
    add_dependencies(Engine CompileShaders)
else()
    message(STATUS "glslc not found, SPIR-V shaders will not be auto-compiled")
endif()
