"""
p3a Flasher - Standalone GUI tool for flashing p3a firmware

A user-friendly application that bundles Python and esptool to flash
the p3a Physical Pixel Art Player firmware to ESP32-P4 devices.

Usage:
    python p3a_flasher.py
    
Or run the standalone executable (p3a-flasher.exe) after building.
"""

import sys
import os
import threading
import queue
import tempfile
import zipfile
import io
import shutil
from pathlib import Path

# GUI imports
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# Serial and flashing imports
import serial.tools.list_ports
import requests

# =============================================================================
# Version - This is replaced by the build script when embedding firmware
# =============================================================================
EMBEDDED_VERSION = None  # Set to version string when firmware is embedded

# For bundled executable - find resources
def resource_path(relative_path):
    """Get absolute path to resource, works for dev and for PyInstaller"""
    if hasattr(sys, '_MEIPASS'):
        return os.path.join(sys._MEIPASS, relative_path)
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), relative_path)


def get_embedded_firmware_dir():
    """Get path to embedded firmware directory, or None if not embedded."""
    firmware_dir = resource_path('firmware')
    if os.path.isdir(firmware_dir):
        # Check if all required files exist
        required = ['bootloader.bin', 'partition-table.bin', 'ota_data_initial.bin',
                   'p3a.bin', 'storage.bin', 'network_adapter.bin']
        if all(os.path.exists(os.path.join(firmware_dir, f)) for f in required):
            return firmware_dir
    return None


# =============================================================================
# Configuration
# =============================================================================

CONFIG = {
    'github_repo': 'fabkury/p3a',
    'chip': 'esp32p4',
    'baud_rate': 460800,
    'flash_mode': 'dio',
    'flash_freq': '80m',
    'flash_size': '32MB',
    'flash_addresses': {
        'bootloader.bin': 0x2000,
        'partition-table.bin': 0x8000,
        'ota_data_initial.bin': 0x10000,
        'p3a.bin': 0x20000,
        'storage.bin': 0x1020000,
        'network_adapter.bin': 0x1420000,  # Must match slave_fw partition in partitions.csv
    },
    'required_files': [
        'bootloader.bin',
        'partition-table.bin', 
        'ota_data_initial.bin',
        'p3a.bin',
        'storage.bin',
        'network_adapter.bin',
    ],
    # USB Vendor/Product IDs for auto-detection
    'known_vids': [0x303a, 0x10c4, 0x1a86, 0x0403],  # Espressif, CP210x, CH340, FTDI
}


# =============================================================================
# Flashing Logic
# =============================================================================

class FlashWorker:
    """Worker class that handles the flashing process in a background thread."""
    
    def __init__(self, port, firmware_dir, log_callback, done_callback):
        self.port = port
        self.firmware_dir = firmware_dir
        self.log_callback = log_callback
        self.done_callback = done_callback
        self.cancelled = False
        
    def flash(self):
        """Main flashing method - runs in background thread."""
        try:
            self.log_callback("Starting flash process...")
            self.log_callback(f"Port: {self.port}")
            self.log_callback(f"Chip: {CONFIG['chip']}")
            self.log_callback(f"Flash mode: {CONFIG['flash_mode']}, freq: {CONFIG['flash_freq']}, size: {CONFIG['flash_size']}")
            
            # Import esptool here to avoid slow startup
            self.log_callback("Loading esptool...")
            import esptool
            
            if self.cancelled:
                self.done_callback(False, "Cancelled by user")
                return
            
            # Build esptool command arguments
            self.log_callback("\nConnecting to device...")
            
            # Build the flash command
            flash_args = [
                '--chip', CONFIG['chip'],
                '--port', self.port,
                '--baud', str(CONFIG['baud_rate']),
                '--before', 'default_reset',
                '--after', 'hard_reset',
                'write_flash',
                '--flash_mode', CONFIG['flash_mode'],
                '--flash_freq', CONFIG['flash_freq'],
                '--flash_size', CONFIG['flash_size'],
                '--force',  # Required for network_adapter.bin (ESP32-C6 firmware)
            ]
            
            # Add files with their addresses
            for filename in CONFIG['required_files']:
                file_path = os.path.join(self.firmware_dir, filename)
                if os.path.exists(file_path):
                    addr = CONFIG['flash_addresses'][filename]
                    flash_args.extend([hex(addr), file_path])
                else:
                    self.log_callback(f"WARNING: {filename} not found")
            
            self.log_callback(f"\nFlashing {len(CONFIG['required_files'])} files...")
            
            # Capture esptool output
            class OutputCapture:
                def __init__(self, callback):
                    self.callback = callback
                    self.buffer = ""
                    
                def write(self, text):
                    self.buffer += text
                    while '\n' in self.buffer:
                        line, self.buffer = self.buffer.split('\n', 1)
                        if line.strip():
                            self.callback(line)
                                    
                def flush(self):
                    pass
            
            output_capture = OutputCapture(self.log_callback)
            old_stdout = sys.stdout
            old_stderr = sys.stderr
            
            try:
                sys.stdout = output_capture
                sys.stderr = output_capture
                esptool.main(flash_args)
            finally:
                sys.stdout = old_stdout
                sys.stderr = old_stderr
            
            self.log_callback("\n" + "="*50)
            self.log_callback("✓ Flash complete!")
            self.log_callback("The device should now reboot into p3a. Enjoy!")
            self.log_callback("="*50)
            self.done_callback(True, "Flash completed successfully!")
            
        except Exception as e:
            error_msg = str(e)
            self.log_callback(f"\n✗ Error: {error_msg}")
            self.done_callback(False, error_msg)


# =============================================================================
# Main Application
# =============================================================================

class P3AFlasher(tk.Tk):
    """Main application window."""
    
    def __init__(self):
        super().__init__()
        
        self.title("p3a Flasher")
        self.geometry("540x700")
        self.minsize(500, 640)
        self.configure(bg='#1a1a2e')

        # Set window icon
        try:
            from PIL import Image, ImageTk
            icon_path = resource_path('p3a_icon.ico')
            if os.path.exists(icon_path):
                # Load ICO with PIL and convert to PhotoImage
                icon_img = Image.open(icon_path)
                # Create multiple sizes for better display
                self._icon_photo_32 = ImageTk.PhotoImage(icon_img.resize((32, 32), Image.Resampling.LANCZOS))
                self._icon_photo_16 = ImageTk.PhotoImage(icon_img.resize((16, 16), Image.Resampling.LANCZOS))
                self.iconphoto(True, self._icon_photo_32, self._icon_photo_16)
        except Exception:
            pass  # Ignore icon errors
        
        # State
        self.firmware_dir = None
        self.releases = []
        self.selected_release = tk.StringVar()
        self.selected_port = tk.StringVar()
        self.firmware_source = tk.StringVar(value='embedded' if get_embedded_firmware_dir() else 'github')
        self.local_file_path = tk.StringVar()
        self.is_flashing = False
        self.flash_thread = None
        self.device_connected = False
        self.ports_map = {}
        self.detected_port = None
        
        # Message queue for thread communication
        self.msg_queue = queue.Queue()
        
        # Setup UI
        self._setup_styles()
        self._create_widgets()
        self._fetch_releases()
        
        # Start port scanning
        self._scan_ports()
        
        # Start message processing
        self.after(100, self._process_messages)
        
        # Center window
        self.update_idletasks()
        x = (self.winfo_screenwidth() - self.winfo_width()) // 2
        y = (self.winfo_screenheight() - self.winfo_height()) // 2
        self.geometry(f"+{x}+{y}")
        
    def _setup_styles(self):
        """Configure ttk styles for dark theme."""
        style = ttk.Style()
        style.theme_use('clam')
        
        # Colors
        bg = '#1a1a2e'
        bg_light = '#252540'
        bg_card = '#2a2a45'
        fg = '#eaeaea'
        fg_muted = '#888899'
        accent = '#4a9eff'
        success = '#4ade80'
        warning = '#fbbf24'
        
        # Frame styles
        style.configure('Dark.TFrame', background=bg)
        style.configure('Card.TFrame', background=bg_card)
        
        # Label styles
        style.configure('Dark.TLabel', background=bg, foreground=fg, font=('Segoe UI', 10))
        style.configure('Title.TLabel', background=bg, foreground=fg, font=('Segoe UI', 18, 'bold'))
        style.configure('Subtitle.TLabel', background=bg, foreground=accent, font=('Segoe UI', 9))
        style.configure('Status.TLabel', background=bg_card, foreground=fg_muted, font=('Segoe UI', 10))
        style.configure('StatusReady.TLabel', background=bg_card, foreground=success, font=('Segoe UI', 10, 'bold'))
        style.configure('StatusWaiting.TLabel', background=bg_card, foreground=warning, font=('Segoe UI', 10))
        style.configure('Card.TLabel', background=bg_card, foreground=fg, font=('Segoe UI', 10))
        style.configure('CardMuted.TLabel', background=bg_card, foreground=fg_muted, font=('Segoe UI', 9))
        style.configure('Footer.TLabel', background=bg, foreground=fg_muted, font=('Segoe UI', 9))
        
        # Button styles
        style.configure('Accent.TButton', font=('Segoe UI', 11, 'bold'), padding=(20, 10))
        style.map('Accent.TButton',
                  background=[('disabled', '#555'), ('active', accent), ('!active', accent)],
                  foreground=[('disabled', '#888'), ('active', 'white'), ('!active', 'white')])
        
        style.configure('Small.TButton', font=('Segoe UI', 9), padding=(8, 4))
        
        # Combobox - remove hover highlight issue
        style.configure('Dark.TCombobox', padding=5, foreground=fg)
        style.map('Dark.TCombobox',
                  fieldbackground=[('readonly', bg_card)],
                  selectbackground=[('readonly', bg_card)],
                  selectforeground=[('readonly', fg)],
                  foreground=[('readonly', fg)])

        # Configure the Combobox dropdown list colors (uses Tk option database)
        self.option_add('*TCombobox*Listbox.background', bg_card)
        self.option_add('*TCombobox*Listbox.foreground', fg)
        self.option_add('*TCombobox*Listbox.selectBackground', accent)
        self.option_add('*TCombobox*Listbox.selectForeground', fg)
        
        # Radiobutton - fix hover highlight
        style.configure('Card.TRadiobutton', background=bg_card, foreground=fg, font=('Segoe UI', 10))
        style.map('Card.TRadiobutton',
                  background=[('active', bg_card), ('!active', bg_card)],
                  foreground=[('active', fg), ('!active', fg)])
        
    def _create_widgets(self):
        """Create all UI widgets."""
        # Main container
        main_frame = ttk.Frame(self, style='Dark.TFrame', padding=25)
        main_frame.pack(fill='both', expand=True)
        
        # Header with logo
        header_frame = ttk.Frame(main_frame, style='Dark.TFrame')
        header_frame.pack(fill='x', pady=(0, 5))
        
        # Logo
        try:
            from PIL import Image, ImageTk
            logo_path = resource_path('p3a_logo.png')
            img = Image.open(logo_path)
            img = img.resize((72, 72), Image.Resampling.NEAREST)
            self.logo_image = ImageTk.PhotoImage(img)
            logo_label = tk.Label(header_frame, image=self.logo_image, bg='#1a1a2e')
            logo_label.pack()
        except Exception:
            logo_label = ttk.Label(header_frame, text="p3a", style='Title.TLabel', 
                                  font=('Segoe UI', 28, 'bold'))
            logo_label.pack()
        
        # Title
        title_label = ttk.Label(header_frame, text="p3a Flasher", style='Title.TLabel')
        title_label.pack(pady=(5, 0))
        
        # GitHub link
        github_label = ttk.Label(header_frame, text="https://github.com/fabkury/p3a", 
                                style='Subtitle.TLabel', cursor='hand2')
        github_label.pack(pady=(2, 0))
        github_label.bind('<Button-1>', lambda e: self._open_url('https://github.com/fabkury/p3a'))
        
        # Options frame
        options_frame = ttk.Frame(main_frame, style='Card.TFrame', padding=12)
        options_frame.pack(fill='x', pady=(15, 10))
        
        # Serial Port row
        port_row = ttk.Frame(options_frame, style='Card.TFrame')
        port_row.pack(fill='x', pady=(0, 8))
        
        ttk.Label(port_row, text="Port:", style='Card.TLabel', width=12).pack(side='left')
        self.port_combo = ttk.Combobox(port_row, textvariable=self.selected_port,
                                       state='readonly', width=28, style='Dark.TCombobox')
        self.port_combo.pack(side='left', padx=(0, 8))
        ttk.Button(port_row, text="↻", style='Small.TButton', width=3,
                  command=self._scan_ports).pack(side='left')

        # Status section (device detected message)
        status_frame = ttk.Frame(options_frame, style='Card.TFrame')
        status_frame.pack(fill='x', pady=(0, 8))

        # Empty label for alignment with "Port:" label
        ttk.Label(status_frame, text="", style='Card.TLabel', width=12).pack(side='left')
        self.status_var = tk.StringVar(value="Looking for p3a device...")
        self.status_label = ttk.Label(status_frame, textvariable=self.status_var,
                                      style='StatusWaiting.TLabel')
        self.status_label.pack(side='left')

        # Firmware Source row
        source_row = ttk.Frame(options_frame, style='Card.TFrame')
        source_row.pack(fill='x', pady=(0, 5))
        
        ttk.Label(source_row, text="Firmware:", style='Card.TLabel', width=12).pack(side='left')
        
        source_options = ttk.Frame(source_row, style='Card.TFrame')
        source_options.pack(side='left', fill='x')
        
        # Embedded firmware option (if available)
        embedded_dir = get_embedded_firmware_dir()
        if embedded_dir:
            version_text = EMBEDDED_VERSION if EMBEDDED_VERSION else "bundled"
            self.embedded_radio = ttk.Radiobutton(source_options, 
                text=f"Embedded ({version_text})", 
                variable=self.firmware_source, value='embedded',
                command=self._on_source_change, style='Card.TRadiobutton')
            self.embedded_radio.pack(anchor='w')
        
        self.github_radio = ttk.Radiobutton(source_options, text="GitHub Release", 
                       variable=self.firmware_source, value='github',
                       command=self._on_source_change, style='Card.TRadiobutton')
        self.github_radio.pack(anchor='w')
        
        self.local_radio = ttk.Radiobutton(source_options, text="Local ZIP file", 
                       variable=self.firmware_source, value='local',
                       command=self._on_source_change, style='Card.TRadiobutton')
        self.local_radio.pack(anchor='w')
        
        # GitHub version selection (shown conditionally)
        self.github_frame = ttk.Frame(options_frame, style='Card.TFrame')
        
        github_row = ttk.Frame(self.github_frame, style='Card.TFrame')
        github_row.pack(fill='x')
        ttk.Label(github_row, text="Version:", style='Card.TLabel', width=12).pack(side='left')
        self.release_combo = ttk.Combobox(github_row, textvariable=self.selected_release,
                                          state='readonly', width=28, style='Dark.TCombobox')
        self.release_combo.pack(side='left')
        
        # Local file selection (shown conditionally)
        self.local_frame = ttk.Frame(options_frame, style='Card.TFrame')
        
        local_row = ttk.Frame(self.local_frame, style='Card.TFrame')
        local_row.pack(fill='x')
        ttk.Label(local_row, text="File:", style='Card.TLabel', width=12).pack(side='left')
        self.local_entry = ttk.Entry(local_row, textvariable=self.local_file_path, width=24)
        self.local_entry.pack(side='left', padx=(0, 8))
        ttk.Button(local_row, text="Browse...", style='Small.TButton',
                  command=self._browse_file).pack(side='left')
        
        # Show appropriate sub-frame
        self._on_source_change()

        # Flash button (below firmware selection)
        flash_frame = ttk.Frame(options_frame, style='Card.TFrame')
        flash_frame.pack(fill='x', pady=(10, 0))

        # Empty label for alignment
        ttk.Label(flash_frame, text="", style='Card.TLabel', width=12).pack(side='left')
        self.flash_btn = ttk.Button(flash_frame, text="⚡  Flash Device",
                                   command=self._start_flash, style='Accent.TButton',
                                   width=22)
        self.flash_btn.pack(side='left')
        self.flash_btn.configure(state='disabled')

        # Info text area before console
        info_frame = ttk.Frame(main_frame, style='Dark.TFrame')
        info_frame.pack(fill='x', pady=(10, 5))

        info_text1 = ttk.Label(info_frame,
            text="The flashing process takes about 4 minutes. At the end, the device will automatically",
            style='Footer.TLabel')
        info_text1.pack(anchor='w')
        info_text2 = ttk.Label(info_frame,
            text="reboot two times. The two reboots are necessary to update the two chips inside p3a.",
            style='Footer.TLabel')
        info_text2.pack(anchor='w')
        info_text3 = ttk.Label(info_frame,
            text="Flashing does not alter the SD card. You can re-flash p3a at any time.",
            style='Footer.TLabel', padding=(0, 5, 0, 0))
        info_text3.pack(anchor='w')

        # Console section
        console_frame = ttk.Frame(main_frame, style='Card.TFrame', padding=10)
        console_frame.pack(fill='both', expand=True, pady=(5, 10))
        
        ttk.Label(console_frame, text="Output", style='Card.TLabel',
                 font=('Segoe UI', 10, 'bold')).pack(anchor='w', pady=(0, 5))
        
        console_inner = ttk.Frame(console_frame, style='Card.TFrame')
        console_inner.pack(fill='both', expand=True)
        
        self.console = tk.Text(console_inner, bg='#0d0d1a', fg='#aaaaaa',
                              font=('Consolas', 9), wrap='word', state='disabled',
                              relief='flat', padx=8, pady=8, height=10)
        scrollbar = ttk.Scrollbar(console_inner, orient='vertical', command=self.console.yview)
        self.console.configure(yscrollcommand=scrollbar.set)
        
        self.console.pack(side='left', fill='both', expand=True)
        scrollbar.pack(side='right', fill='y')
        
        # Footer
        footer_frame = ttk.Frame(main_frame, style='Dark.TFrame')
        footer_frame.pack(fill='x', pady=(5, 0))
        
        footer_label = ttk.Label(footer_frame, text="❤️ pixel art, ❤️ pixel artists", 
                                style='Footer.TLabel')
        footer_label.pack()
        
    def _open_url(self, url):
        """Open URL in default browser."""
        import webbrowser
        webbrowser.open(url)
        
    def _on_source_change(self):
        """Handle firmware source radio button change."""
        source = self.firmware_source.get()
        
        # Hide all conditional frames
        self.github_frame.pack_forget()
        self.local_frame.pack_forget()
        
        # Show the appropriate frame
        if source == 'github':
            self.github_frame.pack(fill='x', pady=(8, 0))
        elif source == 'local':
            self.local_frame.pack(fill='x', pady=(8, 0))
        # 'embedded' doesn't need additional UI
            
    def _scan_ports(self):
        """Scan for available serial ports and check for ESP32 device."""
        ports = []
        self.ports_map = {}
        esp_found = False
        
        for port in serial.tools.list_ports.comports():
            desc = f"{port.device}"
            if port.description and port.description != 'n/a':
                desc += f" — {port.description}"
            ports.append((port.device, desc, port.vid))
            self.ports_map[desc] = port.device
            
            if port.vid in CONFIG['known_vids']:
                esp_found = True
                self.detected_port = port.device
                self.selected_port.set(desc)
        
        if ports:
            self.port_combo['values'] = [p[1] for p in ports]
            if not esp_found and ports:
                self.selected_port.set(ports[0][1])
        else:
            self.port_combo['values'] = ['No ports found']
            self.selected_port.set('No ports found')
        
        self._update_device_status(esp_found)
        
        if not self.is_flashing:
            self.after(2000, self._scan_ports)
            
    def _update_device_status(self, connected):
        """Update the UI based on device connection status."""
        if self.is_flashing:
            return
            
        self.device_connected = connected
        
        if connected:
            port_name = self.detected_port if self.detected_port else "unknown"
            self.status_var.set(f"✓ p3a device detected on {port_name}")
            self.status_label.configure(style='StatusReady.TLabel')
            self.flash_btn.configure(state='normal')
        else:
            self.status_var.set("Connect your p3a via USB...")
            self.status_label.configure(style='StatusWaiting.TLabel')
            self.flash_btn.configure(state='disabled')
            
    def _fetch_releases(self):
        """Fetch available releases from GitHub."""
        def fetch():
            try:
                url = f"https://api.github.com/repos/{CONFIG['github_repo']}/releases"
                response = requests.get(url, timeout=10)
                if response.status_code == 200:
                    releases = response.json()
                    # Filter to releases that have all required firmware files
                    valid_releases = []
                    for r in releases:
                        asset_names = [a['name'] for a in r.get('assets', [])]
                        # Check if all required .bin files are present
                        if all(f in asset_names for f in CONFIG['required_files']):
                            valid_releases.append(r)
                    self.msg_queue.put(('releases', valid_releases))
                else:
                    self.msg_queue.put(('releases_error', f"HTTP {response.status_code}"))
            except Exception as e:
                self.msg_queue.put(('releases_error', str(e)))

        threading.Thread(target=fetch, daemon=True).start()
        
    def _browse_file(self):
        """Open file browser for local firmware."""
        path = filedialog.askopenfilename(
            title="Select Firmware ZIP",
            filetypes=[("ZIP files", "*.zip"), ("All files", "*.*")]
        )
        if path:
            self.local_file_path.set(path)
            
    def _log(self, message):
        """Add message to console."""
        self.console.configure(state='normal')
        self.console.insert('end', message + '\n')
        self.console.see('end')
        self.console.configure(state='disabled')
            
    def _process_messages(self):
        """Process messages from background threads."""
        try:
            while True:
                msg_type, data = self.msg_queue.get_nowait()
                
                if msg_type == 'releases':
                    self.releases = data
                    if data:
                        self.release_combo['values'] = [
                            f"{r['tag_name']}{' (latest)' if i == 0 else ''}"
                            for i, r in enumerate(data)
                        ]
                        self.selected_release.set(self.release_combo['values'][0])
                    else:
                        self.release_combo['values'] = ['No releases found']
                        
                elif msg_type == 'releases_error':
                    self.release_combo['values'] = ['Failed to load releases']
                    self._log(f"Failed to fetch releases: {data}")
                    
                elif msg_type == 'log':
                    self._log(data)
                    
                elif msg_type == 'done':
                    success, message = data
                    self.is_flashing = False
                    self.flash_btn.configure(state='normal', text="⚡  Flash Device")
                    self._scan_ports()
                    
                    # Cleanup temp directory (but not embedded firmware dir)
                    if self.firmware_dir and os.path.exists(self.firmware_dir):
                        embedded = get_embedded_firmware_dir()
                        if not embedded or self.firmware_dir != embedded:
                            try:
                                shutil.rmtree(self.firmware_dir, ignore_errors=True)
                            except:
                                pass
                        self.firmware_dir = None
                    
                    if success:
                        self._update_device_status(False)
                        self.status_var.set("✓ Flash complete!")
                        # No popup - user can see success in console
                    else:
                        self._update_device_status(self.device_connected)
                        messagebox.showerror("Error", f"Flash failed:\n{message}")
                        
        except queue.Empty:
            pass
            
        self.after(100, self._process_messages)
        
    def _start_flash(self):
        """Start the flashing process."""
        if self.is_flashing:
            return
            
        # Get selected port
        port_desc = self.selected_port.get()
        if not port_desc or port_desc == 'No ports found':
            messagebox.showerror("Error", "Please select a serial port.")
            return
            
        port = self.ports_map.get(port_desc)
        if not port:
            messagebox.showerror("Error", "Invalid port selection.")
            return
            
        # Get firmware based on source
        source = self.firmware_source.get()
        
        if source == 'embedded':
            embedded_dir = get_embedded_firmware_dir()
            if embedded_dir:
                self._do_flash(port, embedded_dir)
            else:
                messagebox.showerror("Error", "Embedded firmware not found.")
                
        elif source == 'github':
            if not self.releases:
                messagebox.showerror("Error", "No releases available. Check your internet connection.")
                return
                
            release_str = self.selected_release.get()
            release = None
            for r in self.releases:
                if r['tag_name'] in release_str:
                    release = r
                    break
                    
            if not release:
                messagebox.showerror("Error", "Please select a release version.")
                return
                
            self._download_and_flash(port, release)
            
        else:  # local
            local_path = self.local_file_path.get()
            if not local_path or not os.path.exists(local_path):
                messagebox.showerror("Error", "Please select a valid firmware ZIP file.")
                return
                
            self._load_local_and_flash(port, local_path)
            
    def _download_and_flash(self, port, release):
        """Download firmware from GitHub and flash."""
        self.is_flashing = True
        self.flash_btn.configure(state='disabled', text="Downloading...")
        self.status_label.configure(style='Status.TLabel')
        self.status_var.set("Downloading firmware...")
        self._log(f"\nDownloading {release['tag_name']}...")

        def download():
            try:
                # Create temp directory for firmware files
                temp_dir = tempfile.mkdtemp(prefix='p3a_firmware_')
                self.firmware_dir = temp_dir

                # Build a map of asset names to download URLs
                assets_map = {a['name']: a['browser_download_url']
                              for a in release.get('assets', [])}

                # Download each required firmware file
                for filename in CONFIG['required_files']:
                    if filename not in assets_map:
                        self.msg_queue.put(('done', (False, f"Missing file in release: {filename}")))
                        return

                    self.msg_queue.put(('log', f"Downloading {filename}..."))
                    response = requests.get(assets_map[filename], timeout=120)

                    if response.status_code != 200:
                        self.msg_queue.put(('done', (False, f"Download failed for {filename}: HTTP {response.status_code}")))
                        return

                    out_path = os.path.join(temp_dir, filename)
                    with open(out_path, 'wb') as f:
                        f.write(response.content)
                    self.msg_queue.put(('log', f"  ✓ {filename} ({len(response.content):,} bytes)"))

                self._do_flash(port, temp_dir)

            except Exception as e:
                self.msg_queue.put(('done', (False, str(e))))

        threading.Thread(target=download, daemon=True).start()
        
    def _load_local_and_flash(self, port, zip_path):
        """Load local firmware ZIP and flash."""
        self.is_flashing = True
        self.flash_btn.configure(state='disabled', text="Loading...")
        self.status_label.configure(style='Status.TLabel')
        self.status_var.set("Loading firmware...")
        
        def load():
            try:
                with open(zip_path, 'rb') as f:
                    firmware_dir = self._extract_zip(f)
                    
                if not firmware_dir:
                    self.msg_queue.put(('done', (False, "Failed to extract firmware files")))
                    return
                    
                self._do_flash(port, firmware_dir)
                
            except Exception as e:
                self.msg_queue.put(('done', (False, str(e))))
                
        threading.Thread(target=load, daemon=True).start()
        
    def _extract_zip(self, zip_file):
        """Extract firmware files from ZIP to a temp directory."""
        temp_dir = tempfile.mkdtemp(prefix='p3a_firmware_')
        self.firmware_dir = temp_dir
        
        try:
            with zipfile.ZipFile(zip_file) as zf:
                for filename in CONFIG['required_files']:
                    for name in zf.namelist():
                        if name.endswith(filename):
                            data = zf.read(name)
                            out_path = os.path.join(temp_dir, filename)
                            with open(out_path, 'wb') as f:
                                f.write(data)
                            self.msg_queue.put(('log', f"  ✓ {filename} ({len(data):,} bytes)"))
                            break
                    else:
                        self.msg_queue.put(('log', f"  ✗ {filename} not found"))
                        
        except Exception as e:
            self.msg_queue.put(('log', f"Error extracting ZIP: {e}"))
            return None
            
        missing = []
        for f in CONFIG['required_files']:
            if not os.path.exists(os.path.join(temp_dir, f)):
                missing.append(f)
                
        if missing:
            self.msg_queue.put(('log', f"Missing files: {', '.join(missing)}"))
            return None
            
        return temp_dir
        
    def _do_flash(self, port, firmware_dir):
        """Execute the flash operation."""
        self.firmware_dir = firmware_dir
        self.is_flashing = True
        self.flash_btn.configure(state='disabled', text="Flashing...")
        self.status_label.configure(style='Status.TLabel')
        self.status_var.set("Flashing...")
        
        # Clear console
        self.console.configure(state='normal')
        self.console.delete('1.0', 'end')
        self.console.configure(state='disabled')
        
        def log_cb(message):
            self.msg_queue.put(('log', message))
            
        def done_cb(success, message):
            self.msg_queue.put(('done', (success, message)))
            
        worker = FlashWorker(port, firmware_dir, log_cb, done_cb)
        self.flash_thread = threading.Thread(target=worker.flash, daemon=True)
        self.flash_thread.start()
        
    def destroy(self):
        """Clean up on window close."""
        if self.firmware_dir and os.path.exists(self.firmware_dir):
            embedded = get_embedded_firmware_dir()
            if not embedded or self.firmware_dir != embedded:
                try:
                    shutil.rmtree(self.firmware_dir, ignore_errors=True)
                except:
                    pass
        super().destroy()


# =============================================================================
# Entry Point
# =============================================================================

def main():
    app = P3AFlasher()
    app.mainloop()


if __name__ == '__main__':
    main()
