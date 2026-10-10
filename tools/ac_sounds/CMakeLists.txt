add_executable(miniengine_ac_sounds
    ac_sounds.cpp
)
miniengine_group_target_sources(miniengine_ac_sounds)
miniengine_enable_utf8_code_page(miniengine_ac_sounds)

target_link_libraries(miniengine_ac_sounds
    PRIVATE
        engine_audio
)

if(WIN32)
    set(_ac_sounds_dlls $<TARGET_RUNTIME_DLLS:miniengine_ac_sounds>)
    add_custom_command(TARGET miniengine_ac_sounds POST_BUILD
        COMMAND "$<$<BOOL:${_ac_sounds_dlls}>:${CMAKE_COMMAND};-E;copy_if_different;${_ac_sounds_dlls};$<TARGET_FILE_DIR:miniengine_ac_sounds>>"
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_ac_sounds PROPERTIES FOLDER "Tools")
