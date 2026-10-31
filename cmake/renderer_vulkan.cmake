if(NOT BUILD_CLIENT OR NOT BUILD_RENDERER_VULKAN)
    return()
endif()

include(utils/set_output_dirs)
include(renderer_common)

# Only the headers are used (Vulkan::Headers): Vulkan is loaded at runtime
# through SDL (SDL_Vulkan_GetVkGetInstanceProcAddr), the renderer doesn't
# link libvulkan. Khronos' Vulkan-Headers package (VulkanHeaders) when it is
# installed (the CI builds use it), otherwise FindVulkan, which also needs
# the loader to consider Vulkan found.
find_package(VulkanHeaders CONFIG QUIET)
if(NOT TARGET Vulkan::Headers)
    find_package(Vulkan REQUIRED)
endif()

set(RENDERER_VULKAN_SOURCES
    ${SOURCE_DIR}/renderervk/tr_backend.c
    ${SOURCE_DIR}/renderervk/tr_bsp.c
    ${SOURCE_DIR}/renderervk/tr_cmds.c
    ${SOURCE_DIR}/renderervk/tr_curve.c
    ${SOURCE_DIR}/renderervk/tr_draw.c
    ${SOURCE_DIR}/renderervk/tr_flares.c
    ${SOURCE_DIR}/renderervk/tr_font.cpp
    ${SOURCE_DIR}/renderervk/tr_ghost.cpp
    ${SOURCE_DIR}/renderervk/tr_image.c
    ${SOURCE_DIR}/renderervk/tr_immediate.c
    ${SOURCE_DIR}/renderervk/tr_init.c
    ${SOURCE_DIR}/renderervk/tr_light.c
    ${SOURCE_DIR}/renderervk/tr_main.c
    ${SOURCE_DIR}/renderervk/tr_marks_permanent.c
    ${SOURCE_DIR}/renderervk/tr_marks.c
    ${SOURCE_DIR}/renderervk/tr_model.cpp
    ${SOURCE_DIR}/renderervk/tr_pipelines.c
    ${SOURCE_DIR}/renderervk/tr_scene.c
    ${SOURCE_DIR}/renderervk/tr_shade_calc.c
    ${SOURCE_DIR}/renderervk/tr_shade.c
    ${SOURCE_DIR}/renderervk/tr_shader.c
    ${SOURCE_DIR}/renderervk/tr_shadows.c
    ${SOURCE_DIR}/renderervk/tr_skin.c
    ${SOURCE_DIR}/renderervk/tr_sky_portal.cpp
    ${SOURCE_DIR}/renderervk/tr_sky.c
    ${SOURCE_DIR}/renderervk/tr_sphere_shade.cpp
    ${SOURCE_DIR}/renderervk/tr_sprite.c
    ${SOURCE_DIR}/renderervk/tr_staticgeom.c
    ${SOURCE_DIR}/renderervk/tr_staticmodels.cpp
    ${SOURCE_DIR}/renderervk/tr_sun_flare.cpp
    ${SOURCE_DIR}/renderervk/tr_surface.c
    ${SOURCE_DIR}/renderervk/tr_swipe.cpp
    ${SOURCE_DIR}/renderervk/tr_temporal.c
    ${SOURCE_DIR}/renderervk/tr_terrain.c
    ${SOURCE_DIR}/renderervk/tr_util.cpp
    ${SOURCE_DIR}/renderervk/tr_vis.cpp
    ${SOURCE_DIR}/renderervk/tr_world.c
    ${SOURCE_DIR}/renderervk/vk.c
    ${SOURCE_DIR}/renderervk/vk_framelog.c
    ${SOURCE_DIR}/renderervk/vk_image.c
    ${SOURCE_DIR}/renderervk/vk_instance.c
    ${SOURCE_DIR}/renderervk/tr_smp.c
    ${SOURCE_DIR}/renderervk/vk_output.c
    ${SOURCE_DIR}/renderervk/vk_pipeline.c
    ${SOURCE_DIR}/renderervk/vk_shaders.c
    ${SOURCE_DIR}/renderervk/vk_shaders_temporal.c
    ${SOURCE_DIR}/renderervk/vk_shaders_hdr.c
    ${SOURCE_DIR}/renderervk/vk_submit.c
    ${SOURCE_DIR}/renderervk/vk_temporal.c
)

# The window (sdl_vkimp.c) and the gamma ramp (sdl_gamma.c)
set(SDL_RENDERER_VULKAN_SOURCES
    ${SOURCE_DIR}/sdl/sdl_gamma.c
    ${SOURCE_DIR}/sdl/sdl_vkimp.c
)

set(RENDERER_VULKAN_BASENAME renderer_vulkan)
set(RENDERER_VULKAN_BINARY ${RENDERER_VULKAN_BASENAME})

list(APPEND RENDERER_VULKAN_BINARY_SOURCES
    ${RENDERER_COMMON_SOURCES}
    ${RENDERER_VULKAN_SOURCES}
    ${SDL_RENDERER_VULKAN_SOURCES}
    ${RENDERER_LIBRARY_SOURCES}
    ${DYNAMIC_RENDERER_SOURCES})

add_library(${RENDERER_VULKAN_BINARY} SHARED ${RENDERER_VULKAN_BINARY_SOURCES})

target_link_libraries(      ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_LIBRARIES} Vulkan::Headers)
target_include_directories( ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_INCLUDE_DIRS})
target_compile_definitions( ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_DEFINITIONS} USE_VULKAN)
target_compile_options(     ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_COMPILE_OPTIONS})
target_link_options(        ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_LINK_OPTIONS})

# Only GetRefAPI (Q_EXPORT) is exported: calls between the renderer's own
# functions are then direct instead of going through the PLT
set_target_properties(${RENDERER_VULKAN_BINARY} PROPERTIES
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

set_output_dirs(${RENDERER_VULKAN_BINARY})

INSTALL(TARGETS ${RENDERER_VULKAN_BINARY} DESTINATION ${INSTALL_LIBDIR_FULL})

if(MSVC)
    INSTALL(FILES $<TARGET_PDB_FILE:${RENDERER_VULKAN_BINARY}> DESTINATION ${INSTALL_LIBDIR_FULL} OPTIONAL)
endif()
