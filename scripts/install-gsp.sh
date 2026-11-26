#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# Interactive guided installer for the AIEN GB10/GB20B GSP GPU driver (/dev/aien-gpu).
# Replaces nvidia.ko with aien_gpu.ko (pure-Omega decision units + thin Linux kernel glue).
# Never touches host firmware settings or boot partitions — uses Linux modprobe + insmod only.
#
# Usage:
#   sh scripts/install-gsp.sh                  # Interactive step-by-step walkthrough
#   sh scripts/install-gsp.sh --status         # Read-only status check (safe to run anytime)
#   sh scripts/install-gsp.sh --restore-nvidia # Undo blacklist and switch back to nvidia.ko
#   sh scripts/install-gsp.sh --help           # Show usage
set -eu

HERE=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BLACKLIST_FILE="/etc/modprobe.d/zz-aien-gpu-blacklist-nvidia.conf"
FW_BIN="/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin"
PINNED_OMC_SHA="189ada82e00bb440cb41351bd252cc68c54a1b1050051cc42a1df700081c3b54"
DEFAULT_PKG="${AIEN_GPU_PKG:-$HOME/work/gsp-pkg}"
EV_DIR="${AIEN_GPU_EV:-$HOME/aien-gpu-install-$(date -u +%Y%m%dT%H%M%SZ)}"

say()  { printf "\n==> %s\n" "$*"; }
info() { printf "    %s\n" "$*"; }
warn() { printf "  ! %s\n" "$*" >&2; }

ask_yn() {
  prompt=$1
  printf "\n  ? %s [y/N]: " "$prompt"
  read -r ans || ans=""
  case "$ans" in
    y|Y|yes|YES) return 0 ;;
    *) return 1 ;;
  esac
}

find_pinned_omc() {
  for cand in \
    "${OMC:-}" \
    "$HOME/workspace/review-37b16a71/build/omc" \
    "$HOME/.local/bin/omc" \
    "$(command -v omc 2>/dev/null || true)"
  do
    [ -n "$cand" ] && [ -x "$cand" ] || continue
    got=$(sha256sum "$cand" | awk "{print \$1}")
    if [ "$got" = "$PINNED_OMC_SHA" ]; then
      printf "%s\n" "$cand"
      return 0
    fi
  done
  return 1
}

show_help() {
  cat <<EOF
AIEN GB10/GB20B GSP Driver Guided Installer (aien_gpu.ko)

Usage:
  sh scripts/install-gsp.sh                  Step-by-step guided installation
  sh scripts/install-gsp.sh --status         Read-only check of GPU, driver, and firmware
  sh scripts/install-gsp.sh --restore-nvidia Remove nvidia blacklist and restore nvidia.ko
  sh scripts/install-gsp.sh --help           Show this help message

Environment overrides (optional):
  OMC=/path/to/omc           Path to pinned Omega compiler (sha256: $PINNED_OMC_SHA)
  AIEN_GPU_PKG=/path/to/pkg  Directory containing system_info.bin + GSP boot package
  AIEN_GPU_EV=/path/to/dir   Directory to save bring-up logs (Stage A/B/C + M16)

Note: Switching from nvidia.ko to aien_gpu.ko requires ONE cold reboot so the GB20B
GPU starts from a clean PCI reset before our driver boots the signed GSP firmware.
EOF
}

show_status() {
  say "AIEN GSP Driver Status Check (read-only)"
  info "Host arch:   $(uname -s) $(uname -m) ($(uname -r))"
  if command -v lspci >/dev/null 2>&1; then
    gpu=$(lspci -nn | grep -i "10de:2e12" || true)
    if [ -n "$gpu" ]; then
      info "GB10/GB20B:  FOUND ($gpu)"
    else
      warn "GB10/GB20B:  NOT FOUND (expected PCI 10de:2e12)"
    fi
  fi
  if [ -r "$FW_BIN" ]; then
    info "GSP fw blob: FOUND ($FW_BIN)"
  else
    warn "GSP fw blob: MISSING ($FW_BIN)"
  fi
  if lsmod | grep -q "^nvidia"; then
    info "nvidia.ko:   LOADED (Phase 1 needed: blacklist + reboot before loading aien_gpu.ko)"
  else
    info "nvidia.ko:   NOT LOADED (ready for Phase 2!)"
  fi
  if lsmod | grep -q "^aien_gpu "; then
    info "aien_gpu.ko: LOADED"
  else
    info "aien_gpu.ko: not loaded"
  fi
  if [ -e /dev/aien-gpu ]; then
    info "/dev/aien-gpu: PRESENT ($(ls -l /dev/aien-gpu | awk "{print \$1, \$3\":\"\$4}"))"
  else
    info "/dev/aien-gpu: absent"
  fi
  if [ -e "$BLACKLIST_FILE" ]; then
    info "Blacklist:   ACTIVE ($BLACKLIST_FILE)"
  else
    info "Blacklist:   not installed"
  fi
  if omc_found=$(find_pinned_omc); then
    info "Pinned omc:  FOUND ($omc_found)"
  else
    info "Pinned omc:  not found in default paths (set OMC=/path/to/omc to rebuild from source)"
  fi
}

restore_nvidia() {
  say "Restore NVIDIA driver (undo aien_gpu.ko setup)"
  info "This will:"
  info "  1. Remove $BLACKLIST_FILE (if present)"
  info "  2. Re-enable nvidia-persistenced.service"
  info "  3. Optionally reboot so nvidia.ko binds again"
  if ! ask_yn "Proceed with restoring nvidia.ko?"; then
    info "Cancelled. Nothing was changed."
    exit 0
  fi
  sudo rm -f "$BLACKLIST_FILE" /etc/modprobe.d/zz-window0-blacklist-nvidia.conf
  sudo systemctl enable nvidia-persistenced >/dev/null 2>&1 || true
  say "Blacklist removed. Reboot to return to nvidia.ko."
  if ask_yn "Reboot now?"; then
    sudo reboot
  fi
  exit 0
}

case "${1:-}" in
  --help|-h) show_help; exit 0 ;;
  --status) show_status; exit 0 ;;
  --restore-nvidia) restore_nvidia; exit 0 ;;
  "") ;;
  *) warn "Unknown option: $1"; show_help; exit 2 ;;
esac

# --- Interactive walkthrough ---
say "Welcome to the AIEN GB10/GB20B GSP Driver Guided Installer"
info "What this script does:"
info "  * Phase 1 (if nvidia.ko is loaded): blacklists nvidia.ko so the GPU boots clean."
info "  * Phase 2 (after reboot): builds & loads aien_gpu.ko, boots the signed GSP firmware,"
info "    initializes RM + compute channel (0xcec0), and runs the M16 hardware self-test."
info "  * You can undo this anytime with: sh scripts/install-gsp.sh --restore-nvidia"

show_status

# Basic hardware/OS guardrails
if [ "$(uname -s)/$(uname -m)" != "Linux/aarch64" ]; then
  warn "This driver targets Linux AArch64 (DGX Spark GB10/GB20B). Current host: $(uname -s)/$(uname -m)."
  exit 1
fi
if [ ! -r "$FW_BIN" ]; then
  warn "Signed NVIDIA GSP firmware not found at: $FW_BIN"
  info "Install the NVIDIA 580.173.02 firmware package first (only the firmware blob is used, not nvidia.ko)."
  exit 1
fi

# Step 1: Build if needed
say "Step 1 of 4: Check / Build aien_gpu.ko and bring-up tools"
if [ -f "$HERE/kmod/aien_gpu.ko" ] && [ -x "$HERE/build/aien-gpu-boot" ] && [ -x "$HERE/build/aien-gpu-m16" ]; then
  info "Already built: kmod/aien_gpu.ko, build/aien-gpu-boot, build/aien-gpu-m16."
else
  if omc_path=$(find_pinned_omc); then
    info "Found pinned Omega compiler: $omc_path"
    if ask_yn "Build kmod/aien_gpu.ko and bring-up tools now?"; then
      (cd "$HERE" && make kmod build/aien-gpu-boot build/aien-gpu-m16 OMC="$omc_path")
    else
      warn "Build skipped — cannot continue without kmod/aien_gpu.ko."
      exit 1
    fi
  else
    warn "Pinned Omega compiler (sha256: $PINNED_OMC_SHA) not found."
    info "Set OMC=/path/to/omc and re-run: OMC=/path/to/omc sh scripts/install-gsp.sh"
    exit 1
  fi
fi

# Step 2: Check whether nvidia.ko is currently holding the GPU
say "Step 2 of 4: Ensure nvidia.ko is NOT holding the GPU"
if lsmod | grep -q "^nvidia"; then
  warn "nvidia.ko is currently loaded."
  info "Because GB20B GSP must be booted from a clean PCI reset, we need to:"
  info "  1. Write $BLACKLIST_FILE (blacklists nvidia, nvidia_uvm, nvidia_drm, nvidia_modeset)"
  info "  2. Disable nvidia-persistenced.service"
  info "  3. Reboot once, then run this exact same script again to finish Phase 2."
  if ! ask_yn "Install the nvidia.ko blacklist now?"; then
    info "Cancelled. Nothing was changed."
    exit 0
  fi
  sudo sh -c "cat > \"$BLACKLIST_FILE\"" <<EOF
# Managed by aien-gpu scripts/install-gsp.sh (undo with --restore-nvidia)
blacklist nvidia
blacklist nvidia_uvm
blacklist nvidia_drm
blacklist nvidia_modeset
options nvidia NVreg_OpenRmEnableUnsupportedGpus=0
EOF
  sudo systemctl disable nvidia-persistenced >/dev/null 2>&1 || true
  say "Phase 1 complete!"
  info "After reboot, run:  cd \"$HERE\" && sh scripts/install-gsp.sh"
  if ask_yn "Reboot Spark now to enter Phase 2?"; then
    sudo reboot
  fi
  exit 0
else
  info "Great — nvidia.ko is NOT loaded. Proceeding directly to Phase 2!"
fi

# Step 3: Load aien_gpu.ko and grant user permissions on /dev/aien-gpu
say "Step 3 of 4: Load aien_gpu.ko and expose /dev/aien-gpu"
if lsmod | grep -q "^aien_gpu "; then
  info "aien_gpu.ko is already loaded."
else
  info "Command: sudo insmod $HERE/kmod/aien_gpu.ko allow_bind=1 arena_mib=96 heap_mib=512"
  if ! ask_yn "Load aien_gpu.ko now?"; then
    info "Cancelled."
    exit 0
  fi
  sudo insmod "$HERE/kmod/aien_gpu.ko" allow_bind=1 arena_mib=96 heap_mib=512
fi
if [ -e /dev/aien-gpu ]; then
  sudo chgrp "$(id -gn)" /dev/aien-gpu
  sudo chmod 0660 /dev/aien-gpu
  info "/dev/aien-gpu is ready ($(ls -l /dev/aien-gpu | awk "{print \$1, \$3\":\"\$4}"))"
else
  warn "/dev/aien-gpu did not appear — check: sudo dmesg | tail -n 30"
  exit 1
fi

# Step 4: Bring up GSP (Stage A -> Stage B -> Stage C -> M16 compute check)
say "Step 4 of 4: Boot GSP firmware and verify live compute (M16)"
mkdir -p "$EV_DIR/stageB" "$EV_DIR/m16"
info_line=$(sudo "$HERE/build/aien-gpu-boot" --info)
info "$info_line"

state=$(printf "%s\n" "$info_line" | sed -n "s/.*state=\([0-9]*\).*/\1/p")
if [ "$state" = "1" ]; then
  if [ ! -f "$DEFAULT_PKG/system_info.bin" ]; then
    warn "GSP boot package not found at: $DEFAULT_PKG/system_info.bin"
    info "Build or point to your GSP package directory with:"
    info "  AIEN_GPU_PKG=/path/to/pkg sh scripts/install-gsp.sh"
    exit 1
  fi
  info "Booting signed GSP firmware (Stage B)..."
  sudo "$HERE/build/aien-gpu-boot" --pkg "$DEFAULT_PKG" --out "$EV_DIR/stageB"
elif [ "$state" = "4" ]; then
  info "GSP + RM are already UP (state=4 RM_READY) — skipping Stage B."
else
  info "Current driver state=$state — proceeding to Stage C / M16 check."
fi

info "Running Stage C (RM init if needed) + M16 hardware compute gate (class 0xcec0)..."
sudo "$HERE/build/aien-gpu-m16" --rm-init --evidence "$EV_DIR/m16"

say "SUCCESS! Your GB20B GPU is now running on /dev/aien-gpu (ZERO nvidia.ko, ZERO CUDA)."
info "Logs saved to: $EV_DIR"
info "Next: run Omega Helix directly on /dev/aien-gpu:"
info "  git clone https://github.com/drakestapleton/omega-helix.git && cd omega-helix"
info "  TARGET=gb10 sh scripts/build.sh && ./build/ollm-gb10 nvfp4 4096 4096 8192 20"
info "To switch back to nvidia.ko anytime:"
info "  sh scripts/install-gsp.sh --restore-nvidia"
