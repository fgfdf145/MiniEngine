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

add_test(
    NAME miniengine.vcpkg_layout_contract
    COMMAND ${CMAKE_COMMAND}
        "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_layout_contract.cmake"
)
