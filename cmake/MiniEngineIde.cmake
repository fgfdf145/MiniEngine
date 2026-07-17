include_guard(GLOBAL)

foreach(_miniengine_engine_target IN ITEMS
    engine_core
    engine_platform
    engine_scene
    engine_asset
    engine_logic
    engine_render_core
    engine_editor
    engine_renderer
    engine_application
)
    if(TARGET ${_miniengine_engine_target})
        set_target_properties(${_miniengine_engine_target} PROPERTIES FOLDER "Engine")
    endif()
endforeach()

if(TARGET engine_renderer_shaders)
    set_target_properties(engine_renderer_shaders PROPERTIES FOLDER "Assets")
endif()

if(TARGET miniengine_app)
    set_target_properties(miniengine_app PROPERTIES FOLDER "App")
endif()

if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND TARGET miniengine_app)
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT miniengine_app)
endif()
