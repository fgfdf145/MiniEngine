add_library(engine_audio
    audio_engine.cpp
    audio_engine.h
    audio_file.cpp
    audio_file.h
    fmod_bank.cpp
    fmod_bank.h
    gamepad_haptics.cpp
    gamepad_haptics.h
    haptics_synth.cpp
    haptics_synth.h
    miniaudio_impl.cpp
    sound_bank.cpp
    sound_bank.h
    sound_event.cpp
    sound_event.h
)
miniengine_group_target_sources(engine_audio)

target_include_directories(engine_audio
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
    PRIVATE
        # miniaudio.h, and stb_vorbis.c for Ogg Vorbis (miniaudio_impl.cpp).
        "${MINIENGINE_MINIAUDIO_INCLUDE_DIR}"
        "${Stb_INCLUDE_DIR}"
)

# miniaudio stays private: the public header speaks glm and std::filesystem.
target_link_libraries(engine_audio
    PUBLIC
        engine_core
        glm::glm
    PRIVATE
        yaml-cpp::yaml-cpp
)

# Third-party code compiled in this target: its warnings are not ours.
if(MSVC)
    set_source_files_properties(miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
endif()
