#!/bin/bash
# Test script for P3A image upload endpoint
# Tests the POST /upload/image endpoint with a sample image

# Configuration
P3A_HOST="${1:-p3a.local}"  # Default to p3a.local, can pass IP as argument
TEST_IMAGE="${2:-test_image.jpg}"

echo "==================================="
echo "P3A Upload Endpoint Test"
echo "==================================="
echo "Target: http://$P3A_HOST/upload/image"
echo "Test image: $TEST_IMAGE"
echo ""

# Check if test image exists
if [ ! -f "$TEST_IMAGE" ]; then
    echo "Test image not found. Creating a 720x720 test pattern..."

    # Check if ImageMagick is available
    if command -v convert &> /dev/null; then
        # Create a simple test pattern using ImageMagick
        convert -size 720x720 \
                -background "rgb(255,0,255)" \
                -fill white \
                -pointsize 72 \
                -gravity center \
                label:"P3A Test" \
                "$TEST_IMAGE"
        echo "✓ Created test image: $TEST_IMAGE"
    else
        echo "ERROR: Test image not found and ImageMagick not installed"
        echo "Please provide a test image as second argument:"
        echo "  ./test_upload.sh p3a.local /path/to/image.jpg"
        exit 1
    fi
fi

# Get file size
FILE_SIZE=$(stat -f%z "$TEST_IMAGE" 2>/dev/null || stat -c%s "$TEST_IMAGE" 2>/dev/null)
echo "File size: $FILE_SIZE bytes"
echo ""

# Ping test
echo "Testing connectivity..."
if ping -c 1 -W 2 "$P3A_HOST" &> /dev/null; then
    echo "✓ Host $P3A_HOST is reachable"
else
    echo "✗ Host $P3A_HOST is not reachable"
    echo "  Make sure P3A is powered on and connected to WiFi"
    exit 1
fi
echo ""

# HTTP status check
echo "Checking P3A HTTP server..."
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://$P3A_HOST/" --connect-timeout 5)
if [ "$HTTP_CODE" = "200" ]; then
    echo "✓ HTTP server is running (status: $HTTP_CODE)"
else
    echo "✗ HTTP server check failed (status: $HTTP_CODE)"
    echo "  Make sure P3A firmware is running"
    exit 1
fi
echo ""

# Upload test
echo "Uploading image..."
echo "-----------------------------------"

RESPONSE=$(curl -s -w "\n%{http_code}" \
    -X POST \
    -H "Content-Type: image/jpeg" \
    --data-binary "@$TEST_IMAGE" \
    "http://$P3A_HOST/upload/image" \
    --connect-timeout 10)

HTTP_CODE=$(echo "$RESPONSE" | tail -n 1)
BODY=$(echo "$RESPONSE" | sed '$d')

echo "HTTP Status: $HTTP_CODE"
echo "Response:"
echo "$BODY" | python3 -m json.tool 2>/dev/null || echo "$BODY"
echo "-----------------------------------"
echo ""

if [ "$HTTP_CODE" = "200" ]; then
    echo "✓ Upload successful!"
    echo ""
    echo "Image should now be displayed on P3A."
    echo "Check the device or visit: http://$P3A_HOST/"
    exit 0
else
    echo "✗ Upload failed!"
    echo ""
    echo "Troubleshooting:"
    echo "1. Check P3A logs for errors"
    echo "2. Verify SD card is mounted at /sdcard/animations/"
    echo "3. Check free space on SD card"
    echo "4. Try with a smaller image file"
    exit 1
fi
