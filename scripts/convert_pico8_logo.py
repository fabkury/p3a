#!/usr/bin/env python3
"""
Convert PICO-8 logo GIF to C header file
"""
import sys
from PIL import Image

def convert_gif_to_header(gif_path, output_path):
    # Open and convert to RGBA
    img = Image.open(gif_path)
    img = img.convert('RGBA')
    
    width, height = img.size
    pixels = img.load()
    
    # Generate C array
    output = []
    output.append("#pragma once\n")
    output.append("#include <stdint.h>\n")
    output.append("#include <stddef.h>\n\n")
    output.append(f"#define PICO8_LOGO_WIDTH {width}\n")
    output.append(f"#define PICO8_LOGO_HEIGHT {height}\n")
    output.append(f"#define PICO8_LOGO_SIZE ({width} * {height} * 4)\n\n")
    output.append("static const uint8_t pico8_logo_data[] = {\n")
    
    byte_count = 0
    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            output.append(f"    0x{r:02x}, 0x{g:02x}, 0x{b:02x}, 0x{a:02x},")
            byte_count += 4
            if byte_count % 16 == 0:
                output.append("\n")
    
    if byte_count % 16 != 0:
        output.append("\n")
    
    output.append("};\n")
    
    with open(output_path, 'w') as f:
        f.write(''.join(output))
    
    print(f"Converted {gif_path} ({width}x{height}) to {output_path}")
    print(f"Total bytes: {byte_count}")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: convert_pico8_logo.py <input.gif> <output.h>")
        sys.exit(1)
    
    convert_gif_to_header(sys.argv[1], sys.argv[2])

