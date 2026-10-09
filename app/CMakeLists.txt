add_executable(miniengine_app
    main.cpp
)
miniengine_group_target_sources(miniengine_app)
miniengine_enable_utf8_code_page(miniengine_app)

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
miniengine_copy_dlss_runtime(miniengine_app)

# The Agility SDK's runtime, which D3D12SDKPath (main.cpp) points the system's D3D12 at, and its debug
# layer (MINIENGINE_NVRHI_VALIDATION).
if(MINIENGINE_WITH_D3D12)
    add_custom_command(TARGET miniengine_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:miniengine_app>/D3D12
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_PROPERTY:Microsoft::DirectX12-Core,IMPORTED_LOCATION_RELEASE>
            $<TARGET_PROPERTY:Microsoft::DirectX12-Layers,IMPORTED_LOCATION_DEBUG>
            $<TARGET_FILE_DIR:miniengine_app>/D3D12
    )
endif()
