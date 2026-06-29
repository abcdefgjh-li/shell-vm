#include "include/shell_vm.h"
#include "include/vm_core.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <limits.h>
#include <unistd.h>
#endif

extern const std::uint8_t kEmbeddedBytecode[];
extern const std::size_t kEmbeddedBytecodeSize;

int main(int argc, char** argv) {
    using namespace shellvm;

    (void)argc;

#ifdef _WIN32
    _putenv_s("SHELLVM_TRACE", "");
#else
    unsetenv("SHELLVM_TRACE");

    char exePath[PATH_MAX];
    ssize_t exeLen = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (exeLen > 0) {
        exePath[exeLen] = '\0';
        char* slash = std::strrchr(exePath, '/');
        if (slash != nullptr) {
            *slash = '\0';
            chdir(exePath);
        }
    } else if (argv != nullptr && argv[0] != nullptr) {
        char fallback[PATH_MAX];
        std::snprintf(fallback, sizeof(fallback), "%s", argv[0]);
        char* slash = std::strrchr(fallback, '/');
        if (slash != nullptr) {
            *slash = '\0';
            chdir(fallback);
        }
    }
#endif

    VMConfig config;
    config.enableSandbox = false;
    config.enableFileAccess = true;
    config.timeout = 60000;
    config.maxStackSize = 8192;
    config.workingDirectory = ".";

    VM vm(config);
    Bytecode bytecode(kEmbeddedBytecode, kEmbeddedBytecode + kEmbeddedBytecodeSize);
    const int exitCode = vm.execute(bytecode);
    return exitCode;
}
