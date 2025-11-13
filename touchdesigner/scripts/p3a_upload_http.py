"""
P3A HTTP Upload Script for TouchDesigner
Sends TOP render output to P3A display via HTTP POST

Dependencies: requests, pillow, numpy (install via tdPyEnvManager)
Usage: exec(open(r'touchdesigner/scripts/p3a_upload_http.py').read())

Author: Generated for P3A Display Integration
License: Apache-2.0
"""
import requests
import numpy as np
from PIL import Image
import io

def upload_to_p3a(top_path='render_out', ip_address='p3a.local', format='JPEG', quality=85):
    """
    Upload current frame from TOP to P3A display

    Args:
        top_path: Path to TOP operator (relative or absolute)
        ip_address: P3A device IP or hostname (default: 'p3a.local')
        format: Image format - 'PNG', 'JPEG', or 'WEBP' (default: 'JPEG')
        quality: JPEG/WebP quality 0-100 (default: 85)

    Returns:
        bool: True if upload successful, False otherwise
    """
    try:
        # Get TOP operator
        top_op = op(top_path)
        if top_op is None:
            print(f"ERROR: TOP '{top_path}' not found")
            return False

        # Get image data from TOP (returns normalized float array)
        img_array = top_op.numpyArray(delayed=False)

        # Check if array is valid
        if img_array is None or img_array.size == 0:
            print(f"ERROR: TOP '{top_path}' has no image data")
            return False

        # Convert from float (0.0-1.0) to uint8 (0-255)
        # TouchDesigner uses bottom-left origin, flip vertically for standard image
        img_array = np.flipud(img_array)
        img_uint8 = (img_array * 255).astype(np.uint8)

        # Handle different channel counts (RGB vs RGBA)
        if img_uint8.shape[2] == 4:
            # RGBA - convert to RGB (P3A doesn't support transparency in upload)
            img = Image.fromarray(img_uint8[:, :, :3], mode='RGB')
        else:
            img = Image.fromarray(img_uint8, mode='RGB')

        # Resize to 720x720 if not already (P3A native resolution)
        if img.size != (720, 720):
            img = img.resize((720, 720), Image.LANCZOS)
            print(f"Resized from {top_op.width}x{top_op.height} to 720x720")

        # Encode to desired format
        buffer = io.BytesIO()
        if format.upper() == 'PNG':
            img.save(buffer, format='PNG', compress_level=6)
            content_type = 'image/png'
            filename = 'td_frame.png'
        elif format.upper() == 'JPEG':
            img.save(buffer, format='JPEG', quality=quality, optimize=True)
            content_type = 'image/jpeg'
            filename = 'td_frame.jpg'
        elif format.upper() == 'WEBP':
            img.save(buffer, format='WEBP', quality=quality)
            content_type = 'image/webp'
            filename = 'td_frame.webp'
        else:
            print(f"ERROR: Unsupported format '{format}'. Use PNG, JPEG, or WEBP")
            return False

        file_size = buffer.tell()
        buffer.seek(0)

        # HTTP POST upload (multipart/form-data)
        url = f'http://{ip_address}/upload/image'
        files = {'image': (filename, buffer, content_type)}

        print(f"Uploading {file_size} bytes to {url}...")
        response = requests.post(url, files=files, timeout=5)

        if response.ok:
            result = response.json()
            print(f"✓ Upload successful: {result}")
            return True
        else:
            print(f"✗ Upload failed: HTTP {response.status_code}")
            print(f"  Response: {response.text}")
            return False

    except Exception as e:
        print(f"✗ Upload error: {type(e).__name__}: {e}")
        import traceback
        traceback.print_exc()
        return False

# Run upload (modify these parameters as needed)
# To change settings, edit these values:
upload_to_p3a(
    top_path='render_out',      # Your render TOP name
    ip_address='p3a.local',      # P3A IP or hostname
    format='JPEG',               # PNG, JPEG, or WEBP
    quality=75                   # For JPEG/WebP only
)
