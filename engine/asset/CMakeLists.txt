add_library(engine_asset
    ac_car_data.cpp
    ac_car_data.h
    acd_archive.cpp
    acd_archive.h
    asset_manager.cpp
    asset_manager.h
    asset_paths.cpp
    asset_paths.h
    asset_references.cpp
    asset_references.h
    asset_registry.cpp
    asset_registry.h
    compressed_texture_cache.cpp
    compressed_texture_cache.h
    dds_decoder.cpp
    dds_decoder.h
    gltf_compression.cpp
    gltf_compression.h
    gltf_model_loader.cpp
    gltf_model_loader.h
    gta5_game_data.cpp
    gta5_game_data.h
    gta5_importer.cpp
    gta5_importer.h
    gta5_resource.cpp
    gta5_resource.h
    kn5_importer.cpp
    kn5_importer.h
    kn5_reader.cpp
    kn5_reader.h
    material_definition.cpp
    material_definition.h
    material_graph_runtime.cpp
    material_graph_runtime.h
    mesh.cpp
    mesh.h
    model_cache.cpp
    model_cache.h
    model_import_target.cpp
    model_import_target.h
    model_loader.cpp
    model_loader.h
    model_post_process.cpp
    model_post_process.h
    rage_resource.cpp
    rage_resource.h
    svg_document.h
    svg_font.cpp
    svg_font.h
    svg_icon.cpp
    svg_icon.h
    texture_compression.cpp
    texture_compression.h
    texture_loader.cpp
    texture_loader.h
    texture_preparation.cpp
    texture_preparation.h
)
miniengine_group_target_sources(engine_asset)

# The Assets window's type icons (SVG, written by tools/editor_icons/phosphor_to_svg.py), built
# into the library so they need no file at run time.
set(_asset_icon_names folder model material scene texture file parent_folder audio)
foreach(_icon IN LISTS _asset_icon_names)
    set(_icon_svg "${CMAKE_CURRENT_SOURCE_DIR}/icons/${_icon}.svg")
    set(_icon_source "${CMAKE_CURRENT_BINARY_DIR}/generated/asset_icon_${_icon}_svg.cpp")
    add_custom_command(
        OUTPUT "${_icon_source}"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${_icon_svg}"
            "-DOUTPUT=${_icon_source}"
            "-DSYMBOL=kAssetIconSvg_${_icon}"
            -P "${PROJECT_SOURCE_DIR}/cmake/MiniEngineEmbedFile.cmake"
        DEPENDS "${_icon_svg}" "${PROJECT_SOURCE_DIR}/cmake/MiniEngineEmbedFile.cmake"
        COMMENT "Embedding icons/${_icon}.svg"
        VERBATIM
    )
    target_sources(engine_asset PRIVATE "${_icon_source}")
    source_group("Generated" FILES "${_icon_source}")
endforeach()

target_include_directories(engine_asset
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
    PRIVATE
        "${Stb_INCLUDE_DIR}"
        "${MINIENGINE_TINYGLTF_INCLUDE_DIR}"
        "${PROJECT_SOURCE_DIR}/third_party/phosphor"
)

target_link_libraries(engine_asset
    PUBLIC
        engine_core
        engine_scene
        glm::glm
        imgui::imgui
        yaml-cpp::yaml-cpp
    PRIVATE
        # The Assets window tells sound files by their extension (audio_file.h).
        engine_audio
        unofficial::bc7enc-rdo::bc7enc-rdo
        unofficial::tinyexr::tinyexr
        meshoptimizer::meshoptimizer
        draco::draco
)

if(TARGET KTX::ktx)
    target_link_libraries(engine_asset PRIVATE KTX::ktx)
    target_compile_definitions(engine_asset PRIVATE MINIENGINE_HAS_KTX=1)
else()
    target_compile_definitions(engine_asset PRIVATE MINIENGINE_HAS_KTX=0)
endif()
