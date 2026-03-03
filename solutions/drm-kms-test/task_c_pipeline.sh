#!/bin/bash
# Solution — Task C: Explore the Pipeline
#
# Extracts and summarizes CRTCs and Planes from modetest output.
# Run with: sudo bash task_c_pipeline.sh

set -e

echo "=== DRM/KMS Pipeline Summary ==="
echo

# ── CRTCs ──────────────────────────────────────────────────
echo "--- CRTCs ---"
# Extract CRTC IDs and their status
sudo modetest -M vc4 2>/dev/null | sed -n '/^CRTCs:/,/^[A-Z]/{/^CRTCs:/d; /^$/d; /^[A-Z]/d; p}' | head -20

CRTC_COUNT=$(sudo modetest -M vc4 2>/dev/null | sed -n '/^CRTCs:/,/^[A-Z]/{/^[0-9]/p}' | wc -l)
echo
echo "Total CRTCs: $CRTC_COUNT"
echo

# ── Planes ─────────────────────────────────────────────────
echo "--- Planes ---"
# Count planes and their types
PLANE_DATA=$(sudo modetest -M vc4 2>/dev/null)

# Get plane IDs
PLANE_IDS=$(echo "$PLANE_DATA" | sed -n '/^Planes:/,/^[A-Z]/{/^[0-9]/p}' | awk '{print $1}')
PLANE_COUNT=$(echo "$PLANE_IDS" | wc -w)

echo "Total Planes: $PLANE_COUNT"
echo

# Extract type for each plane by finding the "type:" property value
echo "Plane details:"
echo "$PLANE_DATA" | awk '
/^Planes:/ { in_planes=1; next }
in_planes && /^[A-Z]/ && !/^Planes/ { exit }
in_planes && /^[0-9]/ {
    plane_id = $1
    plane_crtc = $2
    printf "  Plane %s (crtc_id=%s)", plane_id, plane_crtc
}
in_planes && /type:/ { getline; getline; getline
    if ($0 ~ /value:/) {
        val = $NF
        if (val == 0) type = "Overlay"
        else if (val == 1) type = "Primary"
        else if (val == 2) type = "Cursor"
        else type = "Unknown(" val ")"
        printf " → %s\n", type
    }
}
'

echo
echo "--- Answer ---"
echo "If a CRTC has 1 Primary + 2 Overlay planes, it can composite"
echo "3 independent image layers in hardware (1 background + 2 overlays)."
echo "Each layer can have different position, size, and pixel format."
echo "The CRTC blends them together before sending to the connector."
