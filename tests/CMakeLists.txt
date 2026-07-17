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
