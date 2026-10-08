cmake_minimum_required(VERSION 3.31)

project(Engine VERSION 0.1.0)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

option(SLEAK_ENGINE_INSTALL "Generate install and package export rules"
    ${PROJECT_IS_TOP_LEVEL})

# --- Vendor build options ---
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FREEGLUT_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(FREEGLUT_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

# --- Shared vendor libraries ship as their own packages when installing ---
set(SDL_INSTALL ${SLEAK_ENGINE_INSTALL} CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL ${SLEAK_ENGINE_INSTALL} CACHE BOOL "" FORCE)

# --- Assimp build options ---
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL ${SLEAK_ENGINE_INSTALL} CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

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
    src/Graphics/Common/VertexLayout.cpp
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
    src/Graphics/Vulkan/VulkanBloom.cpp
    src/Graphics/Vulkan/VulkanBuffer.cpp
    src/Graphics/Vulkan/VulkanCubemapTexture.cpp
    src/Graphics/Vulkan/VulkanDeferred.cpp
    src/Graphics/Vulkan/VulkanDescriptors.cpp
    src/Graphics/Vulkan/VulkanDevice.cpp
    src/Graphics/Vulkan/VulkanIBL.cpp
    src/Graphics/Vulkan/VulkanPipelines.cpp
    src/Graphics/Vulkan/VulkanRenderer.cpp
    src/Graphics/Vulkan/VulkanShader.cpp
    src/Graphics/Vulkan/VulkanShadow.cpp
    src/Graphics/Vulkan/VulkanSSAO.cpp
    src/Graphics/Vulkan/VulkanSSR.cpp
    src/Graphics/Vulkan/VulkanSwapchain.cpp
    src/Graphics/Vulkan/VulkanTAA.cpp
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
    src/Graphics/DirectX12/DirectX12UploadContext.cpp
    src/Graphics/DirectX12/DirectX12UploadRing.cpp
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
add_library(Sleak::Engine ALIAS Engine)

set_target_properties(Engine PROPERTIES
    VERSION ${Engine_VERSION}
    SOVERSION ${Engine_VERSION_MAJOR}.${Engine_VERSION_MINOR}
)
target_compile_features(Engine PUBLIC cxx_std_23)

# --- Generated version header ---
set(ENGINE_GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/include)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.hpp.in
    ${ENGINE_GENERATED_INCLUDE_DIR}/Core/Version.hpp
    @ONLY
)

# --- Source groups (mirror disk layout in IDEs) ---
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_ALL_SOURCES_AND_HEADERS})

target_compile_definitions(Engine PRIVATE SLEAK_ENGINE)

# --- Include directories ---
target_include_directories(Engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/public>
        $<BUILD_INTERFACE:${ENGINE_GENERATED_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${VENDOR_DIR}/spdlog/include>
        $<BUILD_INTERFACE:${VENDOR_DIR}/json/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/SleakEngine>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/SleakEngine/vendor>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/private
        ${Vulkan_INCLUDE_DIR}
        ${OPENGL_INCLUDE_DIR}
        ${VENDOR_DIR}/imgui
        ${VENDOR_DIR}/glad/include
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

# --- SPIR-V shaders (committed .spv are only the fallback without glslc) ---
set(ENGINE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
file(GLOB ENGINE_VULKAN_SHADERS CONFIGURE_DEPENDS
    "${ENGINE_SHADER_DIR}/*.vert"
    "${ENGINE_SHADER_DIR}/*.frag"
)
list(FILTER ENGINE_VULKAN_SHADERS EXCLUDE REGEX "_gl\\.(vert|frag)$")

find_program(GLSLC glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "${VULKAN_SDK}/Bin")
set(ENGINE_SPIRV_FILES "")
set(ENGINE_COMMITTED_SPIRV "")
foreach(_src ${ENGINE_VULKAN_SHADERS})
    if(EXISTS "${_src}.spv")
        list(APPEND ENGINE_COMMITTED_SPIRV "${_src}.spv")
    endif()
endforeach()

if(GLSLC)
    set(ENGINE_SPIRV_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${ENGINE_SPIRV_DIR}")
    foreach(_src ${ENGINE_VULKAN_SHADERS})
        get_filename_component(_name "${_src}" NAME)
        set(_spv "${ENGINE_SPIRV_DIR}/${_name}.spv")
        add_custom_command(
            OUTPUT "${_spv}"
            COMMAND ${GLSLC} "${_src}" -o "${_spv}"
            DEPENDS "${_src}"
            COMMENT "Compiling ${_name} to SPIR-V"
            VERBATIM
        )
        list(APPEND ENGINE_SPIRV_FILES "${_spv}")
    endforeach()
else()
    set(_missing "")
    foreach(_src ${ENGINE_VULKAN_SHADERS})
        if(NOT EXISTS "${_src}.spv")
            get_filename_component(_name "${_src}" NAME)
            list(APPEND _missing "${_name}")
        endif()
    endforeach()
    if(_missing)
        message(FATAL_ERROR
            "glslc not found and these shaders have no committed SPIR-V: "
            "${_missing}. Install the Vulkan SDK or set VULKAN_SDK.")
    endif()
    message(WARNING
        "glslc not found, using the committed SPIR-V in assets/shaders. "
        "Vulkan GLSL edits will not reach the build until glslc is installed "
        "(Vulkan SDK, or set VULKAN_SDK).")
    set(ENGINE_SPIRV_FILES ${ENGINE_COMMITTED_SPIRV})
endif()

set(_stage_outputs "")
if(PROJECT_IS_TOP_LEVEL)
    set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/engine_spirv_staged.stamp")
    set(_dst "${CMAKE_SOURCE_DIR}/bin/assets/shaders")
    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ENGINE_SPIRV_FILES} "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS ${ENGINE_SPIRV_FILES}
        COMMENT "Staging engine SPIR-V into bin/assets/shaders"
        VERBATIM
    )
    list(APPEND _stage_outputs "${_stamp}")
endif()

add_custom_target(CompileShaders ALL
    DEPENDS ${ENGINE_SPIRV_FILES} ${_stage_outputs})
add_dependencies(Engine CompileShaders)

set_property(GLOBAL PROPERTY ENGINE_SPIRV_FILES ${ENGINE_SPIRV_FILES})
set_property(GLOBAL PROPERTY ENGINE_COMMITTED_SPIRV ${ENGINE_COMMITTED_SPIRV})

function(_engine_collect_targets dir out)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub ${_subdirs})
        _engine_collect_targets("${_sub}" _sub_targets)
        list(APPEND _targets ${_sub_targets})
    endforeach()
    set(${out} ${_targets} PARENT_SCOPE)
endfunction()

# Stage after each consuming executable so its own asset copy never wins
function(_engine_stage_spirv_for_consumers)
    get_property(_spv GLOBAL PROPERTY ENGINE_SPIRV_FILES)
    get_property(_committed GLOBAL PROPERTY ENGINE_COMMITTED_SPIRV)
    _engine_collect_targets("${CMAKE_SOURCE_DIR}" _targets)
    foreach(_target ${_targets})
        get_target_property(_type ${_target} TYPE)
        get_target_property(_libs ${_target} LINK_LIBRARIES)
        if(NOT _type STREQUAL "EXECUTABLE" OR NOT "Engine" IN_LIST _libs)
            continue()
        endif()
        set(_dst "$<TARGET_FILE_DIR:${_target}>/assets/shaders")
        set(_stamp "${CMAKE_BINARY_DIR}/${_target}_engine_spirv.stamp")
        add_custom_command(
            OUTPUT "${_stamp}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_spv} "${_dst}"
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            DEPENDS ${_spv} ${_committed} "$<TARGET_FILE:${_target}>"
            COMMENT "Staging engine SPIR-V for ${_target}"
            VERBATIM
        )
        add_custom_target(${_target}_EngineSpirv ALL DEPENDS "${_stamp}")
        add_dependencies(${_target}_EngineSpirv ${_target})
    endforeach()
endfunction()

if(NOT PROJECT_IS_TOP_LEVEL)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
        CALL _engine_stage_spirv_for_consumers)
endif()

# --- Install and package export ---
if(SLEAK_ENGINE_INSTALL)
    set(ENGINE_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/Engine)
    set(ENGINE_INCLUDE_DIR ${CMAKE_INSTALL_INCLUDEDIR}/SleakEngine)

    if(UNIX AND NOT APPLE)
        set_target_properties(Engine PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()

    install(TARGETS Engine
        EXPORT EngineTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    # WindowHelper.hpp needs SDL3 headers, which are private to the engine
    install(DIRECTORY include/public/
        DESTINATION ${ENGINE_INCLUDE_DIR}
        FILES_MATCHING PATTERN "*.hpp"
        PATTERN "WindowHelper.hpp" EXCLUDE
    )
    install(FILES ${ENGINE_GENERATED_INCLUDE_DIR}/Core/Version.hpp
        DESTINATION ${ENGINE_INCLUDE_DIR}/Core
    )

    # Header-only dependencies that public headers include
    install(DIRECTORY ${VENDOR_DIR}/spdlog/include/spdlog
        DESTINATION ${ENGINE_INCLUDE_DIR}/vendor
    )
    install(DIRECTORY ${VENDOR_DIR}/json/include/nlohmann
        DESTINATION ${ENGINE_INCLUDE_DIR}/vendor
    )

    if(UNIX AND NOT APPLE)
        install(FILES ${Vulkan_LIBRARIES} DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()

    install(EXPORT EngineTargets
        NAMESPACE Sleak::
        DESTINATION ${ENGINE_CMAKE_DIR}
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EngineConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/install/EngineConfig.cmake
        INSTALL_DESTINATION ${ENGINE_CMAKE_DIR}
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/EngineConfigVersion.cmake
        VERSION ${Engine_VERSION}
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/install/EngineConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/EngineConfigVersion.cmake
        DESTINATION ${ENGINE_CMAKE_DIR}
    )

    # Build tree package, usable with -DEngine_DIR=<build dir>
    export(EXPORT EngineTargets
        NAMESPACE Sleak::
        FILE ${CMAKE_CURRENT_BINARY_DIR}/EngineTargets.cmake
    )
    export(TARGETS yaml-cpp
        NAMESPACE yaml-cpp::
        FILE ${CMAKE_CURRENT_BINARY_DIR}/EngineYamlCppTargets.cmake
    )
    export(TARGETS assimp zlibstatic
        NAMESPACE assimp::
        FILE ${CMAKE_CURRENT_BINARY_DIR}/EngineAssimpTargets.cmake
    )
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EngineConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/EngineConfig.cmake
        INSTALL_DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
        INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}
    )
endif()
