add_executable(miniengine_scene_identity_tests
    editor_scene_identity_tests.cpp
)
miniengine_group_target_sources(miniengine_scene_identity_tests)

target_link_libraries(miniengine_scene_identity_tests
    PRIVATE
        engine_logic
)

add_test(
    NAME miniengine.scene_identity
    COMMAND miniengine_scene_identity_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_scene_identity_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_scene_identity_tests>
            $<TARGET_FILE_DIR:miniengine_scene_identity_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_scene_identity_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_gizmo_settings_tests
    gizmo_settings_tests.cpp
)
miniengine_group_target_sources(miniengine_gizmo_settings_tests)

target_link_libraries(miniengine_gizmo_settings_tests
    PRIVATE
        engine_logic
        imguizmo::imguizmo
)

add_test(
    NAME miniengine.gizmo_settings
    COMMAND miniengine_gizmo_settings_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_gizmo_settings_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_gizmo_settings_tests>
            $<TARGET_FILE_DIR:miniengine_gizmo_settings_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_gizmo_settings_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_material_alpha_tests
    material_alpha_tests.cpp
)
miniengine_group_target_sources(miniengine_material_alpha_tests)

target_link_libraries(miniengine_material_alpha_tests
    PRIVATE
        engine_asset
        engine_render_core
)

add_test(
    NAME miniengine.material_alpha
    COMMAND miniengine_material_alpha_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_material_alpha_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_material_alpha_tests>
            $<TARGET_FILE_DIR:miniengine_material_alpha_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_material_alpha_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_exposure_tests
    exposure_tests.cpp
)
miniengine_group_target_sources(miniengine_exposure_tests)

target_link_libraries(miniengine_exposure_tests
    PRIVATE
        engine_render_core
)

add_test(
    NAME miniengine.exposure
    COMMAND miniengine_exposure_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_exposure_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_exposure_tests>
            $<TARGET_FILE_DIR:miniengine_exposure_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_exposure_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_tonemap_tests
    tonemap_tests.cpp
    third_party/gt7/gt7_tone_mapping_reference.h
)
miniengine_group_target_sources(miniengine_tonemap_tests)

target_link_libraries(miniengine_tonemap_tests
    PRIVATE
        engine_render_core
)

add_test(
    NAME miniengine.tonemap
    COMMAND miniengine_tonemap_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_tonemap_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_tonemap_tests>
            $<TARGET_FILE_DIR:miniengine_tonemap_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_tonemap_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_scene_lighting_tests
    scene_lighting_tests.cpp
)
miniengine_group_target_sources(miniengine_scene_lighting_tests)

target_link_libraries(miniengine_scene_lighting_tests
    PRIVATE
        engine_render_core
)

add_test(
    NAME miniengine.scene_lighting
    COMMAND miniengine_scene_lighting_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_scene_lighting_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_scene_lighting_tests>
            $<TARGET_FILE_DIR:miniengine_scene_lighting_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_scene_lighting_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_shadow_cascades_tests
    shadow_cascades_tests.cpp
)
miniengine_group_target_sources(miniengine_shadow_cascades_tests)

target_link_libraries(miniengine_shadow_cascades_tests
    PRIVATE
        engine_render_core
)

add_test(
    NAME miniengine.shadow_cascades
    COMMAND miniengine_shadow_cascades_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_shadow_cascades_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_shadow_cascades_tests>
            $<TARGET_FILE_DIR:miniengine_shadow_cascades_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_shadow_cascades_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_motion_history_tests
    motion_history_tests.cpp
)
miniengine_group_target_sources(miniengine_motion_history_tests)

target_link_libraries(miniengine_motion_history_tests
    PRIVATE
        engine_render_core
)

add_test(
    NAME miniengine.motion_history
    COMMAND miniengine_motion_history_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_motion_history_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_motion_history_tests>
            $<TARGET_FILE_DIR:miniengine_motion_history_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_motion_history_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_texture_compression_tests
    texture_compression_tests.cpp
)
miniengine_group_target_sources(miniengine_texture_compression_tests)

# The encoder library is linked directly as well: the tests decode what engine_asset encoded.
target_link_libraries(miniengine_texture_compression_tests
    PRIVATE
        engine_asset
        unofficial::bc7enc-rdo::bc7enc-rdo
)
target_include_directories(miniengine_texture_compression_tests PRIVATE "${Stb_INCLUDE_DIR}")

add_test(
    NAME miniengine.texture_compression
    COMMAND miniengine_texture_compression_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_texture_compression_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_texture_compression_tests>
            $<TARGET_FILE_DIR:miniengine_texture_compression_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_texture_compression_tests PROPERTIES FOLDER "Tests")

add_executable(miniengine_texture_preparation_tests
    texture_preparation_tests.cpp
)
miniengine_group_target_sources(miniengine_texture_preparation_tests)

target_link_libraries(miniengine_texture_preparation_tests
    PRIVATE
        engine_asset
)

add_test(
    NAME miniengine.texture_preparation
    COMMAND miniengine_texture_preparation_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_texture_preparation_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_texture_preparation_tests>
            $<TARGET_FILE_DIR:miniengine_texture_preparation_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_texture_preparation_tests PROPERTIES FOLDER "Tests")

add_test(
    NAME miniengine.vcpkg_layout_contract
    COMMAND ${CMAKE_COMMAND}
        "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_layout_contract.cmake"
)

set(miniengine_format_contract_cases
    powershell_accepts_literals
    powershell_rejects_same_line_braces
    powershell_switch_numeric_clause
    powershell_switch_variable_clause
    powershell_switch_bareword_clause
    powershell_switch_existing_clause_forms
    powershell_switch_accepts_scriptblock_pattern
    powershell_switch_rejects_inline_scriptblock_action
    powershell_switch_rejects_inline_file_body
    powershell_switch_avoids_non_switch_blocks
    bash_accepts_heredoc_literals
    bash_rejects_hyphenated_function
    bash_here_string_is_atomic
    bash_shift_contexts_are_not_heredocs
    bash_heredoc_delimiter_quote_removal
    bash_ansi_c_heredoc_delimiter
    bash_ansi_c_extended_escape_decoding
    bash_ansi_c_unicode_heredoc_delimiters
    bash_ansi_c_control_heredoc_delimiter
    bash_ansi_c_short_unicode_heredoc_delimiters
    bash_ansi_c_digit_control_heredoc_delimiter
    nested_cmakelists_classification
    explicit_formatter_override
    environment_formatter_override
    automatic_formatter_preference
    missing_formatter_diagnostic
    wrong_formatter_diagnostic
    powershell_command_resolution
)

foreach(miniengine_format_contract_case IN LISTS miniengine_format_contract_cases)
    add_test(
        NAME "miniengine.format_contract.${miniengine_format_contract_case}"
        COMMAND ${CMAKE_COMMAND}
            "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
            "-DTEST_TEMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/format-contract"
            "-DTEST_CASE=${miniengine_format_contract_case}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/format_contract.cmake"
    )
endforeach()

# The three units under test call no Vulkan entry point, so their translation units are compiled
# directly into the test rather than linking engine_renderer, which would drag the whole Vulkan
# backend and the ImGui editor into a unit test. Vulkan::Vulkan is here for the headers only.
add_executable(miniengine_scene_pass_tests
    scene_pass_tests.cpp
    ${PROJECT_SOURCE_DIR}/engine/renderer/vulkan/render_target_layout.cpp
    ${PROJECT_SOURCE_DIR}/engine/renderer/vulkan/format_support.cpp
    ${PROJECT_SOURCE_DIR}/engine/renderer/vulkan/scene_pass_order.cpp
)
# No miniengine_group_target_sources here: that helper's source_group(TREE ...) requires every
# file to sit under the calling directory, and this target deliberately compiles three .cpp files
# from engine/renderer/vulkan/ so a unit test does not have to link the whole Vulkan backend.
# The helper only organises IDE source folders, so skipping it for this one target costs nothing
# but its folder grouping in the Visual Studio solution.

target_include_directories(miniengine_scene_pass_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}"
)

# No POST_BUILD TARGET_RUNTIME_DLLS copy here, unlike the other test targets: this one links
# only Vulkan::Vulkan for its headers and Vulkan-1.dll is a system library, so the runtime DLL
# list is empty by construction and there is nothing to copy.
target_link_libraries(miniengine_scene_pass_tests
    PRIVATE
        Vulkan::Vulkan
)

add_test(
    NAME miniengine.scene_pass
    COMMAND miniengine_scene_pass_tests
)

set_target_properties(miniengine_scene_pass_tests PROPERTIES FOLDER "Tests")

# asset_registry.cpp depends only on engine_core and yaml-cpp, so it is compiled directly into
# the test rather than linking engine_asset. engine_asset links imgui PUBLIC because
# asset_manager.cpp is an ImGui panel that still lives in the asset layer; compiling the one
# translation unit under test keeps that out of a unit test. Same reasoning as
# miniengine_scene_pass_tests above.
add_executable(miniengine_asset_registry_tests
    asset_registry_tests.cpp
    ${PROJECT_SOURCE_DIR}/engine/asset/asset_registry.cpp
)

# No miniengine_group_target_sources: that helper's source_group(TREE ...) requires every file
# to sit under the calling directory, and this target deliberately compiles one .cpp from
# engine/asset/.

target_include_directories(miniengine_asset_registry_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(miniengine_asset_registry_tests
    PRIVATE
        engine_core
        yaml-cpp::yaml-cpp
)

if(WIN32)
    add_custom_command(TARGET miniengine_asset_registry_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_asset_registry_tests>
            $<TARGET_FILE_DIR:miniengine_asset_registry_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(
    NAME miniengine.asset_registry
    COMMAND miniengine_asset_registry_tests
)

set_target_properties(miniengine_asset_registry_tests PROPERTIES FOLDER "Tests")

# Unlike the registry test, this one drives the glTF loader, whose translation unit pulls in
# tinygltf, stb and nlohmann::json. Listing those by hand would be brittle, so this target links
# engine_asset and accepts the transitive imgui dependency that asset_manager.cpp still imposes
# on that library.
add_executable(miniengine_model_import_texture_tests
    model_import_texture_tests.cpp
)
miniengine_group_target_sources(miniengine_model_import_texture_tests)
miniengine_enable_utf8_code_page(miniengine_model_import_texture_tests)

target_link_libraries(miniengine_model_import_texture_tests
    PRIVATE
        engine_asset
)

if(WIN32)
    add_custom_command(TARGET miniengine_model_import_texture_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_model_import_texture_tests>
            $<TARGET_FILE_DIR:miniengine_model_import_texture_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(
    NAME miniengine.model_import_texture
    COMMAND miniengine_model_import_texture_tests
)

set_target_properties(miniengine_model_import_texture_tests PROPERTIES FOLDER "Tests")

# model_cache.cpp calls ApplyImportedMaterialInfo, whose translation unit pulls in
# material_graph_runtime and yaml-cpp. Listing that chain by hand would be brittle, so this
# target links engine_asset, like miniengine_model_import_texture_tests.
add_executable(miniengine_model_cache_tests
    model_cache_tests.cpp
)
miniengine_group_target_sources(miniengine_model_cache_tests)

target_link_libraries(miniengine_model_cache_tests
    PRIVATE
        engine_asset
)

if(WIN32)
    add_custom_command(TARGET miniengine_model_cache_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_model_cache_tests>
            $<TARGET_FILE_DIR:miniengine_model_cache_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(
    NAME miniengine.model_cache
    COMMAND miniengine_model_cache_tests
)

set_target_properties(miniengine_model_cache_tests PROPERTIES FOLDER "Tests")

# asset_references.cpp depends only on engine_core, so it is compiled directly into the test
# rather than linking engine_asset, keeping imgui out of a unit test. Same reasoning as
# miniengine_asset_registry_tests above.
add_executable(miniengine_asset_references_tests
    asset_references_tests.cpp
    ${PROJECT_SOURCE_DIR}/engine/asset/asset_references.cpp
)

# No miniengine_group_target_sources: that helper's source_group(TREE ...) requires every file
# to sit under the calling directory, and this target deliberately compiles one .cpp from
# engine/asset/.

target_include_directories(miniengine_asset_references_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(miniengine_asset_references_tests
    PRIVATE
        engine_core
)

if(WIN32)
    add_custom_command(TARGET miniengine_asset_references_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_asset_references_tests>
            $<TARGET_FILE_DIR:miniengine_asset_references_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(
    NAME miniengine.asset_references
    COMMAND miniengine_asset_references_tests
)

set_target_properties(miniengine_asset_references_tests PROPERTIES FOLDER "Tests")
