#include "include/shell_vm.h"
#include "include/vm_core.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern const std::uint8_t kEmbeddedBytecode[];
extern const std::size_t kEmbeddedBytecodeSize;

int main() {
    using namespace shellvm;

#ifdef _WIN32
    _putenv_s("SHELLVM_TRACE", "");
#else
    unsetenv("SHELLVM_TRACE");
#endif

    VMConfig config;
    config.enableSandbox = false;
    config.enableFileAccess = true;
    config.timeout = 60000;
    config.maxStackSize = 8192;

    VM vm(config);
    Bytecode bytecode(kEmbeddedBytecode, kEmbeddedBytecode + kEmbeddedBytecodeSize);
    const int exitCode = vm.execute(bytecode);
    return exitCode;
}
