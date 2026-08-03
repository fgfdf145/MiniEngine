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
