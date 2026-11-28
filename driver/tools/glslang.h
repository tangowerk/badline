#pragma once

// GLSL -> SPIR-V for the GL shader presets, through the C API of the system glslang
// (the same library the Rust Vulkan renderer uses; a second, static copy would interpose
// the C++ symbols of libglslang.so).

#include <string>
#include <vector>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

struct GLSlang {

    auto compileFragment(const std::string& code, std::vector<unsigned>& out, std::string& error) -> bool {
        return compile(code, GLSLANG_STAGE_FRAGMENT, out, error);
    }

    auto compileVertex(const std::string& code, std::vector<unsigned>& out, std::string& error) -> bool {
        return compile(code, GLSLANG_STAGE_VERTEX, out, error);
    }

    auto compile(const std::string& code, glslang_stage_t stage, std::vector<unsigned>& out, std::string& error) -> bool {
        glslang_initialize_process();
        glslang_input_t input = {};
        input.language = GLSLANG_SOURCE_GLSL;
        input.stage = stage;
        input.client = GLSLANG_CLIENT_VULKAN;
        input.client_version = GLSLANG_TARGET_VULKAN_1_0;
        input.target_language = GLSLANG_TARGET_SPV;
        input.target_language_version = GLSLANG_TARGET_SPV_1_0;
        input.code = code.c_str();
        input.default_version = 110;
        input.default_profile = GLSLANG_NO_PROFILE;
        input.messages = (glslang_messages_t)(GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_VULKAN_RULES_BIT | GLSLANG_MSG_SPV_RULES_BIT);
        input.resource = glslang_default_resource();

        glslang_shader_t* shader = glslang_shader_create(&input);
        glslang_program_t* program = nullptr;
        bool ok = glslang_shader_preprocess(shader, &input) && glslang_shader_parse(shader, &input);
        if (ok) {
            program = glslang_program_create();
            glslang_program_add_shader(program, shader);
            ok = glslang_program_link(program, input.messages);
        }
        if (ok) {
            glslang_program_SPIRV_generate(program, stage);
            out.resize(glslang_program_SPIRV_get_size(program));
            glslang_program_SPIRV_get(program, out.data());
        } else {
            error += std::string(glslang_shader_get_info_log(shader)) + "\n";
            error += std::string(glslang_shader_get_info_debug_log(shader)) + "\n";
            if (program)
                error += std::string(glslang_program_get_info_log(program)) + "\n";
        }
        if (program)
            glslang_program_delete(program);
        glslang_shader_delete(shader);
        glslang_finalize_process();
        return ok;
    }
};
