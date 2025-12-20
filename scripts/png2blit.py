#!/usr/bin/env python3
"""
PNG to C BGR888 Blit Generator

Converts a PNG image to C source files with embedded BGR888 pixel data
and blit functions with clipping support.

Usage:
    python png2blit.py input.png --name logo --outdir ./gen

Example generated structure:

--- logo.h ---
#ifndef LOGO_H
#define LOGO_H

#include <stdint.h>

extern const int logo_w;           // original width
extern const int logo_h;           // original height

// Fast memcpy-based blit (no scaling, no alpha)
void logo_blit_memcpy_bgr888(
    uint8_t *dst, int dst_w, int dst_h, int dst_stride_bytes, int x, int y
);

// Pixel-wise blit with alpha blending and scaling support
// alpha=255: opaque blit, alpha=0: fill bg color, 0<alpha<255: blend
// scale: 1 = no scaling, 2-16 = scale up
void logo_blit_pixelwise_bgr888(
    uint8_t *dst, int dst_w, int dst_h, int dst_stride_bytes, int x, int y,
    uint8_t alpha, uint8_t bg_b, uint8_t bg_g, uint8_t bg_r, int scale
);

#endif /* LOGO_H */
"""

import argparse
import os
import re
import sys

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)


def sanitize_c_identifier(name: str) -> str:
    """
    Sanitize a string to be a valid C identifier.
    - Replace invalid characters with underscores
    - Ensure it doesn't start with a digit
    - Convert to lowercase for consistency
    """
    # Replace any non-alphanumeric characters with underscores
    sanitized = re.sub(r'[^a-zA-Z0-9_]', '_', name)
    
    # Collapse multiple underscores
    sanitized = re.sub(r'_+', '_', sanitized)
    
    # Remove leading/trailing underscores
    sanitized = sanitized.strip('_')
    
    # If empty after sanitization, use a default
    if not sanitized:
        sanitized = "image"
    
    # Ensure it doesn't start with a digit
    if sanitized[0].isdigit():
        sanitized = '_' + sanitized
    
    return sanitized.lower()


def format_pixel_array(pixels: bytes, bytes_per_line: int = 16) -> str:
    """
    Format pixel bytes as a C array initializer with nice formatting.
    Uses 16 bytes per line for readable diffs.
    """
    lines = []
    for i in range(0, len(pixels), bytes_per_line):
        chunk = pixels[i:i + bytes_per_line]
        hex_values = ', '.join(f'0x{b:02x}' for b in chunk)
        lines.append(f'    {hex_values},')
    return '\n'.join(lines)


def generate_header(name: str, width: int, height: int) -> str:
    """Generate the .h header file content."""
    guard = f'{name.upper()}_H'
    
    lines = [
        f'#ifndef {guard}',
        f'#define {guard}',
        '',
        '#include <stdint.h>',
        '',
    ]
    
    # Dimension externs
    lines.append(f'extern const int {name}_w;')
    lines.append(f'extern const int {name}_h;')
    lines.append('')
    
    # Memcpy blit function prototype
    lines.extend([
        '/**',
        f' * Fast blit the {name} image to a destination BGR888 buffer using memcpy.',
        ' *',
        ' * This is the fastest blit method - uses row-by-row memcpy.',
        ' * No scaling, no alpha blending - direct pixel copy.',
        ' *',
        ' * @param dst              Pointer to destination buffer (BGR888 format)',
        ' * @param dst_w            Width of destination buffer in pixels',
        ' * @param dst_h            Height of destination buffer in pixels',
        ' * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)',
        ' * @param x                X position to blit to (can be negative for clipping)',
        ' * @param y                Y position to blit to (can be negative for clipping)',
        ' */',
        f'void {name}_blit_memcpy_bgr888(',
        '    uint8_t *dst,',
        '    int dst_w,',
        '    int dst_h,',
        '    int dst_stride_bytes,',
        '    int x,',
        '    int y',
        ');',
        '',
    ])
    
    # Pixelwise blit function prototype
    lines.extend([
        '/**',
        f' * Blit the {name} image with alpha blending and scaling to a BGR888 buffer.',
        ' *',
        ' * Pixel-by-pixel blit with three optimized code paths:',
        ' * - alpha = 255: Direct pixel copy (opaque)',
        ' * - alpha = 0: Fill with background color only',
        ' * - 0 < alpha < 255: Alpha-blend source with background color',
        ' *',
        ' * @param dst              Pointer to destination buffer (BGR888 format)',
        ' * @param dst_w            Width of destination buffer in pixels',
        ' * @param dst_h            Height of destination buffer in pixels',
        ' * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)',
        ' * @param x                X position to blit to (can be negative for clipping)',
        ' * @param y                Y position to blit to (can be negative for clipping)',
        ' * @param alpha            Alpha value (0=transparent/bg only, 255=fully opaque)',
        ' * @param bg_b             Background blue component (0-255)',
        ' * @param bg_g             Background green component (0-255)',
        ' * @param bg_r             Background red component (0-255)',
        ' * @param scale            Scale factor (1 = no scaling, 2-16 = scale up)',
        ' */',
        f'void {name}_blit_pixelwise_bgr888(',
        '    uint8_t *dst,',
        '    int dst_w,',
        '    int dst_h,',
        '    int dst_stride_bytes,',
        '    int x,',
        '    int y,',
        '    uint8_t alpha,',
        '    uint8_t bg_b,',
        '    uint8_t bg_g,',
        '    uint8_t bg_r,',
        '    int scale',
        ');',
        '',
    ])
    
    lines.append(f'#endif /* {guard} */')
    lines.append('')
    
    return '\n'.join(lines)


def generate_source(name: str, width: int, height: int, pixels: bytes) -> str:
    """Generate the .c source file content."""
    pixel_array = format_pixel_array(pixels)
    
    lines = [
        f'#include "{name}.h"',
        '#include <string.h>',
        '',
        f'const int {name}_w = {width};',
        f'const int {name}_h = {height};',
        '',
        f'static const uint8_t {name}_pixels[] = {{',
        pixel_array,
        '};',
        '',
    ]
    
    # Memcpy blit function (efficient row copy)
    lines.extend([
        f'void {name}_blit_memcpy_bgr888(',
        '    uint8_t *dst,',
        '    int dst_w,',
        '    int dst_h,',
        '    int dst_stride_bytes,',
        '    int x,',
        '    int y',
        ')',
        '{',
        f'    const int src_w = {name}_w;',
        f'    const int src_h = {name}_h;',
        '',
        '    /* Compute source and destination regions with clipping */',
        '    int src_x = 0;',
        '    int src_y = 0;',
        '    int copy_w = src_w;',
        '    int copy_h = src_h;',
        '',
        '    /* Clip left edge */',
        '    if (x < 0) {',
        '        src_x = -x;',
        '        copy_w += x;',
        '        x = 0;',
        '    }',
        '',
        '    /* Clip top edge */',
        '    if (y < 0) {',
        '        src_y = -y;',
        '        copy_h += y;',
        '        y = 0;',
        '    }',
        '',
        '    /* Clip right edge */',
        '    if (x + copy_w > dst_w) {',
        '        copy_w = dst_w - x;',
        '    }',
        '',
        '    /* Clip bottom edge */',
        '    if (y + copy_h > dst_h) {',
        '        copy_h = dst_h - y;',
        '    }',
        '',
        '    /* If fully clipped, nothing to draw */',
        '    if (copy_w <= 0 || copy_h <= 0) {',
        '        return;',
        '    }',
        '',
        '    /* Blit row by row using memcpy */',
        '    const int src_stride = src_w * 3;',
        '    const int copy_bytes = copy_w * 3;',
        '',
        '    for (int j = 0; j < copy_h; j++) {',
        f'        const uint8_t *src_row = {name}_pixels + (src_y + j) * src_stride + (src_x * 3);',
        '        uint8_t *dst_row = dst + (y + j) * dst_stride_bytes + (x * 3);',
        '        memcpy(dst_row, src_row, copy_bytes);',
        '    }',
        '}',
        '',
    ])
    
    # Pixelwise blit function (with alpha and scaling)
    lines.extend([
        f'void {name}_blit_pixelwise_bgr888(',
        '    uint8_t *dst,',
        '    int dst_w,',
        '    int dst_h,',
        '    int dst_stride_bytes,',
        '    int x,',
        '    int y,',
        '    uint8_t alpha,',
        '    uint8_t bg_b,',
        '    uint8_t bg_g,',
        '    uint8_t bg_r,',
        '    int scale',
        ')',
        '{',
        f'    const int src_w = {name}_w;',
        f'    const int src_h = {name}_h;',
        '',
        '    /* Treat scale <= 1 as no scaling */',
        '    if (scale <= 1) scale = 1;',
        '',
        '    const int scaled_w = src_w * scale;',
        '    const int scaled_h = src_h * scale;',
        '',
        '    /* Compute clipping in scaled coordinates */',
        '    int dst_x0 = x;',
        '    int dst_y0 = y;',
        '    int dst_x1 = x + scaled_w;',
        '    int dst_y1 = y + scaled_h;',
        '',
        '    /* Clip to destination bounds */',
        '    if (dst_x0 < 0) dst_x0 = 0;',
        '    if (dst_y0 < 0) dst_y0 = 0;',
        '    if (dst_x1 > dst_w) dst_x1 = dst_w;',
        '    if (dst_y1 > dst_h) dst_y1 = dst_h;',
        '',
        '    /* If fully clipped, nothing to draw */',
        '    if (dst_x0 >= dst_x1 || dst_y0 >= dst_y1) {',
        '        return;',
        '    }',
        '',
        '    /* Optimized code paths based on alpha value */',
        '    if (alpha == 0) {',
        '        /* Alpha = 0: Fill region with background color only */',
        '        for (int dy = dst_y0; dy < dst_y1; dy++) {',
        '            uint8_t *dst_row = dst + dy * dst_stride_bytes;',
        '            for (int dx = dst_x0; dx < dst_x1; dx++) {',
        '                uint8_t *dst_px = dst_row + dx * 3;',
        '                dst_px[0] = bg_b;',
        '                dst_px[1] = bg_g;',
        '                dst_px[2] = bg_r;',
        '            }',
        '        }',
        '    } else if (alpha == 255) {',
        '        /* Alpha = 255: Opaque blit, no blending needed */',
        '        for (int dy = dst_y0; dy < dst_y1; dy++) {',
        '            int src_y_idx = (dy - y) / scale;',
        '            uint8_t *dst_row = dst + dy * dst_stride_bytes;',
        '',
        '            for (int dx = dst_x0; dx < dst_x1; dx++) {',
        '                int src_x_idx = (dx - x) / scale;',
        f'                const uint8_t *src_px = {name}_pixels + (src_y_idx * src_w + src_x_idx) * 3;',
        '',
        '                uint8_t *dst_px = dst_row + dx * 3;',
        '                dst_px[0] = src_px[0];  /* B */',
        '                dst_px[1] = src_px[1];  /* G */',
        '                dst_px[2] = src_px[2];  /* R */',
        '            }',
        '        }',
        '    } else {',
        '        /* 0 < alpha < 255: Alpha blend with background */',
        '        const int inv_alpha = 255 - alpha;',
        '',
        '        for (int dy = dst_y0; dy < dst_y1; dy++) {',
        '            int src_y_idx = (dy - y) / scale;',
        '            uint8_t *dst_row = dst + dy * dst_stride_bytes;',
        '',
        '            for (int dx = dst_x0; dx < dst_x1; dx++) {',
        '                int src_x_idx = (dx - x) / scale;',
        f'                const uint8_t *src_px = {name}_pixels + (src_y_idx * src_w + src_x_idx) * 3;',
        '',
        '                uint8_t *dst_px = dst_row + dx * 3;',
        '                /* Composite: out = src * alpha + bg * (1 - alpha) */',
        '                dst_px[0] = (uint8_t)((src_px[0] * alpha + bg_b * inv_alpha + 127) / 255);',
        '                dst_px[1] = (uint8_t)((src_px[1] * alpha + bg_g * inv_alpha + 127) / 255);',
        '                dst_px[2] = (uint8_t)((src_px[2] * alpha + bg_r * inv_alpha + 127) / 255);',
        '            }',
        '        }',
        '    }',
        '}',
        '',
    ])
    
    return '\n'.join(lines)


def convert_png_to_blit(input_path: str, name: str, outdir: str) -> None:
    """
    Convert a PNG image to C source files with embedded BGR888 data
    and blit functions.
    """
    # Validate input file exists
    if not os.path.isfile(input_path):
        print(f"Error: Input file '{input_path}' not found.")
        sys.exit(1)
    
    # Try to open the image
    try:
        img = Image.open(input_path)
    except Exception as e:
        print(f"Error: Could not open image '{input_path}': {e}")
        sys.exit(1)
    
    # Get image info
    width, height = img.size
    original_mode = img.mode
    
    # Convert to RGB (discards alpha if present)
    img = img.convert('RGB')
    
    # Get raw pixel data as bytes (RGB order, row-major)
    rgb_pixels = img.tobytes()
    
    # Convert RGB to BGR
    bgr_pixels = bytearray(len(rgb_pixels))
    for i in range(0, len(rgb_pixels), 3):
        bgr_pixels[i] = rgb_pixels[i + 2]      # B <- R position
        bgr_pixels[i + 1] = rgb_pixels[i + 1]  # G stays
        bgr_pixels[i + 2] = rgb_pixels[i]      # R <- B position
    bgr_pixels = bytes(bgr_pixels)
    
    # Sanitize name for C identifier
    c_name = sanitize_c_identifier(name)
    if c_name != name:
        print(f"Note: Name sanitized from '{name}' to '{c_name}'")
    
    # Create output directory if needed
    if outdir and not os.path.exists(outdir):
        os.makedirs(outdir)
        print(f"Created output directory: {outdir}")
    
    # Generate file paths
    header_path = os.path.join(outdir, f'{c_name}.h') if outdir else f'{c_name}.h'
    source_path = os.path.join(outdir, f'{c_name}.c') if outdir else f'{c_name}.c'
    
    # Generate content
    header_content = generate_header(c_name, width, height)
    source_content = generate_source(c_name, width, height, bgr_pixels)
    
    # Write files
    with open(header_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(header_content)
    
    with open(source_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(source_content)
    
    # Print summary
    total_bytes = len(bgr_pixels)
    print(f"Input:  {input_path}")
    print(f"Size:   {width}x{height} pixels")
    print(f"Mode:   {original_mode} -> BGR888")
    print(f"Funcs:  blit_memcpy_bgr888, blit_pixelwise_bgr888")
    print(f"Output: {header_path}")
    print(f"        {source_path}")
    print(f"Bytes:  {total_bytes} ({total_bytes / 1024:.1f} KB)")


def main():
    parser = argparse.ArgumentParser(
        description='Convert PNG image to C source files with BGR888 blit functions.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
    python png2blit.py logo.png --name logo --outdir ./gen

This generates:
    ./gen/logo.h  - Header with extern declarations and function prototypes
    ./gen/logo.c  - Source with BGR888 pixel data and blit implementations

Generated functions:
    logo_blit_memcpy_bgr888()    - Fast memcpy-based blit (no scaling/alpha)
    logo_blit_pixelwise_bgr888() - Pixel-by-pixel with alpha blend & scaling
'''
    )
    
    parser.add_argument(
        'input',
        help='Input PNG file path'
    )
    
    parser.add_argument(
        '--name', '-n',
        default=None,
        help='Base name for output files and C identifiers (default: input filename without extension)'
    )
    
    parser.add_argument(
        '--outdir', '-o',
        default='.',
        help='Output directory for generated files (default: current directory)'
    )
    
    args = parser.parse_args()
    
    # Derive name from input filename if not provided
    if args.name is None:
        args.name = os.path.splitext(os.path.basename(args.input))[0]
    
    convert_png_to_blit(args.input, args.name, args.outdir)


if __name__ == '__main__':
    main()
