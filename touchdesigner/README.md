# TouchDesigner to P3A Display Integration

This directory contains scripts for integrating TouchDesigner with the P3A pixel art display.

## Quick Start (Option 1: HTTP POST Upload)

### Prerequisites

1. **P3A Display** powered on and connected to WiFi
2. **TouchDesigner 2023+** installed
3. **Python packages** installed via tdPyEnvManager:
   - `requests`
   - `pillow`
   - `numpy`

### Installation Steps

**Step 1: Install Python Packages**

1. Open TouchDesigner
2. Open Palette (Alt+L)
3. Search for "tdPyEnvManager"
4. Drag to network and open the component
5. Install packages: `requests`, `pillow`, `numpy`

**Step 2: Build P3A Integration Network**

Run this in TouchDesigner textport:

```python
exec(open(r'touchdesigner/scripts/build_p3a_network.py').read())
```

This will create:
- `p3a_upload_timer` (Timer CHOP)
- `p3a_upload_execute` (Execute DAT with upload callback)

**Step 3: Configure Your Render TOP**

Make sure you have a TOP operator named `render_out` (or edit the script to use your TOP's name).

**Step 4: Start Uploading**

To start continuous uploads:

```python
op('p3a_upload_timer').par.start.pulse()
```

To stop:

```python
op('p3a_upload_timer').par.start = 0
```

### Manual Upload (Testing)

To manually upload a single frame:

```python
exec(open(r'touchdesigner/scripts/p3a_upload_http.py').read())
```

## Configuration

### Upload Rate

Change upload frequency (default: 2 Hz = 2 fps):

```python
op('p3a_upload_timer').par.timeout = 5  # 5 Hz = 5 fps
```

### Image Format & Quality

Edit `build_p3a_network.py` before running:

```python
ops = build_p3a_integration_network(
    render_top_name='your_top_name',  # Your render TOP
    upload_rate=2,                     # Upload rate in Hz
    ip_address='p3a.local',            # Or use IP like '192.168.1.100'
    format='JPEG',                     # PNG, JPEG, or WEBP
    quality=75                         # Lower = smaller files
)
```

Or edit the Execute DAT directly after creation.

## Troubleshooting

### "TOP 'render_out' not found"

Make sure your render TOP exists and has the correct name:

```python
# Find all TOPs in your project:
print([op.path for op in root.findChildren(type=TOP)])
```

### "No module named 'requests'"

Install Python packages via tdPyEnvManager (see Prerequisites).

### "Connection refused" or Timeout

- Verify P3A is on same WiFi network
- Ping device: `ping p3a.local`
- Try using IP address instead of hostname
- Check P3A web UI is accessible: http://p3a.local/

### Upload Too Slow

- Use JPEG instead of PNG (10× smaller)
- Lower quality: `quality=60` or `quality=50`
- Reduce upload rate: `upload_rate=1`
- Resize TOP to 512×512 before upload

## Performance Tips

| Use Case | Format | Quality | Rate | Notes |
|----------|--------|---------|------|-------|
| **High Quality** | PNG | N/A | 0.5 Hz | Lossless, slow |
| **Balanced** | JPEG | 85 | 2 Hz | Good quality |
| **Fast** | JPEG | 60 | 5 Hz | Lower quality, faster |

## File Structure

```
touchdesigner/
├── README.md                       # This file
├── scripts/
│   ├── build_p3a_network.py       # Network builder script
│   └── p3a_upload_http.py         # Manual upload script
└── examples/
    └── (coming soon: example .toe files)
```

## Next Steps

- **Option 2**: Direct memory upload (10-25 fps) - Coming soon
- **Option 3**: WebSocket streaming (20-50 fps) - Coming soon

## Support

See main project documentation at: `/TOUCHDESIGNER_INTEGRATION.md`

For issues, check the troubleshooting section in the main document.
