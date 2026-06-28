#include "../include/compiler.h"
#include "../include/shell_vm.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace shellvm;

namespace {

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: embed_bytecode <script-path> <output-cpp> [symbol-name]\n";
        return 1;
    }

    const std::string scriptPath = argv[1];
    const std::string outputPath = argv[2];
    const std::string symbolName = argc >= 4 ? argv[3] : "kEmbeddedBytecode";

    const std::string script = readFile(scriptPath);
    if (script.empty()) {
        std::cerr << "Failed to read script: " << scriptPath << "\n";
        return 2;
    }

    try {
        const Bytecode bytecode = Compiler::compile(script);
        const std::string cppSource = Compiler::bytecodeToCppArray(bytecode, symbolName);
        if (!writeFile(outputPath, cppSource)) {
            std::cerr << "Failed to write output file: " << outputPath << "\n";
            return 3;
        }

        std::cout << "Embedded bytecode written to " << outputPath
                  << " (" << bytecode.size() << " bytes)\n";
        return 0;
    } catch (const VMException& e) {
        std::cerr << "Compile error: " << e.what() << "\n";
        return 4;
    }
}
