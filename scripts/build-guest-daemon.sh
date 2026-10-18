#!/usr/bin/env bash
# =============================================================================
# Fang Android Guest Daemon Cross-Compilation Pipeline Entry Point (POSIX Shell)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

NDK_PATH=""
ABI="x86_64"
API_LEVEL="26"
CONFIG="Release"
OUT_DIR="${REPO_ROOT}/out/staging"
MIN_NDK_VERSION="25"
VALIDATE_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ndk-path)
            NDK_PATH="$2"
            shift 2
            ;;
        --abi)
            ABI="$2"
            shift 2
            ;;
        --api-level)
            API_LEVEL="$2"
            shift 2
            ;;
        --configuration)
            CONFIG="$2"
            shift 2
            ;;
        --out-dir)
            OUT_DIR="$2"
            shift 2
            ;;
        --min-ndk-version)
            MIN_NDK_VERSION="$2"
            shift 2
            ;;
        --validate-only)
            VALIDATE_ONLY=1
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "${NDK_PATH}" ]]; then
    if [[ -n "${ANDROID_NDK_ROOT:-}" && -d "${ANDROID_NDK_ROOT}" ]]; then
        NDK_PATH="${ANDROID_NDK_ROOT}"
    elif [[ -n "${ANDROID_NDK_HOME:-}" && -d "${ANDROID_NDK_HOME}" ]]; then
        NDK_PATH="${ANDROID_NDK_HOME}"
    elif [[ -n "${NDK_ROOT:-}" && -d "${NDK_ROOT}" ]]; then
        NDK_PATH="${NDK_ROOT}"
    fi
fi

if [[ -z "${NDK_PATH}" || ! -d "${NDK_PATH}" ]]; then
    echo "ERROR: Android NDK not found. Set ANDROID_NDK_ROOT or provide --ndk-path." >&2
    exit 1
fi

SOURCE_PROPS="${NDK_PATH}/source.properties"
if [[ ! -f "${SOURCE_PROPS}" ]]; then
    echo "ERROR: Invalid Android NDK at '${NDK_PATH}': missing source.properties." >&2
    exit 1
fi

NDK_REV=$(grep -E '^Pkg\.Revision' "${SOURCE_PROPS}" | awk -F'=' '{print $2}' | tr -d ' \r\n')
NDK_MAJOR=$(echo "${NDK_REV}" | awk -F'.' '{print $1}')

if [[ "${NDK_MAJOR}" -lt "${MIN_NDK_VERSION}" ]]; then
    echo "ERROR: NDK version r${NDK_REV} is below required minimum r${MIN_NDK_VERSION}." >&2
    exit 1
fi

LLVM_PREBUILT="${NDK_PATH}/toolchains/llvm/prebuilt"
if [[ ! -d "${LLVM_PREBUILT}" ]]; then
    echo "ERROR: Toolchain LLVM directory missing at '${LLVM_PREBUILT}'." >&2
    exit 1
fi

HOST_DIR=$(find "${LLVM_PREBUILT}" -mindepth 1 -maxdepth 1 -type d | head -n 1)
CLANG_CXX="${HOST_DIR}/bin/clang++"

if [[ ! -x "${CLANG_CXX}" ]]; then
    echo "ERROR: Clang++ compiler not found or not executable at '${CLANG_CXX}'." >&2
    exit 1
fi

echo "Discovered Android NDK: ${NDK_PATH} (r${NDK_REV})"
echo "Target Clang++: ${CLANG_CXX}"

if [[ "${VALIDATE_ONLY}" -eq 1 ]]; then
    echo "Toolchain validation succeeded."
    exit 0
fi

case "${ABI}" in
    x86_64)
        TARGET_TRIPLE="x86_64-linux-android${API_LEVEL}"
        EXPECTED_MACHINE="62" # 0x3E
        ;;
    arm64-v8a)
        TARGET_TRIPLE="aarch64-linux-android${API_LEVEL}"
        EXPECTED_MACHINE="183" # 0xB7
        ;;
    x86)
        TARGET_TRIPLE="i686-linux-android${API_LEVEL}"
        EXPECTED_MACHINE="3" # 0x03
        ;;
    armeabi-v7a)
        TARGET_TRIPLE="armv7a-linux-androideabi${API_LEVEL}"
        EXPECTED_MACHINE="40" # 0x28
        ;;
    *)
        echo "ERROR: Unsupported ABI '${ABI}'." >&2
        exit 1
        ;;
esac

mkdir -p "${OUT_DIR}"
OUTPUT_BIN="${OUT_DIR}/fang-input-daemon-android-${ABI}"
MANIFEST_FILE="${OUT_DIR}/guest_manifest.json"

rm -f "${OUTPUT_BIN}" "${MANIFEST_FILE}"

SOURCES=(
    "${REPO_ROOT}/apps/fang-input-daemon/main.cpp"
    "${REPO_ROOT}/apps/fang-input-daemon/guest_input_backend.cpp"
    "${REPO_ROOT}/apps/fang-input-daemon/guest_input_daemon.cpp"
    "${REPO_ROOT}/apps/fang-input-daemon/guest_manifest.cpp"
    "${REPO_ROOT}/apps/fang-studio/input_ipc_protocol.cpp"
    "${REPO_ROOT}/apps/fang-studio/input_transport.cpp"
    "${REPO_ROOT}/apps/fang-studio/socket_input_transport.cpp"
)

for src in "${SOURCES[@]}"; do
    if [[ ! -f "${src}" ]]; then
        echo "ERROR: Missing source file: ${src}" >&2
        exit 1
    fi
done

CFLAGS=(
    "--target=${TARGET_TRIPLE}"
    "-std=c++20"
    "-fPIE"
    "-pie"
    "-Wall"
    "-Wextra"
    "-D__ANDROID__"
    "-I${REPO_ROOT}/apps/fang-input-daemon"
    "-I${REPO_ROOT}/apps/fang-studio"
)

if [[ "${CONFIG}" == "Release" ]]; then
    CFLAGS+=("-O2" "-DNDEBUG")
else
    CFLAGS+=("-O0" "-g" "-DFANG_DEBUG")
fi

echo "Compiling guest daemon for ${TARGET_TRIPLE} (${CONFIG})..."
if ! "${CLANG_CXX}" "${CFLAGS[@]}" "${SOURCES[@]}" -o "${OUTPUT_BIN}"; then
    rm -f "${OUTPUT_BIN}"
    echo "ERROR: Compilation failed." >&2
    exit 1
fi

if [[ ! -f "${OUTPUT_BIN}" ]]; then
    echo "ERROR: Output binary was not generated." >&2
    exit 1
fi

SHA256_HEX=$(sha256sum "${OUTPUT_BIN}" | awk '{print $1}')
FILE_SIZE=$(wc -c < "${OUTPUT_BIN}" | tr -d ' ')
SOURCE_REVISION=$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || true)

cat <<EOF > "${MANIFEST_FILE}"
{
  "artifact_name": "fang-input-daemon-android-${ABI}",
  "artifact_format": "ELF",
  "target_os": "android",
  "target_abi": "${ABI}",
  "protocol_version": 1,
  "compiler_id": "Android NDK Clang r${NDK_REV}",
  "build_configuration": "${CONFIG}",
  "build_config": "${CONFIG}",
  "backend_capability": "RecordingStub",
  "privileged_backend_enabled": false,
  "is_cross_compiled": true,
  "is_android_elf_binary": true,
  "toolchain_status": "ToolchainAvailable",
  "artifact_path": "${OUTPUT_BIN}",
  "android_api_level": ${API_LEVEL},
  "ndk_version": "r${NDK_REV}",
  "artifact_size_bytes": ${FILE_SIZE},
  "artifact_sha256": "${SHA256_HEX}",
  "build_result": "Success",
  "elf_machine": "${EXPECTED_MACHINE}",
  "elf_class": 2,
  "elf_data": 1,
  "build_rule_version": "stage9-v1",
  "manifest_schema_version": "stage9-v2",
  "source_revision": "${SOURCE_REVISION}",
  "provenance_collected": ["artifact_name","target_abi","android_api_level","build_configuration","compiler_id","ndk_version","artifact_size_bytes","artifact_sha256","build_result","elf_machine","elf_class","elf_data"],
  "provenance_unavailable": []
}
EOF

echo "Successfully compiled and validated Android guest daemon artifact:"
echo "  Artifact: ${OUTPUT_BIN} (${FILE_SIZE} bytes)"
echo "  SHA-256:  ${SHA256_HEX}"
echo "  ABI:      ${ABI} (API ${API_LEVEL})"
echo "  Manifest: ${MANIFEST_FILE}"
exit 0
