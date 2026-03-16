#include "gl_functions.h"

#include <SDL3/SDL_video.h>

#include <stdexcept>
#include <string>

namespace
{
template <typename T>
void LoadRequiredFunction(T& function, const char* name)
{
    function = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (function == nullptr)
    {
        throw std::runtime_error(std::string("Failed to load OpenGL function: ") + name);
    }
}
}

void OpenGLFunctions::Load()
{
    LoadRequiredFunction(ActiveTexture, "glActiveTexture");
    LoadRequiredFunction(AttachShader, "glAttachShader");
    LoadRequiredFunction(BindAttribLocation, "glBindAttribLocation");
    LoadRequiredFunction(BindBuffer, "glBindBuffer");
    LoadRequiredFunction(BindFramebuffer, "glBindFramebuffer");
    LoadRequiredFunction(BindRenderbuffer, "glBindRenderbuffer");
    LoadRequiredFunction(BindVertexArray, "glBindVertexArray");
    LoadRequiredFunction(BufferData, "glBufferData");
    LoadRequiredFunction(CheckFramebufferStatus, "glCheckFramebufferStatus");
    LoadRequiredFunction(CompileShader, "glCompileShader");
    LoadRequiredFunction(CreateProgram, "glCreateProgram");
    LoadRequiredFunction(CreateShader, "glCreateShader");
    LoadRequiredFunction(DeleteBuffers, "glDeleteBuffers");
    LoadRequiredFunction(DeleteFramebuffers, "glDeleteFramebuffers");
    LoadRequiredFunction(DeleteProgram, "glDeleteProgram");
    LoadRequiredFunction(DeleteRenderbuffers, "glDeleteRenderbuffers");
    LoadRequiredFunction(DeleteShader, "glDeleteShader");
    LoadRequiredFunction(DeleteVertexArrays, "glDeleteVertexArrays");
    LoadRequiredFunction(DisableVertexAttribArray, "glDisableVertexAttribArray");
    LoadRequiredFunction(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LoadRequiredFunction(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    LoadRequiredFunction(FramebufferTexture2D, "glFramebufferTexture2D");
    LoadRequiredFunction(GenBuffers, "glGenBuffers");
    LoadRequiredFunction(GenFramebuffers, "glGenFramebuffers");
    LoadRequiredFunction(GenRenderbuffers, "glGenRenderbuffers");
    LoadRequiredFunction(GenVertexArrays, "glGenVertexArrays");
    LoadRequiredFunction(GetProgramInfoLog, "glGetProgramInfoLog");
    LoadRequiredFunction(GetProgramiv, "glGetProgramiv");
    LoadRequiredFunction(GetShaderInfoLog, "glGetShaderInfoLog");
    LoadRequiredFunction(GetShaderiv, "glGetShaderiv");
    LoadRequiredFunction(GetUniformLocation, "glGetUniformLocation");
    LoadRequiredFunction(LinkProgram, "glLinkProgram");
    LoadRequiredFunction(RenderbufferStorage, "glRenderbufferStorage");
    LoadRequiredFunction(ShaderSource, "glShaderSource");
    LoadRequiredFunction(Uniform1i, "glUniform1i");
    LoadRequiredFunction(Uniform4fv, "glUniform4fv");
    LoadRequiredFunction(UniformMatrix4fv, "glUniformMatrix4fv");
    LoadRequiredFunction(UseProgram, "glUseProgram");
    LoadRequiredFunction(VertexAttribPointer, "glVertexAttribPointer");
}
