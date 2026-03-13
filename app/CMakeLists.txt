add_executable(miniengine_app
    main.cpp
)

target_link_libraries(miniengine_app
PRIVATE
    engine_core
    engine_platform
    engine_renderer
    imguizmo::imguizmo
    yaml-cpp::yaml-cpp
    EnTT::EnTT
)

add_custom_command(TARGET miniengine_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:miniengine_app>
        $<TARGET_FILE_DIR:miniengine_app>
    COMMAND_EXPAND_LISTS
)
