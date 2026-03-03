#!/usr/bin/env python3
"""Solution — Task A: Framebuffer Math

Reads framebuffer parameters from sysfs and calculates:
1. Total framebuffer size in bytes
2. Wasted bytes per row (alignment padding)
3. Memory savings from 32 bpp → 16 bpp
"""

def read_sysfs(name):
    with open(f"/sys/class/graphics/fb0/{name}") as f:
        return f.read().strip()

width, height = [int(x) for x in read_sysfs("virtual_size").split(",")]
bpp = int(read_sysfs("bits_per_pixel"))
stride = int(read_sysfs("stride"))
pixel_bytes = bpp // 8

print(f"Resolution:      {width} x {height}")
print(f"Bits per pixel:  {bpp}")
print(f"Bytes per pixel: {pixel_bytes}")
print(f"Stride:          {stride} bytes/row")
print()

# 1. Total size
total = stride * height
print(f"1. Total framebuffer size:")
print(f"   stride × height = {stride} × {height} = {total} bytes")
print(f"   = {total / 1024:.1f} KB = {total / 1024 / 1024:.2f} MB")
print()

# 2. Wasted bytes per row
used_per_row = width * pixel_bytes
wasted = stride - used_per_row
print(f"2. Wasted bytes per row:")
print(f"   stride - (width × bytes_per_pixel) = {stride} - ({width} × {pixel_bytes}) = {wasted}")
if wasted > 0:
    total_wasted = wasted * height
    print(f"   Total wasted: {wasted} × {height} = {total_wasted} bytes ({total_wasted / 1024:.1f} KB)")
    print(f"   Why: Hardware requires rows aligned to a power-of-2 boundary")
    print(f"   (often 64 or 256 bytes) for efficient DMA transfers.")
else:
    print(f"   No padding — stride equals width × bytes_per_pixel exactly.")
print()

# 3. Memory savings at 16 bpp
if bpp == 32:
    size_16bpp = width * 2 * height  # 2 bytes per pixel, assuming stride = width*2
    savings = total - size_16bpp
    pct = savings / total * 100
    print(f"3. Memory savings switching to 16 bpp:")
    print(f"   Current (32 bpp): {total:,} bytes")
    print(f"   At 16 bpp:        {size_16bpp:,} bytes")
    print(f"   Savings:          {savings:,} bytes ({pct:.0f}%)")
    print()
    print(f"   Color precision lost:")
    print(f"     Red:   8 bits (256 levels) → 5 bits (32 levels)")
    print(f"     Green: 8 bits (256 levels) → 6 bits (64 levels)")
    print(f"     Blue:  8 bits (256 levels) → 5 bits (32 levels)")
    print(f"     Total: 16.7M colors → 65,536 colors")
elif bpp == 16:
    size_32bpp = width * 4 * height
    print(f"3. Already at 16 bpp. Upgrading to 32 bpp would cost:")
    print(f"   Current (16 bpp): {total:,} bytes")
    print(f"   At 32 bpp:        {size_32bpp:,} bytes")
    print(f"   Extra:            {size_32bpp - total:,} bytes")
