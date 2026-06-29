#!/usr/bin/env python3
"""
Shell-VM Android NDK build script
Build Android executables with the Obfuscator-LLVM NDK
License: GPL-3.0-or-later
"""

import os
import sys
import shutil
import subprocess
import argparse
from pathlib import Path

if os.name == "nt":
    os.system("chcp 65001 > nul")
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")

# ============================================================================
# Configuration
# ============================================================================

# Project root
PROJECT_ROOT = Path(__file__).parent.resolve()

# NDK path
NDK_PATH = Path(r"D:\cpp\obfuscator-ollvm-21.x\android-ndk-r30-beta1-windows")

# Source files
SOURCES = [
    "src/vm_core.cpp",
    "src/compiler.cpp",
    "src/runtime.cpp",
]

# Include directories
INCLUDE_DIRS = ["include"]

# Target ABIs
TARGET_ABIS = ["arm64-v8a"]

# Compiler flags
CFLAGS = [
    "-std=c++17",
    "-O2",
    "-Wall",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-Wno-unused-but-set-variable",
    "-DNDEBUG",
    "-DANDROID",
    "-fPIE",
    "-fvisibility=hidden",
]

# Linker flags
LDFLAGS = [
    "-pie",
    "-llog",
]

# Bundle sources
EMBED_TOOL_TARGET = "embed_bytecode"
EMBED_RUNNER_SOURCE = "embedded_runner.cpp"
SAFE_SCRIPT = "test.sh"
BUNDLE_SYMBOL = "kEmbeddedBytecode"


# ============================================================================
# Helpers
# ============================================================================

def find_ndk_build():
    """Find the ndk-build tool."""
    candidates = [
        NDK_PATH / "ndk-build.cmd",
        NDK_PATH / "ndk-build",
        NDK_PATH / "ndk-build.py",
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    return None


def find_clang():
    """Find clang inside the NDK."""
    # NDK layout: toolchains/llvm/prebuilt/<host>/bin/
    prebuilt = NDK_PATH / "toolchains" / "llvm" / "prebuilt"
    if not prebuilt.exists():
        return None, None

    # Detect host directory
    hosts = list(prebuilt.iterdir())
    if not hosts:
        return None, None

    host_dir = hosts[0]
    bin_dir = host_dir / "bin"

    clang = bin_dir / "clang++.cmd"
    if not clang.exists():
        clang = bin_dir / "clang++"
    if not clang.exists():
        clang = bin_dir / "clang++.exe"

    return str(clang), str(bin_dir)


def run_command(cmd, cwd=None, env=None):
    """Run a command and stream its output."""
    print(f"[RUN] {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    result = subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        shell=isinstance(cmd, str),
        text=True,
    )
    return result.returncode == 0


def find_host_tool(build_dir, tool_name):
    """Find a host-side executable produced by CMake."""
    exe_name = f"{tool_name}.exe" if os.name == "nt" else tool_name
    candidates = [
        build_dir / exe_name,
        build_dir / "Debug" / exe_name,
        build_dir / "Release" / exe_name,
        build_dir / "RelWithDebInfo" / exe_name,
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def ensure_host_embed_tool():
    """Configure and build the host-side embed tool."""
    host_build_dir = PROJECT_ROOT / "build"
    cache_file = host_build_dir / "CMakeCache.txt"

    if not cache_file.exists():
        if not run_command(["cmake", "-S", str(PROJECT_ROOT), "-B", str(host_build_dir)]):
            return None

    if not run_command(["cmake", "--build", str(host_build_dir), "--config", "Debug", "--target", EMBED_TOOL_TARGET]):
        return None

    return find_host_tool(host_build_dir, EMBED_TOOL_TARGET)


def generate_embedded_bytecode_source(build_dir, script_path):
    """Generate a C++ source file containing embedded bytecode."""
    tool_path = ensure_host_embed_tool()
    if not tool_path:
        print("[ERROR] Failed to build the host embed tool")
        return None

    script_file = Path(script_path)
    if not script_file.is_absolute():
        script_file = (PROJECT_ROOT / script_file).resolve()
    if not script_file.exists():
        print(f"[ERROR] Script file does not exist: {script_file}")
        return None

    generated_dir = build_dir / "generated"
    generated_dir.mkdir(parents=True, exist_ok=True)
    output_cpp = generated_dir / "embedded_bytecode.cpp"

    cmd = [str(tool_path), str(script_file), str(output_cpp), BUNDLE_SYMBOL]
    if not run_command(cmd, cwd=str(PROJECT_ROOT)):
        print("[ERROR] Failed to generate embedded bytecode source")
        return None

    print(f"[GENERATE] {output_cpp}")
    return output_cpp


def resolve_output_name(script_path):
    """Use the input script filename as the final bundle name."""
    script_file = Path(script_path)
    name = script_file.name
    return name or "shellvm_bundle"


def cleanup_object_files(build_dir):
    """Delete intermediate object files after a successful build."""
    for obj_file in build_dir.rglob("*.o"):
        try:
            obj_file.unlink()
        except OSError:
            pass


def prepare_build_dir(build_dir):
    """Prepare the build directory."""
    if build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True)
    print(f"[PREPARE] Build directory: {build_dir}")


def generate_android_mk(build_dir, embedded_cpp, output_name):
    """Generate Android.mk."""
    android_mk = build_dir / "jni" / "Android.mk"
    android_mk.parent.mkdir(parents=True, exist_ok=True)

    content = f"""LOCAL_PATH := $(call my-dir)

# Shell-VM static library
include $(CLEAR_VARS)
LOCAL_MODULE := shellvm
LOCAL_SRC_FILES := {PROJECT_ROOT / 'src' / 'vm_core.cpp'} \\
                   {PROJECT_ROOT / 'src' / 'compiler.cpp'} \\
                   {PROJECT_ROOT / 'src' / 'runtime.cpp'}
LOCAL_C_INCLUDES := {PROJECT_ROOT / 'include'}
LOCAL_CFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-unused-variable -DNDEBUG -DANDROID
LOCAL_CPPFLAGS := -std=c++17 -O2 -fno-rtti
include $(BUILD_STATIC_LIBRARY)

# Shell-VM bundled runner
include $(CLEAR_VARS)
LOCAL_MODULE := {output_name}
LOCAL_SRC_FILES := {PROJECT_ROOT / EMBED_RUNNER_SOURCE} \\
                   {embedded_cpp}
LOCAL_C_INCLUDES := {PROJECT_ROOT / 'include'}
LOCAL_STATIC_LIBRARIES := shellvm
LOCAL_CFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter -DNDEBUG -DANDROID
LOCAL_CPPFLAGS := -std=c++17 -O2 -fno-rtti
LOCAL_LDLIBS := -llog
include $(BUILD_EXECUTABLE)
"""
    android_mk.write_text(content, encoding="utf-8")
    print(f"[GENERATE] {android_mk}")


def generate_application_mk(build_dir, abis=None):
    """Generate Application.mk."""
    app_mk = build_dir / "jni" / "Application.mk"
    app_mk.parent.mkdir(parents=True, exist_ok=True)

    abi_list = " ".join(abis or TARGET_ABIS)
    content = f"""APP_ABI := {abi_list}
APP_PLATFORM := android-21
APP_STL := c++_static
APP_CFLAGS := -std=c++17 -O2 -DNDEBUG -DANDROID -fPIE -fvisibility=hidden
APP_CPPFLAGS := -std=c++17 -O2 -fno-rtti
APP_LDFLAGS := -pie
APP_PIE := true
"""
    app_mk.write_text(content, encoding="utf-8")
    print(f"[GENERATE] {app_mk}")


def build_with_ndk_build(build_dir, embedded_cpp, output_name, abis=None):
    """Build with ndk-build."""
    ndk_build = find_ndk_build()
    if not ndk_build:
        print(f"[ERROR] ndk-build not found, NDK path: {NDK_PATH}")
        return False

    print(f"[INFO] Using NDK: {NDK_PATH}")
    print(f"[INFO] Using ndk-build: {ndk_build}")

    generate_android_mk(build_dir, embedded_cpp, output_name)
    generate_application_mk(build_dir, abis)

    cmd = [
        ndk_build,
        f"NDK_PROJECT_PATH={build_dir}",
        f"APP_BUILD_SCRIPT={build_dir / 'jni' / 'Android.mk'}",
        f"NDK_APPLICATION_MK={build_dir / 'jni' / 'Application.mk'}",
    ]

    return run_command(cmd, cwd=str(build_dir))


def build_with_clang_directly(build_dir, embedded_cpp, output_name, abis=None):
    """Build directly with clang++ when ndk-build is unavailable."""
    clang, bin_dir = find_clang()
    if not clang:
        print("[ERROR] clang++ not found in NDK")
        return False

    print(f"[INFO] Using clang++: {clang}")

    include_flags = [f"-I{PROJECT_ROOT / 'include'}"]
    cflags = CFLAGS + include_flags

    success = True
    for abi in (abis or TARGET_ABIS):
        print(f"\n[BUILD] Target ABI: {abi}")

        # Select target triple
        if abi == "arm64-v8a":
            target = "aarch64-linux-android21"
        elif abi == "armeabi-v7a":
            target = "armv7a-linux-androideabi21"
        elif abi == "x86":
            target = "i686-linux-android21"
        elif abi == "x86_64":
            target = "x86_64-linux-android21"
        else:
            print(f"[WARN] Unknown ABI: {abi}, skipped")
            continue

        abi_dir = build_dir / "libs" / abi
        abi_dir.mkdir(parents=True, exist_ok=True)

        # Compile sources to object files
        obj_files = []
        for src in SOURCES:
            src_path = PROJECT_ROOT / src
            obj_name = Path(src).stem + ".o"
            obj_path = abi_dir / obj_name

            cmd = [clang, f"--target={target}"] + cflags + ["-c", str(src_path), "-o", str(obj_path)]
            if not run_command(cmd):
                print(f"[ERROR] Failed to compile: {src}")
                success = False
                break
            obj_files.append(str(obj_path))

        if not success:
            continue

        runner_sources = [
            PROJECT_ROOT / EMBED_RUNNER_SOURCE,
            embedded_cpp,
        ]
        for runner_src in runner_sources:
            obj_name = Path(runner_src).stem + ".o"
            obj_path = abi_dir / obj_name
            cmd = [clang, f"--target={target}"] + cflags + ["-c", str(runner_src), "-o", str(obj_path)]
            if not run_command(cmd):
                print(f"[ERROR] Failed to compile bundle source: {runner_src}")
                success = False
                break
            obj_files.append(str(obj_path))

        if not success:
            continue

        # Link
        output = abi_dir / output_name
        link_flags = [f"--target={target}", "-pie", "-llog", "-static-libstdc++"]
        cmd = [clang] + link_flags + obj_files + ["-o", str(output)]
        if not run_command(cmd):
            print(f"[ERROR] Link failed: {abi}")
            success = False
            continue

        for obj_file in obj_files:
            try:
                Path(obj_file).unlink(missing_ok=True)
            except OSError:
                pass

        print(f"[OK] {abi}: {output}")

    return success


def build(abus=None, use_ndk_build=True, script_path=SAFE_SCRIPT):
    """Main build entry."""
    print("=" * 60)
    print("Shell-VM Android NDK Build")
    print("=" * 60)
    print()

    if not NDK_PATH.exists():
        print(f"[ERROR] NDK path does not exist: {NDK_PATH}")
        return False

    build_dir = PROJECT_ROOT / "build_android"
    prepare_build_dir(build_dir)
    embedded_cpp = generate_embedded_bytecode_source(build_dir, script_path)
    if not embedded_cpp:
        return False

    output_name = resolve_output_name(script_path)

    if use_ndk_build:
        result = build_with_ndk_build(build_dir, embedded_cpp, output_name, abus)
    else:
        result = build_with_clang_directly(build_dir, embedded_cpp, output_name, abus)

    if result:
        cleanup_object_files(build_dir)
        print()
        print("=" * 60)
        print("[DONE] Build succeeded")
        print("=" * 60)
        print()
        print("Generated bundles:")
        for abi in (abus or TARGET_ABIS):
            exe = build_dir / "libs" / abi / output_name
            if exe.exists():
                size = exe.stat().st_size
                print(f"  - {exe} ({size:,} bytes)")
        print()
        selected_abi = next(
            (abi for abi in (abus or TARGET_ABIS) if (build_dir / "libs" / abi / output_name).exists()),
            (abus or TARGET_ABIS)[0],
        )
        script_name = Path(script_path).name
        print("Push to device and test:")
        print(f'  adb push "build_android/libs/{selected_abi}/{output_name}" /data/local/tmp/')
        print('  adb shell')
        print('  cd /data/local/tmp')
        print(f'  chmod +x "{output_name}"')
        print(f'  ./"{output_name}"')
        print(f'  # embedded script: {script_name}')
    else:
        print()
        print("[FAILED] Build failed, please check the error output")

    return result


# ============================================================================
# Main entry
# ============================================================================

def main():
    global NDK_PATH

    parser = argparse.ArgumentParser(description="Shell-VM Android NDK build script")
    parser.add_argument(
        "--abi",
        nargs="+",
        default=TARGET_ABIS,
        help=f"Target ABI list (default: {TARGET_ABIS})",
    )
    parser.add_argument(
        "--ndk-build",
        action="store_true",
        default=True,
        help="Build with ndk-build (default)",
    )
    parser.add_argument(
        "--clang",
        action="store_true",
        help="Build directly with clang++",
    )
    parser.add_argument(
        "--ndk-path",
        type=str,
        default=str(NDK_PATH),
        help=f"NDK path (default: {NDK_PATH})",
    )
    parser.add_argument(
        "--script",
        type=str,
        default=SAFE_SCRIPT,
        help=f"Script to bundle into the executable (default: {SAFE_SCRIPT})",
    )

    args = parser.parse_args()

    # Update NDK path
    NDK_PATH = Path(args.ndk_path)

    # Select build mode
    use_ndk_build = not args.clang

    success = build(args.abi, use_ndk_build, args.script)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
