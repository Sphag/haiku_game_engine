#!/usr/bin/env python3
# Maintainer tool: downloads pinned upstream commit archives, extracts the listed
# paths into third_party/, and records a SHA-256 for every vendored file in
# third_party/manifest.json. CMake never runs this script; configure and build
# only validate what is already vendored (cmake/ValidateVendor.cmake).
#
#   python3 -I tools/vendor/vendor.py --download-dir <new empty dir> [--only NAME ...]
#
# Include and exclude entries are fnmatch patterns over paths relative to the
# upstream repository root. Each include pattern must match at least one file,
# so a wrong pattern fails loudly instead of silently vendoring nothing.

import argparse
import fnmatch
import hashlib
import json
import pathlib
import shutil
import sys
import tarfile
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
VENDOR_DIR = REPO_ROOT / "third_party"
MANIFEST = VENDOR_DIR / "manifest.json"

# Dependencies the PRD forbids outright (FR-02 and the dependency catalog).
PROHIBITED = ["EnTT", "Flecs", "SDL", "GLFW", "sol2"]

# Native system ABIs and toolchains are used but never vendored as sources.
SYSTEM_ABI = [
    {"name": "Windows SDK", "reason": "d3d12.h, dxgi, Win32, Raw Input, XInput; OS headers and import libraries"},
    {"name": "GPU drivers and Vulkan ICD", "reason": "installed on the system; Vulkan headers and loader sources are vendored"},
    {"name": "X11, XInput2, XIM, evdev, glibc", "reason": "Linux system ABI for windowing, input, and the C library"},
    {"name": "Compilers and standard libraries", "reason": "MSVC, Clang, GCC, libc++/libstdc++, and their runtimes"},
    {"name": "Python 3 interpreter", "reason": "build-time code generators for Vulkan-Loader, Vulkan-ValidationLayers, and Vulkan-Utility-Libraries; a system tool, not vendored"},
]

SPECS = [
    {
        "name": "Dear ImGui", "version": "docking d88f238d", "repo": "ocornut/imgui",
        "sha": "d88f238d1d363ef387382b2e29ad12cf96f826ac", "root": "imgui",
        "license": "MIT", "licenseFile": "LICENSE.txt", "dependsOn": [],
        "purpose": "Editor UI (docking branch); renderer backends only, the platform/input adapter is in-house",
        "include": [
            "imgui.h", "imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
            "imgui_internal.h", "imconfig.h", "imstb_rectpack.h", "imstb_textedit.h",
            "imstb_truetype.h", "LICENSE.txt",
            "backends/imgui_impl_dx12.*", "backends/imgui_impl_vulkan.*",
        ],
        "exclude": [],
    },
    {
        "name": "Jolt Physics", "version": "master 01adafba", "repo": "jrouwe/JoltPhysics",
        "sha": "01adafba08bddb650046a822983c52aa734fcadb", "root": "jolt",
        "license": "MIT", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "Physics backend (FR-18, US-052)",
        "include": ["Jolt/*", "LICENSE"],
        "exclude": [],
    },
    {
        "name": "Lua", "version": "5.4.7", "repo": "lua/lua",
        "sha": "1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3", "root": "lua",
        "license": "MIT", "licenseFile": "README.md", "dependsOn": [],
        "purpose": "Lua VM with in-house C API bindings (no sol2); the lua and luac command-line programs, the test hooks, and onelua are not vendored",
        "include": ["*.c", "*.h", "README.md"],
        "exclude": ["lua.c", "luac.c", "ltests.*", "onelua.c", "testes/*"],
    },
    {
        "name": "GLM", "version": "1.0.1", "repo": "g-truc/glm",
        "sha": "0af55ccecd98d4e5a8d1fad7de25ba429d60e863", "root": "glm",
        "license": "MIT", "licenseFile": "copying.txt", "dependsOn": [],
        "purpose": "Math primitives; in-house wrappers define engine conventions",
        "include": ["glm/*", "copying.txt"],
        "exclude": [],
    },
    {
        "name": "stb", "version": "master a7d4c217", "repo": "nothings/stb",
        "sha": "a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d", "root": "stb",
        "license": "MIT OR Unlicense", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "Image decoding, font rasterization, Ogg Vorbis decoding; stb_image_resize2 is only available under deprecated/ at this pin and is vendored from there",
        "include": ["stb_image.h", "deprecated/stb_image_resize2.h", "stb_truetype.h", "stb_vorbis.c", "LICENSE"],
        "exclude": [],
    },
    {
        "name": "bc7enc_rdo", "version": "master b9438627", "repo": "richgel999/bc7enc_rdo",
        "sha": "b9438627eef73a1157e84201b6fa6eb2ffd6d9f0", "root": "bc7enc_rdo",
        "license": "MIT", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "BC7 encoding; the BC5 encoder and format wrapper live in the converter",
        "include": ["*.cpp", "*.h", "LICENSE"],
        "exclude": ["test.cpp", "lodepng.*", "*/*"],
    },
    {
        "name": "miniaudio", "version": "master 9634bedb", "repo": "mackron/miniaudio",
        "sha": "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d", "root": "miniaudio",
        "license": "MIT-0 OR Unlicense", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "Audio device and mixing adapter",
        "include": ["miniaudio.h", "LICENSE"],
        "exclude": [],
    },
    {
        "name": "nlohmann/json", "version": "3.11.3", "repo": "nlohmann/json",
        "sha": "9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03", "root": "nlohmann_json",
        "license": "MIT", "licenseFile": "LICENSE.MIT", "dependsOn": [],
        "purpose": "Authoring formats (single-header amalgamation)",
        "include": ["single_include/nlohmann/json.hpp", "LICENSE.MIT"],
        "exclude": [],
    },
    {
        "name": "zstd", "version": "1.5.6", "repo": "facebook/zstd",
        "sha": "794ea1b0afca0f020f4e57b6732332231fb23c70", "root": "zstd",
        "license": "BSD-3-Clause OR GPL-2.0-only", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "Pack compression; the whole lib/ tree (Windows DLL projects excluded)",
        "include": ["lib/*", "LICENSE", "COPYING"],
        "exclude": ["lib/dll/*"],
    },
    {
        "name": "ImGuizmo", "version": "master 105e4fde", "repo": "CedricGuillemet/ImGuizmo",
        "sha": "105e4fde45e4e74aae8973d98750820ef7aca7e2", "root": "imguizmo",
        "license": "MIT", "licenseFile": "LICENSE", "dependsOn": ["Dear ImGui"],
        "purpose": "Transform gizmos",
        "include": ["src/ImGuizmo.h", "src/ImGuizmo.cpp", "LICENSE"],
        "exclude": [],
    },
    {
        "name": "imgui-node-editor", "version": "master 1e0e494f", "repo": "thedmd/imgui-node-editor",
        "sha": "1e0e494f1a86885eb332ca518543108ccd88d13e", "root": "imgui_node_editor",
        "license": "MIT", "licenseFile": "LICENSE", "dependsOn": ["Dear ImGui"],
        "purpose": "Graph canvas for the material and animation graph editors",
        "include": ["*.h", "*.cpp", "*.inl", "LICENSE"],
        "exclude": ["*/*"],
    },
    {
        "name": "D3D12MemoryAllocator", "version": "master b73737b0", "repo": "GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator",
        "sha": "b73737b0b4cde0b5ab80d685209cdc0d81cf878e", "root": "d3d12ma",
        "license": "MIT", "licenseFile": "LICENSE.txt", "dependsOn": [],
        "purpose": "GPU allocations behind the DX12 RHI; requires the Windows SDK d3d12.h",
        "include": ["include/D3D12MemAlloc.h", "src/D3D12MemAlloc.cpp", "LICENSE.txt"],
        "exclude": [],
    },
    {
        "name": "VulkanMemoryAllocator", "version": "3.1.0", "repo": "GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator",
        "sha": "009ecd192c1289c7529bff248a16cfe896254816", "root": "vma",
        "license": "MIT", "licenseFile": "LICENSE.txt", "dependsOn": ["Vulkan-Headers"],
        "purpose": "GPU allocations behind the Vulkan RHI",
        "include": ["include/vk_mem_alloc.h", "LICENSE.txt"],
        "exclude": [],
    },
    {
        "name": "Vulkan-Headers", "version": "1.4.363 (vulkan-sdk-1.4.363.0)", "repo": "KhronosGroup/Vulkan-Headers",
        "sha": "6802bb4733b63ed5efd3adb308a6c885ef180ea1", "root": "vulkan_headers",
        "license": "Apache-2.0 OR MIT", "licenseFile": "LICENSE.md", "dependsOn": [],
        "purpose": "Vulkan API headers for the Vulkan RHI and the Vulkan memory allocator; registry/ (vk.xml and generators) is needed by the Vulkan loader and validation layer builds",
        "include": ["include/*", "registry/*", "LICENSE.md"],
        "exclude": [],
    },
    {
        "name": "Assimp", "version": "5.4.3", "repo": "assimp/assimp",
        "sha": "c35200e38ea8f058812b83de2ef32c6093b0ece2", "root": "assimp",
        "license": "BSD-3-Clause", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "Model and animation import in the converter; the root CMakeLists.txt and cmake-modules drive the build of code/ and the bundled contrib sources (zlib, unzip, rapidjson, pugixml, and others); draco and gtest excluded",
        "include": ["CMakeLists.txt", "assimp.pc.in", "cmake-modules/*", "code/*", "include/*", "contrib/*", "LICENSE"],
        "exclude": ["contrib/draco/*", "contrib/gtest/*"],
    },
    {
        "name": "meshoptimizer", "version": "master f0a684f1", "repo": "zeux/meshoptimizer",
        "sha": "f0a684f11ead064e176775c9b796f82a55910e92", "root": "meshoptimizer",
        "license": "MIT", "licenseFile": "LICENSE.md", "dependsOn": [],
        "purpose": "Offline LOD and meshlet generation in the converter; the library sources only (demo, gltf, js, and tools are not vendored)",
        "include": ["src/*", "LICENSE.md"],
        "exclude": [],
    },
    {
        "name": "SPIRV-Headers", "version": "known good for Vulkan-ValidationLayers 1.4.363", "repo": "KhronosGroup/SPIRV-Headers",
        "sha": "496543121ce6419f23d6fa5d7194ba66c36212d2", "root": "spirv_headers",
        "license": "MIT", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "SPIR-V headers for SPIRV-Tools, Vulkan-ValidationLayers, and DirectXShaderCompiler; passed to their builds as SPIRV_HEADER_DIR or DXC_SPIRV_HEADERS_DIR",
        "include": ["CMakeLists.txt", "cmake/*", "include/*", "LICENSE"],
        "exclude": [],
    },
    {
        "name": "SPIRV-Tools", "version": "known good for Vulkan-ValidationLayers 1.4.363", "repo": "KhronosGroup/SPIRV-Tools",
        "sha": "ef96ed763b43b59b33b31b362f09a02b729fa1c9", "root": "spirv_tools",
        "license": "Apache-2.0", "licenseFile": "LICENSE", "dependsOn": ["SPIRV-Headers"],
        "purpose": "SPIR-V optimizer and validator library. Its top-level CMake adds tools/, test/, and examples/ unconditionally, so those directories are vendored with their CMake files only; test/CMakeLists.txt returns early when SPIRV_SKIP_TESTS is ON (implied by SPIRV_SKIP_EXECUTABLES) and the SPIRV-Tools unit tests are not vendored",
        "include": ["CHANGES", "CMakeLists.txt", "cmake/*", "examples/*", "external/*", "include/*", "source/*", "test/CMakeLists.txt", "tools/*", "utils/*", "LICENSE"],
        "exclude": [],
    },
    {
        "name": "Vulkan-Utility-Libraries", "version": "known good for Vulkan-ValidationLayers 1.4.363", "repo": "KhronosGroup/Vulkan-Utility-Libraries",
        "sha": "d498a40c2e1fa6ce49667e8aaa4472c0db623cda", "root": "vulkan_utility_libraries",
        "license": "Apache-2.0", "licenseFile": "LICENSE.md", "dependsOn": ["Vulkan-Headers"],
        "purpose": "Vulkan helper libraries used by the validation layers; scripts/gn and tests are not vendored",
        "include": ["CMakeLists.txt", "include/*", "scripts/*", "src/*", "LICENSE.md"],
        "exclude": ["scripts/gn/*", "scripts/update_deps.py"],
    },
    {
        "name": "Vulkan-Loader", "version": "1.4.363 (vulkan-sdk-1.4.363.0)", "repo": "KhronosGroup/Vulkan-Loader",
        "sha": "fa218055ff4948b159f31d43f800c808a13d51df", "root": "vulkan_loader",
        "license": "Apache-2.0 (per-file exceptions noted in source)", "licenseFile": "LICENSE.txt", "dependsOn": ["Vulkan-Headers"],
        "purpose": "Vulkan loader for the Vulkan RHI on Windows and Linux; code generators under scripts/ run with the system Python 3; packaging (gn, qnx) and tests are not vendored",
        "include": ["CMakeLists.txt", "loader/*", "scripts/*", "LICENSE.txt"],
        "exclude": ["scripts/gn/*", "scripts/qnx/*", "scripts/update_deps.py"],
    },
    {
        "name": "Vulkan-ValidationLayers", "version": "1.4.363 (vulkan-sdk-1.4.363.0)", "repo": "KhronosGroup/Vulkan-ValidationLayers",
        "sha": "15c64e73db67e27ff0215708fbe48684a35c6827", "root": "vulkan_validation_layers",
        "license": "Apache-2.0 (per-file exceptions noted in source)", "licenseFile": "LICENSE.txt",
        "dependsOn": ["Vulkan-Headers", "Vulkan-Utility-Libraries", "SPIRV-Headers", "SPIRV-Tools"],
        "purpose": "Khronos validation layer for debug and sanitizer-style GPU runs; its own bundled external/ headers are included. mimalloc (optional, USE_MIMALLOC=OFF), glslang and googletest (tests only) are not vendored",
        "include": ["CMakeLists.txt", "layers/*", "scripts/*", "LICENSE.txt"],
        "exclude": ["scripts/gn/*", "scripts/update_deps.py"],
    },
    {
        "name": "DirectX-Headers", "version": "pinned by DirectXShaderCompiler v1.10.2605.37", "repo": "microsoft/DirectX-Headers",
        "sha": "980971e835876dc0cde415e8f9bc646e64667bf7", "root": "directx_headers",
        "license": "MIT", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "D3D12 headers used by DirectXShaderCompiler on non-Windows hosts; Windows builds use the Windows SDK headers",
        "include": ["include/*", "src/*", "LICENSE"],
        "exclude": [],
    },
    {
        "name": "DirectXShaderCompiler", "version": "v1.10.2605.37", "repo": "microsoft/DirectXShaderCompiler",
        "sha": "c4d8f4f99aa221da58cd540bf2099a2889632ab6", "root": "dxc",
        "license": "NCSA (LLVM Release License)", "licenseFile": "LICENSE.TXT",
        "dependsOn": ["DirectX-Headers", "SPIRV-Headers", "SPIRV-Tools"],
        "purpose": "HLSL to DXIL and SPIR-V compiler (US-003 builds the host toolchain). The in-tree LLVM/Clang sources it needs are included; test, unittest, example, and docs trees are not. Its DXC_SPIRV_* paths point at the shared SPIRV-Headers/SPIRV-Tools roots",
        "include": [
            "CMakeLists.txt", "LICENSE.TXT", "LLVMBuild.txt", "ThirdPartyNotices.txt",
            "cmake/*", "docs/LLVMBuild.txt", "examples/LLVMBuild.txt", "external/CMakeLists.txt",
            "external/GTestConfig.cmake", "include/*", "lib/*", "projects/CMakeLists.txt",
            "projects/LLVMBuild.txt", "projects/include/*",
            "resources/*", "tools/*", "utils/*",
        ],
        "exclude": [
            "tools/*/test/*", "tools/*/unittests/*", "tools/*/docs/*", "tools/*/examples/*",
            "utils/lit/*", "utils/llvm-lit/*", "utils/unittest/*",
        ],
    },
    {
        "name": "FidelityFX FSR 2", "version": "master 1680d1ed", "repo": "GPUOpen-Effects/FidelityFX-FSR2",
        "sha": "1680d1edd5c034f88ebbbb793d8b88f8842cf804", "root": "fsr2",
        "license": "MIT", "licenseFile": "LICENSE.txt", "dependsOn": [],
        "purpose": "Temporal upscaling for the full_1080 profile (FSR 2 Quality), with the DX12 and Vulkan backends and their shader sources; sample applications and prebuilt tools/sc binaries are not vendored",
        "include": ["LICENSE.txt", "src/ffx-fsr2-api/*"],
        "exclude": [],
    },
    {
        "name": "Roboto", "version": "2.x (38062f4b)", "repo": "googlefonts/roboto-2",
        "sha": "38062f4b4a0be4346d07a928408da21602545e9e", "root": "roboto",
        "license": "Apache-2.0", "licenseFile": "LICENSE", "dependsOn": [],
        "purpose": "Licensed font with Cyrillic coverage for the editor and runtime UI (regular weight only)",
        "include": ["LICENSE", "src/hinted/Roboto-Regular.ttf"],
        "exclude": [],
    },
]


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_archive(url, dest, attempts=3):
    if not dest.exists():
        partial = dest.with_name(dest.name + ".part")
        for attempt in range(1, attempts + 1):
            try:
                request = urllib.request.Request(url, headers={"User-Agent": "open-world-engine-vendor"})
                with urllib.request.urlopen(request, timeout=300) as response, partial.open("wb") as out:
                    shutil.copyfileobj(response, out)
                partial.replace(dest)
                break
            except OSError as error:
                if attempt == attempts:
                    raise SystemExit(f"download failed after {attempts} attempts: {url}: {error}")
                print(f"retrying {url}: {error}", file=sys.stderr)
    return sha256_file(dest)


def is_selected(rel, spec, hit_patterns):
    if any(fnmatch.fnmatchcase(rel, pattern) for pattern in spec["exclude"]):
        return False
    hits = [pattern for pattern in spec["include"] if fnmatch.fnmatchcase(rel, pattern)]
    hit_patterns.update(hits)
    return bool(hits)


def vendor_one(spec, archive):
    root_dir = VENDOR_DIR / spec["root"]
    if root_dir.exists():
        shutil.rmtree(root_dir)
    hit_patterns = set()
    files = []
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar:
            if not member.isfile():
                continue
            parts = member.name.split("/", 1)
            if len(parts) != 2:
                continue
            rel = parts[1]
            segments = rel.split("/")
            if ".." in segments or any(segment.startswith(".") for segment in segments):
                continue
            if not is_selected(rel, spec, hit_patterns):
                continue
            dest = root_dir / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            source = tar.extractfile(member)
            with source, dest.open("wb") as out:
                shutil.copyfileobj(source, out)
            files.append({"path": f"{spec['root']}/{rel}", "sha256": sha256_file(dest)})

    unmatched = [pattern for pattern in spec["include"] if pattern not in hit_patterns]
    if unmatched:
        raise SystemExit(f"{spec['name']}: include patterns matched no files: {unmatched}")
    files.sort(key=lambda entry: entry["path"])
    return files


def manifest_entry(spec, archive_url, archive_sha, files):
    return {
        "name": spec["name"],
        "version": spec["version"],
        "url": f"https://github.com/{spec['repo']}",
        "revision": spec["sha"],
        "license": spec["license"],
        "licenseFile": f"{spec['root']}/{spec['licenseFile']}",
        "root": spec["root"],
        "archive": {"url": archive_url, "sha256": archive_sha},
        "purpose": spec["purpose"],
        "patches": [],
        "dependsOn": spec["dependsOn"],
        "files": files,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--download-dir", required=True, type=pathlib.Path,
                        help="directory for downloaded archives; use a new empty directory outside the repo")
    parser.add_argument("--only", nargs="+", metavar="NAME", help="vendor only these dependencies")
    args = parser.parse_args()

    for spec in SPECS:
        for word in PROHIBITED:
            if word.lower() in spec["name"].lower():
                raise SystemExit(f"prohibited dependency: {spec['name']}")
    selected_specs = [spec for spec in SPECS if not args.only or spec["name"] in args.only]
    if args.only and len(selected_specs) != len(args.only):
        raise SystemExit(f"unknown dependency name in --only: {args.only}")

    args.download_dir.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    updated = {}
    for spec in selected_specs:
        url = f"https://codeload.github.com/{spec['repo']}/tar.gz/{spec['sha']}"
        archive = args.download_dir / f"{spec['root']}-{spec['sha']}.tar.gz"
        print(f"{spec['name']}: fetching {url}", file=sys.stderr)
        archive_sha = fetch_archive(url, archive)
        files = vendor_one(spec, archive)
        print(f"{spec['name']}: {len(files)} files", file=sys.stderr)
        updated[spec["name"]] = manifest_entry(spec, url, archive_sha, files)

    known = {dep["name"] for dep in manifest["dependencies"]} | set(updated)
    for spec in SPECS:
        for dependency in spec["dependsOn"]:
            if dependency not in known:
                raise SystemExit(f"{spec['name']}: dependsOn '{dependency}' is not a manifest dependency")

    merged = [updated.pop(dep["name"], dep) for dep in manifest["dependencies"]]
    merged.extend(entry for entry in updated.values())
    manifest["dependencies"] = merged
    manifest["prohibitedDependencies"] = PROHIBITED
    manifest["systemAbiExceptions"] = SYSTEM_ABI
    manifest["generator"] = "tools/vendor/vendor.py"
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {MANIFEST}", file=sys.stderr)


if __name__ == "__main__":
    main()
