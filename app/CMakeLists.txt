add_executable(miniengine_app
    main.cpp
)

target_link_libraries(miniengine_app
PRIVATE
    engine_application
)

if(WIN32)
    set(_miniengine_app_runtime_dlls
        $<TARGET_RUNTIME_DLLS:miniengine_app>
    )

    add_custom_command(TARGET miniengine_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_miniengine_app_runtime_dlls}
            $<TARGET_FILE_DIR:miniengine_app>
        COMMAND_EXPAND_LISTS
    )
endif()
