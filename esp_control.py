#!/usr/bin/env python3
"""
ESP Display Controller - Python Version
Supports both Serial and Bluetooth connectivity
Converts images/GIFs to matrix format and sends to ESP device
"""

import sys
import struct
import time
import argparse
from pathlib import Path
from typing import List, Tuple
import serial
import serial.tools.list_ports

try:
    from PIL import Image
    PIL_AVAILABLE = True
except ImportError:
    PIL_AVAILABLE = False
    print("Warning: PIL not installed. Install with: pip install Pillow")

try:
    import imageio
    IMAGEIO_AVAILABLE = True
except ImportError:
    IMAGEIO_AVAILABLE = False
    print("Warning: imageio not installed. Install with: pip install imageio")


# ============ PROTOCOL DEFINITIONS ============
START_BYTE = 0xFF
CMD_DISPLAY_IMAGE = 0x01
CMD_ANIMATION_START = 0x02


class ESPDisplayController:
    """Control ESP LED matrix display via Serial or Bluetooth"""
    
    def __init__(self, port: str = None, baudrate: int = 115200, timeout: float = 2.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial_connection = None
        self.matrix_width = 32
        self.matrix_height = 8
        
    def connect(self) -> bool:
        """Connect to ESP device"""
        try:
            self.serial_connection = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=self.timeout
            )
            print(f"✓ Connected to {self.port} at {self.baudrate} baud")
            time.sleep(2)  # Wait for ESP to boot
            return True
        except serial.SerialException as e:
            print(f"✗ Failed to connect: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from ESP device"""
        if self.serial_connection and self.serial_connection.is_open:
            self.serial_connection.close()
            print("✓ Disconnected")
    
    def send_data(self, data: bytes) -> bool:
        """Send raw bytes to ESP"""
        try:
            if not self.serial_connection or not self.serial_connection.is_open:
                print("✗ Not connected to device")
                return False
            
            self.serial_connection.write(data)
            
            # Wait for response
            time.sleep(0.1)
            if self.serial_connection.in_waiting:
                response = self.serial_connection.readline().decode('utf-8', errors='ignore')
                print(f"ESP: {response.strip()}")
            
            return True
        except serial.SerialException as e:
            print(f"✗ Send failed: {e}")
            return False
    
    def image_to_matrix(self, image_path: str) -> bytes:
        """Convert image to matrix format"""
        if not PIL_AVAILABLE:
            raise ImportError("PIL required for image processing")
        
        img = Image.open(image_path)
        
        # Resize to matrix dimensions
        img = img.resize((self.matrix_width, self.matrix_height), Image.Resampling.LANCZOS)
        
        # Convert to grayscale
        img = img.convert('L')
        
        # Convert to binary (threshold at 127)
        pixels = img.getdata()
        matrix = bytes([255 if p > 127 else 0 for p in pixels])
        
        return matrix
    
    def gif_to_frames(self, gif_path: str) -> List[bytes]:
        """Extract frames from GIF and convert to matrix format"""
        if not IMAGEIO_AVAILABLE:
            raise ImportError("imageio required for GIF processing")
        
        gif = imageio.mimread(gif_path)
        frames = []
        
        for frame_data in gif:
            img = Image.fromarray(frame_data)
            
            # Resize and convert
            img = img.resize((self.matrix_width, self.matrix_height), Image.Resampling.LANCZOS)
            img = img.convert('L')
            
            # Convert to binary
            pixels = img.getdata()
            matrix = bytes([255 if p > 127 else 0 for p in pixels])
            frames.append(matrix)
        
        return frames
    
    def create_display_packet(self, matrix_data: bytes) -> bytes:
        """Create display packet: [0xFF][CMD][WIDTH][HEIGHT][DATA...]"""
        packet = bytearray()
        packet.append(START_BYTE)
        packet.append(CMD_DISPLAY_IMAGE)
        packet.append(self.matrix_width)
        packet.append(self.matrix_height)
        packet.extend(matrix_data)
        return bytes(packet)
    
    def create_animation_packet(self, frame_count: int, delay_ms: int) -> bytes:
        """Create animation start packet: [0xFF][CMD][FRAMES][DELAY_H][DELAY_L]"""
        packet = bytearray()
        packet.append(START_BYTE)
        packet.append(CMD_ANIMATION_START)
        packet.append(frame_count)
        packet.append((delay_ms >> 8) & 0xFF)
        packet.append(delay_ms & 0xFF)
        return bytes(packet)
    
    def display_image(self, image_path: str) -> bool:
        """Send image to display"""
        try:
            print(f"Processing image: {image_path}")
            matrix = self.image_to_matrix(image_path)
            packet = self.create_display_packet(matrix)
            
            print(f"Sending packet ({len(packet)} bytes)...")
            return self.send_data(packet)
        except Exception as e:
            print(f"✗ Error: {e}")
            return False
    
    def display_gif(self, gif_path: str, delay_ms: int = 100) -> bool:
        """Send animated GIF to display"""
        try:
            print(f"Processing GIF: {gif_path}")
            frames = self.gif_to_frames(gif_path)
            print(f"Extracted {len(frames)} frames")
            
            # Send animation start command
            anim_packet = self.create_animation_packet(len(frames), delay_ms)
            self.send_data(anim_packet)
            
            # Send each frame
            for i, frame_data in enumerate(frames):
                packet = self.create_display_packet(frame_data)
                print(f"Sending frame {i+1}/{len(frames)}...")
                self.send_data(packet)
                time.sleep(delay_ms / 1000.0)
            
            return True
        except Exception as e:
            print(f"✗ Error: {e}")
            return False
    
    @staticmethod
    def list_ports() -> List[Tuple[str, str]]:
        """List available serial ports"""
        ports = []
        for port_info in serial.tools.list_ports.comports():
            ports.append((port_info.device, port_info.description))
        return ports
    
    @staticmethod
    def find_esp_port() -> str:
        """Try to auto-detect ESP device port"""
        ports = serial.tools.list_ports.comports()
        for port_info in ports:
            # Look for common ESP indicators
            desc = port_info.description.lower()
            if any(keyword in desc for keyword in ['esp', 'ch340', 'cp2102', 'ftdi']):
                return port_info.device
        
        # Return first available port if no ESP found
        if ports:
            return ports[0].device
        
        return None


def main():
    parser = argparse.ArgumentParser(
        description='ESP Matrix Display Controller',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # List available ports
  python esp_control.py --list-ports
  
  # Display image
  python esp_control.py -p COM3 -i image.jpg
  
  # Display animated GIF
  python esp_control.py -p COM3 -g animation.gif --delay 100
  
  # Auto-detect ESP port
  python esp_control.py -i image.jpg
        """
    )
    
    parser.add_argument('-p', '--port', help='Serial port (auto-detect if not specified)')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('-i', '--image', help='Display image file')
    parser.add_argument('-g', '--gif', help='Display animated GIF')
    parser.add_argument('-d', '--delay', type=int, default=100, help='GIF frame delay in ms (default: 100)')
    parser.add_argument('-w', '--width', type=int, default=32, help='Matrix width (default: 32)')
    parser.add_argument('-H', '--height', type=int, default=8, help='Matrix height (default: 8)')
    parser.add_argument('--list-ports', action='store_true', help='List available ports')
    parser.add_argument('--test', action='store_true', help='Test connection')
    
    args = parser.parse_args()
    
    # List ports
    if args.list_ports:
        print("Available serial ports:")
        ports = ESPDisplayController.list_ports()
        if ports:
            for port, desc in ports:
                print(f"  {port}: {desc}")
        else:
            print("  No ports found")
        return
    
    # Auto-detect port if not specified
    port = args.port or ESPDisplayController.find_esp_port()
    
    if not port:
        print("✗ No port specified and no ESP device found")
        print("Use --list-ports to see available options")
        return
    
    # Create controller
    controller = ESPDisplayController(port=port, baudrate=args.baud)
    controller.matrix_width = args.width
    controller.matrix_height = args.height
    
    # Connect
    if not controller.connect():
        return
    
    try:
        # Test connection
        if args.test:
            print("Testing connection...")
            time.sleep(1)
            print("✓ Connection test passed")
        
        # Display image
        if args.image:
            if not Path(args.image).exists():
                print(f"✗ File not found: {args.image}")
                return
            controller.display_image(args.image)
        
        # Display GIF
        if args.gif:
            if not Path(args.gif).exists():
                print(f"✗ File not found: {args.gif}")
                return
            controller.display_gif(args.gif, delay_ms=args.delay)
    
    finally:
        controller.disconnect()


if __name__ == '__main__':
    main()
