#pragma once

#include <string>

namespace engine::script {

enum class ScriptErrorKind {
    Syntax,   // the chunk did not compile
    Runtime,  // the chunk or a handler raised an error, including a binding's controlled error
};

// One failure, with the message Lua produced. The message carries the chunk name and line when Lua reports them.
struct ScriptError {
    ScriptErrorKind kind = ScriptErrorKind::Runtime;
    std::string message;
};

}  // namespace engine::script
