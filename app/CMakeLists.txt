add_executable(miniengine_app
    main.cpp
)

target_link_libraries(miniengine_app
PRIVATE
    engine_core
    engine_platform
    engine_renderer
)
