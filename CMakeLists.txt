cmake_minimum_required(VERSION 3.25)

project(UnleashedV3 VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(UNLEASHED_TEST_SERVER "Build the optional localhost read-only test server" ON)
option(UNLEASHED_OBSERVATION_ONLY "Disable physical output and target writes for visual validation" OFF)
# UniCore ray backend. The SDK is a local build artifact, not a package dependency; the
# product now owns the backend, so a missing SDK is a configuration error instead of a
# silently dropped feature.
set(UNICORN_DIR "D:/Desktop/SenseZen/ECS_O/05_SHARED_TOOLS/Unicorn/windows-msvc64-shared"
    CACHE PATH "Unicorn 2.x MSVC x64 SDK")
foreach(required
        "${UNICORN_DIR}/include/unicorn/unicorn.h"
        "${UNICORN_DIR}/lib/unicorn.lib"
        "${UNICORN_DIR}/bin/unicorn.dll")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "UniCore ray backend dependency missing: ${required}")
    endif()
endforeach()

enable_testing()
# Product modules are shared by the bounded offline checks.
include_directories("${CMAKE_SOURCE_DIR}/src")

if(MSVC)
    add_compile_options(/utf-8 /MP8)
endif()

# ============================================================================
# ImGui source files
# ============================================================================

set(IMGUI_SOURCES
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
    vendor/imgui/backends/imgui_impl_dx11.cpp
)

# ============================================================================
# Game / Engine source files
# ============================================================================

set(GAME_HEADERS
    include/resource.h
    include/Game/AbilityIcons.hpp
    include/Game/AimArchitecture.hpp
    include/Game/AimSmoothingTiming.hpp
    include/Game/Decrypt.hpp
    include/Game/Entity.hpp
    include/Game/EntityRosterPolicy.hpp
    include/Game/HeroGeometrySpec.hpp
    include/Game/HeroProfileSwitch.hpp
    include/Game/HeroPerkLookupSeed.hpp
    include/Game/HeroPerkRuntime.hpp
    include/Game/HeroPerks.hpp
    include/Game/HeroSkills.hpp
    include/output/InputOrchestrator.hpp
    include/Game/LeadPrediction.hpp
    include/Game/Offsets.hpp
    include/Game/EngineTrace.hpp
    include/Game/V3Raycast.hpp
    include/Game/RayDecision.hpp
    include/Game/RayQueryBroker.hpp
    include/Game/RayRuntime.hpp
    include/Game/Overwatch.hpp
    include/Game/ProductImageIdentity.hpp
    include/output/OutputOwnership.hpp
    include/output/OutputScheduler.hpp
    include/Game/RuntimeState.hpp
    include/Game/SDK.hpp
    include/Game/Structs.hpp
    include/Game/Target.hpp
    include/Game/TriggerBoneSelection.hpp
    include/Game/WeaponCadence.hpp
    include/Game/WeaponSpec.hpp
    include/output/OutputRuntime.hpp
    include/output/TimerResolution.hpp
    include/output/OutputTypes.hpp
    include/output/MockDiagnostics.hpp
    include/Memory/KeyState.hpp
    include/Memory/Memory.h
    include/Memory/MemoryRanges.h
    include/Renderer/IconManager.hpp
    include/Renderer/Overlay.hpp
    include/Renderer/Renderer.hpp
    include/Utils/Config.hpp
    include/Utils/CrashDump.hpp
    include/Utils/Diagnostics.hpp
    include/Utils/InputLabels.hpp
    include/Utils/TestServer.hpp
    include/Utils/Types.hpp
    include/Features/UI.hpp
)

include(cmake/OutputSources.cmake)

# Capture and pixel processing have no DMA or output-device dependency.
add_library(UnContourVision STATIC
    src/vision/CaptureCard.cpp
    src/vision/ContourDetection.cpp
    src/vision/AsheAmmo.cpp
    src/vision/ZaryaAmmo.cpp
    src/vision/ContourService.cpp
    src/config/ContourSettings.cpp
)
target_include_directories(UnContourVision PUBLIC include PRIVATE src vendor/leechcore vendor/imgui vendor/rapidjson)
target_link_libraries(UnContourVision PUBLIC strmiids ole32 oleaut32 uuid)

set(UNLEASHED_SOURCES
    src/main.cpp
    src/unleashed.rc
    src/Memory/Memory.cpp
    src/Renderer/IconManager.cpp
    src/Renderer/Overlay.cpp
    src/Renderer/OverlayWindow.cpp
    src/Renderer/Renderer.cpp
    src/Utils/CrashDump.cpp
    src/Utils/Diagnostics.cpp
    src/Utils/TestServer.cpp
    src/Utils/Types.cpp
    src/Utils/Config.cpp
    src/Features/UI.cpp
    src/ui/WorkspaceShell.cpp
    src/ui/SlotSelector.cpp
    src/ui/Text.cpp
    src/ui/Theme.cpp
    src/config/UiPreferences.cpp
    src/config/HeroSettings.cpp
    src/ui/pages/AimPage.cpp
    src/ui/pages/TriggerPage.cpp
    src/ui/pages/SkillPages.cpp
    src/ui/pages/AlertPage.cpp
    src/ui/pages/DisplayPages.cpp
    src/ui/pages/SettingsPages.cpp
    src/ui/vision/ContourPanel.cpp
    src/runtime/ProcessSession.cpp
    src/runtime/WorkerGroup.cpp
    src/features/aim/BehaviorController.cpp
    src/features/aim/Smoothing.cpp
    src/features/aim/SessionState.cpp
    src/features/alerts/Settings.cpp
    src/features/alerts/Speech.cpp
    src/features/alerts/Service.cpp
    src/features/alerts/SkillWorker.cpp
    src/config/HeroProfileWorker.cpp
    src/config/FileTransaction.cpp
    src/config/HeroSlotStore.cpp
    src/features/visuals/EntityVisuals.cpp
    src/features/visuals/HitboxContour.cpp
    src/features/visuals/SceneRenderer.cpp
    src/Game/camera/CameraWorker.cpp
    src/Game/map/MapReader.cpp
    src/Game/snapshots/PresentationWorker.cpp
    src/Game/entities/EntityDiscovery.cpp
    src/Game/entities/EntityReader.cpp
    src/Game/HeroGeometrySpec.cpp
    src/Game/HeroPerkRuntime.cpp
    src/Game/HeroSkills.cpp
    src/Game/Offsets.cpp
    src/Game/EngineTrace.cpp
    src/Game/EngineTrace/UnicornBackend.cpp
    src/Game/EngineTrace/VisionProbe.cpp
    src/Game/RayQueryBroker.cpp
    src/Game/RayRuntime.cpp
    src/Game/RayProbeRuntime.cpp
    src/runtime/RayProbeCheck.cpp
    src/Game/V3Raycast.cpp
    src/Features/visuals/RayScene.cpp
    src/Game/WeaponCadence.cpp
    src/Game/WeaponSpec.cpp
    vendor/stb/stb_image.cpp
    ${UNLEASHED_OUTPUT_BACKEND_SOURCES}
    ${UNLEASHED_OUTPUT_ACTION_SOURCES}
    src/ui/output/OutputPage.cpp
    src/ui/output/DevicePanels.cpp
    src/ui/output/SerialFields.cpp
    src/ui/output/kmbox_net/Panel.cpp
    src/ui/output/kmbox_serial/Panel.cpp
    src/ui/output/makcu/Panel.cpp
    src/ui/output/ferrum/Panel.cpp
    src/ui/output/catbox/Panel.cpp
    src/ui/output/dhzbox/Panel.cpp
    src/ui/output/mock/Panel.cpp
    src/output/settings/DeviceMaintenance.cpp
    src/output/devices/kmbox_net/Maintenance.cpp
    src/output/platform/NetworkMaintenance.cpp
)

# ============================================================================
# Executable
# ============================================================================

add_executable(UnleashedV3
    ${UNLEASHED_SOURCES}
    ${IMGUI_SOURCES}
)
# The desktop product should not launch a foreground console/Terminal window.
set_target_properties(UnleashedV3 PROPERTIES
    OUTPUT_NAME "UNDMA_V3"
    WIN32_EXECUTABLE TRUE
)
target_link_options(UnleashedV3 PRIVATE /ENTRY:mainCRTStartup)
if(UNLEASHED_OBSERVATION_ONLY)
    target_compile_definitions(UnleashedV3 PRIVATE UNLEASHED_OBSERVATION_ONLY=1)
endif()

# ============================================================================
# Include directories
# ============================================================================

target_include_directories(UnleashedV3 PRIVATE
    src
    include
    vendor/leechcore
    vendor/imgui
    vendor/imgui/backends
    vendor/rapidjson
    vendor/stb
    "${UNICORN_DIR}/include"
)

# ============================================================================
# Link libraries
# ============================================================================

target_link_libraries(UnleashedV3 PRIVATE
    comdlg32
    UnContourVision
    "${CMAKE_SOURCE_DIR}/vendor/leechcore/leechcore.lib"
    "${CMAKE_SOURCE_DIR}/vendor/leechcore/vmm.lib"
    d3d11
    dxgi
    windowscodecs
    ole32
    ws2_32
    winmm
    shell32
    dwmapi
    setupapi
    dbghelp
    # Import library for bin/unicorn.dll. The SDK also ships a 105 MB static unicorn.lib
    # (built against the debug CRT); linking that into the release product produces a
    # 35 MB exe and an MSVCRTD conflict, so the product uses the shared runtime instead.
    "${UNICORN_DIR}/lib/unicorn-import.lib"
)

# ============================================================================
# Post-build: copy DMA runtime DLLs
# ============================================================================

function(copy_leechcore_runtime target_name)
    # Supplier/SDK components are optional; selecting their source still requires them at runtime.
    foreach(optional_dll FTD2XX.dll vmsavedstatedumpprovider.dll)
        if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/leechcore/${optional_dll}")
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/vendor/leechcore/${optional_dll}"
                    "$<TARGET_FILE_DIR:${target_name}>")
        endif()
    endforeach()
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${UNICORN_DIR}/bin/unicorn.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMENT "${target_name}: stage unicorn.dll next to the product")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/info.db"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/leechcore.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/vmm.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/FTD3XX.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/FTD3XXWU.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/leechcore_device_rawtcp.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/leechcore_device_hvsavedstate.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/leechcore_driver.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/Helper64.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${target_name}>/config"
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_CONFIG=${CMAKE_SOURCE_DIR}/config/dma_device.cfg"
            "-DOUTPUT_CONFIG=$<TARGET_FILE_DIR:${target_name}>/config/dma_device.cfg"
            -P "${CMAKE_SOURCE_DIR}/cmake/CopyDefaultConfig.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/config/dma-examples"
            "$<TARGET_FILE_DIR:${target_name}>/config/dma-examples"
    )
endfunction()

copy_leechcore_runtime(UnleashedV3)

add_custom_target(V3Assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/icons"
        "$<TARGET_FILE_DIR:UnleashedV3>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/heroes"
        "$<TARGET_FILE_DIR:UnleashedV3>/assets/heroes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/ability-icons"
        "$<TARGET_FILE_DIR:UnleashedV3>/assets/ability-icons"
    DEPENDS UnleashedV3
    COMMENT "Sync V3 runtime icons and UI/overlay portraits, including asset-only changes"
)

# ============================================================================
# Compiler options
# ============================================================================

if(MSVC)
    function(enable_release_symbols target_name)
        target_compile_options(${target_name} PRIVATE "$<$<CONFIG:Release>:/Zi>")
        target_link_options(${target_name} PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL>")
    endfunction()

    enable_release_symbols(UnleashedV3)
endif()

target_compile_options(UnleashedV3 PRIVATE /W3 /wd4244 /wd4267 /wd4305 /wd4200)
target_compile_definitions(UnleashedV3 PRIVATE _CRT_SECURE_NO_WARNINGS)
if(UNLEASHED_TEST_SERVER)
    target_compile_definitions(UnleashedV3 PRIVATE UNLEASHED_TEST_SERVER=1)
endif()

# V3 raycast geometry and selector regression. This target is pure local/replay
# work and never opens a game process or an input/output device.
add_executable(V3RaycastSelfTest
    tools/v3_raycast_selftest.cpp
    src/Game/V3Raycast.cpp
    src/Game/EngineTrace.cpp
    src/Game/RayQueryBroker.cpp)
target_include_directories(V3RaycastSelfTest PRIVATE include vendor/imgui)
target_compile_definitions(V3RaycastSelfTest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(V3RaycastSelfTest PRIVATE /W3 /wd4244 /wd4267 /wd4305)
endif()
add_test(NAME V3RaycastSelfTest COMMAND V3RaycastSelfTest)

# ============================================================================
# Optional: UniCore trace bench (experimental, not part of the product binary)
# ============================================================================

option(UNLEASHED_TRACE_BENCH "Build the UniCore trace bench (requires the Unicorn SDK)" OFF)

if(UNLEASHED_TRACE_BENCH)
    foreach(required
            "${UNICORN_DIR}/include/unicorn/unicorn.h"
            "${UNICORN_DIR}/lib/unicorn.lib"
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/vmm.lib"
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/leechcore.lib")
        if(NOT EXISTS "${required}")
            message(FATAL_ERROR "UNLEASHED_TRACE_BENCH dependency missing: ${required}")
        endif()
    endforeach()

    add_executable(TraceBench
        tools/trace_bench.cpp
        tools/trace_bench_offline.cpp
        src/Game/EngineTrace.cpp
        src/Game/EngineTrace/UnicornBackend.cpp
        src/Game/EngineTrace/VisionProbe.cpp
    )
    target_include_directories(TraceBench PRIVATE
        include
        vendor/leechcore
        "${UNICORN_DIR}/include")
    target_compile_definitions(TraceBench PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(TraceBench PRIVATE /W3 /wd4244 /wd4267)
    target_link_libraries(TraceBench PRIVATE
        "${CMAKE_SOURCE_DIR}/vendor/leechcore/vmm.lib"
        "${CMAKE_SOURCE_DIR}/vendor/leechcore/leechcore.lib"
        "${UNICORN_DIR}/lib/unicorn.lib"
        psapi.lib)

    add_custom_command(TARGET TraceBench POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/vmm.dll" "$<TARGET_FILE_DIR:TraceBench>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/leechcore.dll" "$<TARGET_FILE_DIR:TraceBench>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/FTD3XX.dll" "$<TARGET_FILE_DIR:TraceBench>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/FTD3XXWU.dll" "$<TARGET_FILE_DIR:TraceBench>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/Helper64.dll" "$<TARGET_FILE_DIR:TraceBench>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/leechcore/info.db" "$<TARGET_FILE_DIR:TraceBench>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${UNICORN_DIR}/bin/unicorn.dll" "$<TARGET_FILE_DIR:TraceBench>"
        COMMENT "TraceBench: stage VMM/LeechCore/Unicorn next to the bench")
endif()
