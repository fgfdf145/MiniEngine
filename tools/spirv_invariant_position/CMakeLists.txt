# Marks a vertex shader's Position output Invariant after slangc (engine/renderer/CMakeLists.txt).
add_executable(miniengine_spirv_invariant_position main.cpp)
set_target_properties(miniengine_spirv_invariant_position PROPERTIES FOLDER "Tools")
