add_library(engine_asset
    asset_manager.cpp
    gltf_model_loader.cpp
    material_graph_runtime.cpp
    mesh.cpp
    model_loader.cpp
    model_post_process.cpp
    texture_loader.cpp
)

target_include_directories(engine_asset
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
PRIVATE
    ${Stb_INCLUDE_DIR}
    ${MINIENGINE_TINYGLTF_INCLUDE_DIR}
)

target_compile_definitions(engine_asset
PRIVATE
    MINIENGINE_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
    MINIENGINE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
)

target_link_libraries(engine_asset
PUBLIC
    engine_scene
    engine_core
    glm::glm
    yaml-cpp::yaml-cpp
    imgui::imgui
)
