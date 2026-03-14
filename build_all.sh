#!/bin/bash
# build_all.sh — Build all apps and solutions in the embedded-linux repo
#
# Usage:  ./build_all.sh          # build everything
#         ./build_all.sh clean    # remove all build artifacts

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

ok()   { echo -e "  ${GREEN}OK${NC}  $1"; }
fail() { echo -e "  ${RED}FAIL${NC}  $1"; ERRORS=$((ERRORS + 1)); }
info() { echo -e "${CYAN}==> $1${NC}"; }

ERRORS=0

# ── Clean mode ──────────────────────────────────────────────────────
if [ "$1" = "clean" ]; then
    info "Cleaning SDL2 apps"
    make -C "$SCRIPT_DIR/apps" clean 2>/dev/null || true

    info "Cleaning Qt Dashboard"
    rm -rf "$SCRIPT_DIR/apps/qt_dashboard/build"

    info "Cleaning Qt Launcher"
    rm -rf "$SCRIPT_DIR/apps/qt_launcher/build"

    info "Cleaning SDL2 Rotating Cube"
    rm -rf "$SCRIPT_DIR/solutions/sdl2-rotating-cube/build"

    info "Cleaning SDL2 Touch Paint"
    rm -rf "$SCRIPT_DIR/solutions/sdl2-touch-paint/build"

    echo -e "${GREEN}Clean complete.${NC}"
    exit 0
fi

# ── Build SDL2 apps (Makefile) ──────────────────────────────────────
info "Building SDL2 apps (apps/Makefile)"
if make -C "$SCRIPT_DIR/apps" -j"$(nproc)" 2>&1; then
    ok "SDL2 apps"
else
    fail "SDL2 apps"
fi

# ── Build Qt Dashboard (CMake + Qt6) ───────────────────────────────
info "Building Qt Dashboard"
if cmake -S "$SCRIPT_DIR/apps/qt_dashboard" \
         -B "$SCRIPT_DIR/apps/qt_dashboard/build" -DCMAKE_BUILD_TYPE=Release 2>&1 \
   && cmake --build "$SCRIPT_DIR/apps/qt_dashboard/build" -j"$(nproc)" 2>&1; then
    ok "Qt Dashboard"
else
    fail "Qt Dashboard"
fi

# ── Build Qt Launcher (CMake + Qt6 + libdrm) ──────────────────────
info "Building Qt Launcher"
if cmake -S "$SCRIPT_DIR/apps/qt_launcher" \
         -B "$SCRIPT_DIR/apps/qt_launcher/build" -DCMAKE_BUILD_TYPE=Release 2>&1 \
   && cmake --build "$SCRIPT_DIR/apps/qt_launcher/build" -j"$(nproc)" 2>&1; then
    ok "Qt Launcher"
else
    fail "Qt Launcher"
fi

# ── Build SDL2 Rotating Cube solutions (CMake) ────────────────────
info "Building SDL2 Rotating Cube (solutions)"
if cmake -S "$SCRIPT_DIR/solutions/sdl2-rotating-cube" \
         -B "$SCRIPT_DIR/solutions/sdl2-rotating-cube/build" -DCMAKE_BUILD_TYPE=Release 2>&1 \
   && cmake --build "$SCRIPT_DIR/solutions/sdl2-rotating-cube/build" -j"$(nproc)" 2>&1; then
    ok "SDL2 Rotating Cube"
else
    fail "SDL2 Rotating Cube"
fi

# ── Build SDL2 Touch Paint (CMake) ────────────────────────────────
info "Building SDL2 Touch Paint (solutions)"
if cmake -S "$SCRIPT_DIR/solutions/sdl2-touch-paint" \
         -B "$SCRIPT_DIR/solutions/sdl2-touch-paint/build" -DCMAKE_BUILD_TYPE=Release 2>&1 \
   && cmake --build "$SCRIPT_DIR/solutions/sdl2-touch-paint/build" -j"$(nproc)" 2>&1; then
    ok "SDL2 Touch Paint"
else
    fail "SDL2 Touch Paint"
fi

# ── Summary ───────────────────────────────────────────────────────
echo ""
if [ $ERRORS -eq 0 ]; then
    echo -e "${GREEN}All builds succeeded.${NC}"
else
    echo -e "${RED}$ERRORS build(s) failed.${NC}"
    exit 1
fi
