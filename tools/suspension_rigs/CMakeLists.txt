add_executable(miniengine_suspension_rig_report
    suspension_rig_report.cpp
)
miniengine_group_target_sources(miniengine_suspension_rig_report)

target_link_libraries(miniengine_suspension_rig_report
    PRIVATE
        engine_asset
        engine_physics
)

if(WIN32)
    set(_rig_report_dlls $<TARGET_RUNTIME_DLLS:miniengine_suspension_rig_report>)
    add_custom_command(TARGET miniengine_suspension_rig_report POST_BUILD
        COMMAND "$<$<BOOL:${_rig_report_dlls}>:${CMAKE_COMMAND};-E;copy_if_different;${_rig_report_dlls};$<TARGET_FILE_DIR:miniengine_suspension_rig_report>>"
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_suspension_rig_report PROPERTIES FOLDER "Tools")
