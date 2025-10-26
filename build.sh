#!/bin/bash
set -e

# Build script for Wireless Android Auto Dongle
# Handles patch changes gracefully by cleaning build artifacts when needed

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILDROOT_DIR="${SCRIPT_DIR}/buildroot"
BR2_EXTERNAL_PATH="${SCRIPT_DIR}/aa_wireless_dongle"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 <board-name> [options]"
    echo ""
    echo "Board names:"
    echo "  raspberrypi0w      - Raspberry Pi Zero W"
    echo "  raspberrypizero2w  - Raspberry Pi Zero 2 W"
    echo "  raspberrypi3a      - Raspberry Pi 3 A+"
    echo "  raspberrypi4       - Raspberry Pi 4"
    echo "  raspberrypi5       - Raspberry Pi 5"
    echo ""
    echo "Options:"
    echo "  --clean           Clean the entire build directory"
    echo "  --linux-clean     Clean only Linux build artifacts"
    echo "  --force-rebuild   Force a complete rebuild (implies --clean)"
    echo "  --help            Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 raspberrypi4"
    echo "  $0 raspberrypi0w --linux-clean"
    exit 1
}

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Parse arguments
BOARD_NAME=""
DO_CLEAN=0
DO_LINUX_CLEAN=0
FORCE_REBUILD=0

while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            DO_CLEAN=1
            shift
            ;;
        --linux-clean)
            DO_LINUX_CLEAN=1
            shift
            ;;
        --force-rebuild)
            FORCE_REBUILD=1
            DO_CLEAN=1
            shift
            ;;
        --help)
            usage
            ;;
        *)
            if [[ -z "$BOARD_NAME" ]]; then
                BOARD_NAME="$1"
            else
                log_error "Unknown option: $1"
                usage
            fi
            shift
            ;;
    esac
done

if [[ -z "$BOARD_NAME" ]]; then
    log_error "Board name is required"
    usage
fi

# Validate board name
VALID_BOARDS=("raspberrypi0w" "raspberrypizero2w" "raspberrypi3a" "raspberrypi4" "raspberrypi5")
if [[ ! " ${VALID_BOARDS[@]} " =~ " ${BOARD_NAME} " ]]; then
    log_error "Invalid board name: $BOARD_NAME"
    usage
fi

OUTPUT_DIR="${BUILDROOT_DIR}/output/${BOARD_NAME}"
PATCHES_DIR="${BR2_EXTERNAL_PATH}/patches"
PATCH_HASH_FILE="${OUTPUT_DIR}/.patch_hash"

log_info "Building for board: $BOARD_NAME"
log_info "Output directory: $OUTPUT_DIR"

# Change to buildroot directory
cd "$BUILDROOT_DIR"

# Check if we need to run defconfig
if [[ ! -f "${OUTPUT_DIR}/.config" ]]; then
    log_info "Configuration not found, running defconfig..."
    make "BR2_EXTERNAL=${BR2_EXTERNAL_PATH}" "O=${OUTPUT_DIR}" "${BOARD_NAME}_defconfig"
fi

# Calculate hash of all patch files
calculate_patch_hash() {
    if [[ -d "$PATCHES_DIR" ]]; then
        find "$PATCHES_DIR" -type f -name "*.patch" -exec md5sum {} \; | sort | md5sum | cut -d' ' -f1
    else
        echo "no_patches"
    fi
}

# Check if patches have changed
check_patches_changed() {
    local current_hash=$(calculate_patch_hash)

    if [[ ! -f "$PATCH_HASH_FILE" ]]; then
        # First build or hash file missing
        echo "$current_hash" > "$PATCH_HASH_FILE"
        return 1 # Not changed, first time
    fi

    local stored_hash=$(cat "$PATCH_HASH_FILE")

    if [[ "$current_hash" != "$stored_hash" ]]; then
        log_warn "Patches have changed since last build"
        log_warn "Previous hash: $stored_hash"
        log_warn "Current hash:  $current_hash"
        echo "$current_hash" > "$PATCH_HASH_FILE"
        return 0 # Changed
    fi

    return 1 # Not changed
}

# Handle cleaning
if [[ $FORCE_REBUILD -eq 1 ]]; then
    log_warn "Forcing complete rebuild..."
    if [[ -d "$OUTPUT_DIR" ]]; then
        cd "$OUTPUT_DIR"
        make clean
        cd "$BUILDROOT_DIR"
        # Remove patch hash to force recheck
        rm -f "$PATCH_HASH_FILE"
    fi
elif [[ $DO_CLEAN -eq 1 ]]; then
    log_info "Cleaning build directory..."
    if [[ -d "$OUTPUT_DIR" ]]; then
        cd "$OUTPUT_DIR"
        make clean
        cd "$BUILDROOT_DIR"
    fi
elif [[ $DO_LINUX_CLEAN -eq 1 ]]; then
    log_info "Cleaning Linux build artifacts..."
    if [[ -d "$OUTPUT_DIR" ]]; then
        cd "$OUTPUT_DIR"
        make linux-dirclean
        cd "$BUILDROOT_DIR"
    fi
    # Update patch hash after manual clean
    calculate_patch_hash > "$PATCH_HASH_FILE"
else
    # Auto-detect if we need to clean Linux
    if check_patches_changed; then
        log_warn "Patches have changed, cleaning Linux build artifacts..."
        if [[ -d "$OUTPUT_DIR" ]]; then
            cd "$OUTPUT_DIR"
            make linux-dirclean
            cd "$BUILDROOT_DIR"
        fi
    fi
fi

# Build
log_info "Starting build..."
cd "$OUTPUT_DIR"

if ! make; then
    log_error "Build failed!"
    log_error ""
    log_error "If you're seeing patch application errors, try:"
    log_error "  $0 $BOARD_NAME --linux-clean"
    log_error ""
    log_error "For a complete rebuild:"
    log_error "  $0 $BOARD_NAME --force-rebuild"
    exit 1
fi

# Success
log_info "Build completed successfully!"

if [[ -f "images/sdcard.img" ]]; then
    log_info "SD card image: ${OUTPUT_DIR}/images/sdcard.img"

    # Copy to images directory if it exists in parent
    IMAGES_OUT_DIR="${SCRIPT_DIR}/images"
    mkdir -p "$IMAGES_OUT_DIR"
    cp "images/sdcard.img" "${IMAGES_OUT_DIR}/sdcard-${BOARD_NAME}.img"
    log_info "Copied to: ${IMAGES_OUT_DIR}/sdcard-${BOARD_NAME}.img"
fi
