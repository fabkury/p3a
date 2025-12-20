#!/usr/bin/env python3
"""
PNG to C RGB888 Blit Generator

Converts a PNG image to C source files with embedded RGB888 pixel data
and a row-wise memcpy blit function with clipping support.

Usage:
    python png2blit.py input.png --name logo --outdir ./gen
    python png2blit.py input.png --name logo --outdir ./gen --scale 2
    python png2blit.py input.png --name logo --outdir ./gen --scale 3 --alpha

Options:
    --scale N   Generate an additional scaled blit function (2-16x)
    --alpha     Generate an alpha-blending blit function (with optional scaling)

Example generated structure (with --scale 2 --alpha):

--- logo.h ---
#ifndef LOGO_H
#define LOGO_H

#include <stdint.h>

#define LOGO_SCALE 2

extern const int logo_w;           // original width
extern const int logo_h;           // original height
extern const int logo_scaled_w;    // scaled width (w * SCALE)
extern const int logo_scaled_h;    // scaled height (h * SCALE)

void logo_blit_rgb888(
    uint8_t *dst, int dst_w, int dst_h, int dst_stride_bytes, int x, int y
);

void logo_blit_rgb888_2x(
    uint8_t *dst, int dst_w, int dst_h, int dst_stride_bytes, int x, int y
);

// Alpha blit supports optional scaling via the 'scale' parameter
// scale=0 or scale=1 means no scaling, scale=2-16 scales the image
void logo_blit_rgb888_alpha(
    uint8_t *dst, int dst_w, int dst_h, int dst_stride_bytes, int x, int y,
    uint8_t alpha, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b, int scale
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


def generate_header(name: str, width: int, height: int,
                    scale: int = None, alpha: bool = False) -> str:
    """Generate the .h header file content."""
    guard = f'{name.upper()}_H'
    name_upper = name.upper()
    
    lines = [
        f'#ifndef {guard}',
        f'#define {guard}',
        '',
        '#include <stdint.h>',
        '',
    ]
    
    # Scale factor define (only when scaling is enabled)
    if scale:
        lines.append(f'#define {name_upper}_SCALE {scale}')
        lines.append('')
    
    # Dimension externs
    lines.append(f'extern const int {name}_w;')
    lines.append(f'extern const int {name}_h;')
    
    if scale:
        lines.append(f'extern const int {name}_scaled_w;')
        lines.append(f'extern const int {name}_scaled_h;')
    
    lines.append('')
    
    # Base blit function prototype
    lines.extend([
        '/**',
        f' * Blit the {name} image to a destination RGB888 buffer.',
        ' *',
        ' * @param dst              Pointer to destination buffer (RGB888 format)',
        ' * @param dst_w            Width of destination buffer in pixels',
        ' * @param dst_h            Height of destination buffer in pixels',
        ' * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)',
        ' * @param x                X position to blit to (can be negative for clipping)',
        ' * @param y                Y position to blit to (can be negative for clipping)',
        ' */',
        f'void {name}_blit_rgb888(',
        '    uint8_t *dst,',
        '    int dst_w,',
        '    int dst_h,',
        '    int dst_stride_bytes,',
        '    int x,',
        '    int y',
        ');',
        '',
    ])
    
    # Scaled blit function prototype
    if scale:
        lines.extend([
            '/**',
            f' * Blit the {name} image scaled {scale}x to a destination RGB888 buffer.',
            ' *',
            ' * @param dst              Pointer to destination buffer (RGB888 format)',
            ' * @param dst_w            Width of destination buffer in pixels',
            ' * @param dst_h            Height of destination buffer in pixels',
            ' * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)',
            ' * @param x                X position to blit to (can be negative for clipping)',
            ' * @param y                Y position to blit to (can be negative for clipping)',
            ' */',
            f'void {name}_blit_rgb888_{scale}x(',
            '    uint8_t *dst,',
            '    int dst_w,',
            '    int dst_h,',
            '    int dst_stride_bytes,',
            '    int x,',
            '    int y',
            ');',
            '',
        ])
    
    # Alpha blit function prototype
    if alpha:
        lines.extend([
            '/**',
            f' * Blit the {name} image with alpha blending to a destination RGB888 buffer.',
            ' *',
            ' * Composites the image over a specified background color using the given alpha.',
            ' * Does not read from destination; writes fully opaque blended pixels.',
            ' * Supports optional scaling (scale=0 or scale=1 means no scaling).',
            ' *',
            ' * @param dst              Pointer to destination buffer (RGB888 format)',
            ' * @param dst_w            Width of destination buffer in pixels',
            ' * @param dst_h            Height of destination buffer in pixels',
            ' * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)',
            ' * @param x                X position to blit to (can be negative for clipping)',
            ' * @param y                Y position to blit to (can be negative for clipping)',
            ' * @param alpha            Alpha value (0=fully transparent, 255=fully opaque)',
            ' * @param bg_r             Background red component (0-255)',
            ' * @param bg_g             Background green component (0-255)',
            ' * @param bg_b             Background blue component (0-255)',
            ' * @param scale            Scale factor (0 or 1 = no scaling, 2-16 = scale up)',
            ' */',
            f'void {name}_blit_rgb888_alpha(',
            '    uint8_t *dst,',
            '    int dst_w,',
            '    int dst_h,',
            '    int dst_stride_bytes,',
            '    int x,',
            '    int y,',
            '    uint8_t alpha,',
            '    uint8_t bg_r,',
            '    uint8_t bg_g,',
            '    uint8_t bg_b,',
            '    int scale',
            ');',
            '',
        ])
    
    lines.append(f'#endif /* {guard} */')
    lines.append('')
    
    return '\n'.join(lines)


def generate_source(name: str, width: int, height: int, pixels: bytes,
                    scale: int = None, alpha: bool = False) -> str:
    """Generate the .c source file content."""
    pixel_array = format_pixel_array(pixels)
    
    lines = [
        f'#include "{name}.h"',
        '#include <string.h>',
        '',
        f'const int {name}_w = {width};',
        f'const int {name}_h = {height};',
    ]
    
    if scale:
        lines.append(f'const int {name}_scaled_w = {width * scale};')
        lines.append(f'const int {name}_scaled_h = {height * scale};')
    
    lines.extend([
        '',
        f'static const uint8_t {name}_pixels[] = {{',
        pixel_array,
        '};',
        '',
    ])
    
    # Base blit function (memcpy-based, efficient)
    lines.extend([
        f'void {name}_blit_rgb888(',
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
    
    # Scaled blit function (pixel-by-pixel)
    if scale:
        lines.extend([
            f'void {name}_blit_rgb888_{scale}x(',
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
            f'    const int scale = {scale};',
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
            '    /* Blit pixel by pixel with scaling */',
            '    for (int dy = dst_y0; dy < dst_y1; dy++) {',
            '        /* Map destination Y to source Y */',
            '        int src_y_idx = (dy - y) / scale;',
            '        uint8_t *dst_row = dst + dy * dst_stride_bytes;',
            '',
            '        for (int dx = dst_x0; dx < dst_x1; dx++) {',
            '            /* Map destination X to source X */',
            '            int src_x_idx = (dx - x) / scale;',
            '',
            '            /* Get source pixel */',
            f'            const uint8_t *src_px = {name}_pixels + (src_y_idx * src_w + src_x_idx) * 3;',
            '',
            '            /* Write to destination */',
            '            uint8_t *dst_px = dst_row + dx * 3;',
            '            dst_px[0] = src_px[0];',
            '            dst_px[1] = src_px[1];',
            '            dst_px[2] = src_px[2];',
            '        }',
            '    }',
            '}',
            '',
        ])
    
    # Alpha blit function (with optional scaling support)
    if alpha:
        lines.extend([
            f'void {name}_blit_rgb888_alpha(',
            '    uint8_t *dst,',
            '    int dst_w,',
            '    int dst_h,',
            '    int dst_stride_bytes,',
            '    int x,',
            '    int y,',
            '    uint8_t alpha,',
            '    uint8_t bg_r,',
            '    uint8_t bg_g,',
            '    uint8_t bg_b,',
            '    int scale',
            ')',
            '{',
            f'    const int src_w = {name}_w;',
            f'    const int src_h = {name}_h;',
            '',
            '    /* Precompute inverse alpha (for background blend) */',
            '    const int inv_alpha = 255 - alpha;',
            '',
            '    /* Treat scale <= 1 as no scaling */',
            '    if (scale <= 1) {',
            '        /* Unscaled alpha blit */',
            '        int src_x = 0;',
            '        int src_y = 0;',
            '        int copy_w = src_w;',
            '        int copy_h = src_h;',
            '        int dst_x = x;',
            '        int dst_y = y;',
            '',
            '        /* Clip left edge */',
            '        if (dst_x < 0) {',
            '            src_x = -dst_x;',
            '            copy_w += dst_x;',
            '            dst_x = 0;',
            '        }',
            '',
            '        /* Clip top edge */',
            '        if (dst_y < 0) {',
            '            src_y = -dst_y;',
            '            copy_h += dst_y;',
            '            dst_y = 0;',
            '        }',
            '',
            '        /* Clip right edge */',
            '        if (dst_x + copy_w > dst_w) {',
            '            copy_w = dst_w - dst_x;',
            '        }',
            '',
            '        /* Clip bottom edge */',
            '        if (dst_y + copy_h > dst_h) {',
            '            copy_h = dst_h - dst_y;',
            '        }',
            '',
            '        /* If fully clipped, nothing to draw */',
            '        if (copy_w <= 0 || copy_h <= 0) {',
            '            return;',
            '        }',
            '',
            '        /* Blit pixel by pixel with alpha compositing */',
            '        const int src_stride = src_w * 3;',
            '',
            '        for (int j = 0; j < copy_h; j++) {',
            f'            const uint8_t *src_row = {name}_pixels + (src_y + j) * src_stride + (src_x * 3);',
            '            uint8_t *dst_row = dst + (dst_y + j) * dst_stride_bytes + (dst_x * 3);',
            '',
            '            for (int i = 0; i < copy_w; i++) {',
            '                const uint8_t *src_px = src_row + i * 3;',
            '                uint8_t *dst_px = dst_row + i * 3;',
            '',
            '                /* Composite: out = src * alpha + bg * (1 - alpha) */',
            '                dst_px[0] = (uint8_t)((src_px[0] * alpha + bg_r * inv_alpha + 127) / 255);',
            '                dst_px[1] = (uint8_t)((src_px[1] * alpha + bg_g * inv_alpha + 127) / 255);',
            '                dst_px[2] = (uint8_t)((src_px[2] * alpha + bg_b * inv_alpha + 127) / 255);',
            '            }',
            '        }',
            '    } else {',
            '        /* Scaled alpha blit */',
            '        const int scaled_w = src_w * scale;',
            '        const int scaled_h = src_h * scale;',
            '',
            '        /* Compute clipping in scaled coordinates */',
            '        int dst_x0 = x;',
            '        int dst_y0 = y;',
            '        int dst_x1 = x + scaled_w;',
            '        int dst_y1 = y + scaled_h;',
            '',
            '        /* Clip to destination bounds */',
            '        if (dst_x0 < 0) dst_x0 = 0;',
            '        if (dst_y0 < 0) dst_y0 = 0;',
            '        if (dst_x1 > dst_w) dst_x1 = dst_w;',
            '        if (dst_y1 > dst_h) dst_y1 = dst_h;',
            '',
            '        /* If fully clipped, nothing to draw */',
            '        if (dst_x0 >= dst_x1 || dst_y0 >= dst_y1) {',
            '            return;',
            '        }',
            '',
            '        /* Blit pixel by pixel with scaling and alpha compositing */',
            '        for (int dy = dst_y0; dy < dst_y1; dy++) {',
            '            /* Map destination Y to source Y */',
            '            int src_y_idx = (dy - y) / scale;',
            '            uint8_t *dst_row = dst + dy * dst_stride_bytes;',
            '',
            '            for (int dx = dst_x0; dx < dst_x1; dx++) {',
            '                /* Map destination X to source X */',
            '                int src_x_idx = (dx - x) / scale;',
            '',
            '                /* Get source pixel */',
            f'                const uint8_t *src_px = {name}_pixels + (src_y_idx * src_w + src_x_idx) * 3;',
            '',
            '                /* Write blended pixel to destination */',
            '                uint8_t *dst_px = dst_row + dx * 3;',
            '                dst_px[0] = (uint8_t)((src_px[0] * alpha + bg_r * inv_alpha + 127) / 255);',
            '                dst_px[1] = (uint8_t)((src_px[1] * alpha + bg_g * inv_alpha + 127) / 255);',
            '                dst_px[2] = (uint8_t)((src_px[2] * alpha + bg_b * inv_alpha + 127) / 255);',
            '            }',
            '        }',
            '    }',
            '}',
            '',
        ])
    
    return '\n'.join(lines)


def convert_png_to_blit(input_path: str, name: str, outdir: str,
                        scale: int = None, alpha: bool = False) -> None:
    """
    Convert a PNG image to C source files with embedded RGB888 data
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
    pixels = img.tobytes()
    
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
    header_content = generate_header(c_name, width, height, scale, alpha)
    source_content = generate_source(c_name, width, height, pixels, scale, alpha)
    
    # Write files
    with open(header_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(header_content)
    
    with open(source_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(source_content)
    
    # Print summary
    total_bytes = len(pixels)
    print(f"Input:  {input_path}")
    print(f"Size:   {width}x{height} pixels")
    print(f"Mode:   {original_mode} -> RGB")
    
    functions = ['blit_rgb888']
    if scale:
        print(f"Scale:  {scale}x ({width * scale}x{height * scale} scaled)")
        functions.append(f'blit_rgb888_{scale}x')
    if alpha:
        functions.append('blit_rgb888_alpha')
    
    print(f"Funcs:  {', '.join(functions)}")
    print(f"Output: {header_path}")
    print(f"        {source_path}")
    print(f"Bytes:  {total_bytes} ({total_bytes / 1024:.1f} KB)")


def main():
    parser = argparse.ArgumentParser(
        description='Convert PNG image to C source files with RGB888 blit function.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
    python png2blit.py logo.png --name logo --outdir ./gen
    python png2blit.py logo.png --name logo --outdir ./gen --scale 2
    python png2blit.py logo.png --name logo --outdir ./gen --scale 3 --alpha

This generates:
    ./gen/logo.h  - Header with extern declarations and function prototypes
    ./gen/logo.c  - Source with pixel data and blit implementations

Optional features:
    --scale N   Adds logo_blit_rgb888_Nx() for scaled blitting (N = 2-16)
    --alpha     Adds logo_blit_rgb888_alpha() for alpha-blended blitting
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
    
    parser.add_argument(
        '--scale', '-s',
        type=int,
        default=None,
        choices=range(2, 17),
        metavar='N',
        help='Generate additional scaled blit function (N = 2-16)'
    )
    
    parser.add_argument(
        '--alpha', '-a',
        action='store_true',
        help='Generate alpha-blending blit function'
    )
    
    args = parser.parse_args()
    
    # Derive name from input filename if not provided
    if args.name is None:
        args.name = os.path.splitext(os.path.basename(args.input))[0]
    
    convert_png_to_blit(args.input, args.name, args.outdir, args.scale, args.alpha)


if __name__ == '__main__':
    main()
