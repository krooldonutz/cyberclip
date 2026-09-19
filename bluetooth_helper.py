#!/usr/bin/env python3
"""
Bluetooth Connection Helper for ESP32 Display
Handles platform-specific Bluetooth device discovery and connection
"""

import sys
import platform
import subprocess
from typing import List, Tuple, Optional


class BluetoothHelper:
    """Platform-specific Bluetooth utilities"""
    
    def __init__(self):
        self.system = platform.system()
    
    # ============ WINDOWS ============
    def find_bluetooth_ports_windows(self) -> List[Tuple[str, str]]:
        """Find Bluetooth COM ports on Windows"""
        import winreg
        
        ports = []
        try:
            reg_path = r"HARDWARE\DEVICEMAP\SERIALCOMM"
            reg = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, reg_path)
            
            i = 0
            while True:
                try:
                    name, value, _ = winreg.EnumValue(reg, i)
                    # Look for Bluetooth ports (typically RFCOMM)
                    if 'RFCOMM' in name or 'Bluetooth' in name:
                        ports.append((value, name))
                    i += 1
                except OSError:
                    break
        except:
            pass
        
        return ports
    
    def find_bluetooth_devices_windows(self) -> List[Tuple[str, str]]:
        """Discover Bluetooth devices on Windows"""
        try:
            # Use Windows WMI to find paired Bluetooth devices
            import wmi
            c = wmi.WMI()
            devices = []
            
            for device in c.Win32_PnPDevice():
                if 'Bluetooth' in str(device.Description):
                    devices.append((device.Name, device.DeviceID))
            
            return devices
        except:
            print("Install wmi package for device discovery: pip install wmi")
            return []
    
    # ============ LINUX ============
    def find_bluetooth_ports_linux(self) -> List[Tuple[str, str]]:
        """Find Bluetooth RFCOMM devices on Linux"""
        import glob
        
        ports = []
        for device in glob.glob('/dev/rfcomm*'):
            ports.append((device, f"RFCOMM {device[-1]}"))
        
        return ports
    
    def find_bluetooth_devices_linux(self) -> List[Tuple[str, str]]:
        """Discover Bluetooth devices on Linux using bluetoothctl"""
        try:
            result = subprocess.run(
                ['bluetoothctl', 'devices'],
                capture_output=True,
                text=True,
                timeout=5
            )
            
            devices = []
            for line in result.stdout.strip().split('\n'):
                if line.startswith('Device'):
                    parts = line.split()
                    if len(parts) >= 3:
                        mac = parts[1]
                        name = ' '.join(parts[2:])
                        devices.append((mac, name))
            
            return devices
        except:
            print("bluetoothctl not found. Install with: sudo apt-get install bluez")
            return []
    
    # ============ MACOS ============
    def find_bluetooth_ports_macos(self) -> List[Tuple[str, str]]:
        """Find Bluetooth serial ports on macOS"""
        import glob
        
        ports = []
        for device in glob.glob('/dev/tty.*.SPP'):
            ports.append((device, f"Bluetooth SPP: {device}"))
        for device in glob.glob('/dev/tty.ESP*'):
            ports.append((device, f"Bluetooth: {device}"))
        
        return ports
    
    def find_bluetooth_devices_macos(self) -> List[Tuple[str, str]]:
        """Discover Bluetooth devices on macOS"""
        try:
            result = subprocess.run(
                ['system_profiler', 'SPBluetoothDataType'],
                capture_output=True,
                text=True,
                timeout=5
            )
            
            devices = []
            lines = result.stdout.split('\n')
            
            for i, line in enumerate(lines):
                if 'Device Name:' in line and i > 0:
                    name = line.split(':', 1)[1].strip()
                    # Try to find address
                    for j in range(max(0, i-3), i):
                        if 'Address:' in lines[j]:
                            addr = lines[j].split(':', 1)[1].strip()
                            devices.append((addr, name))
                            break
            
            return devices
        except:
            print("system_profiler not available")
            return []
    
    # ============ PUBLIC METHODS ============
    def find_bluetooth_ports(self) -> List[Tuple[str, str]]:
        """Find all available Bluetooth ports for current platform"""
        if self.system == 'Windows':
            return self.find_bluetooth_ports_windows()
        elif self.system == 'Linux':
            return self.find_bluetooth_ports_linux()
        elif self.system == 'Darwin':
            return self.find_bluetooth_ports_macos()
        else:
            print(f"Unsupported platform: {self.system}")
            return []
    
    def find_bluetooth_devices(self) -> List[Tuple[str, str]]:
        """Discover Bluetooth devices on current platform"""
        if self.system == 'Windows':
            return self.find_bluetooth_devices_windows()
        elif self.system == 'Linux':
            return self.find_bluetooth_devices_linux()
        elif self.system == 'Darwin':
            return self.find_bluetooth_devices_macos()
        else:
            print(f"Unsupported platform: {self.system}")
            return []
    
    @staticmethod
    def connect_to_device_linux(device_mac: str) -> Optional[str]:
        """
        Connect to Bluetooth device on Linux and create RFCOMM port
        Returns the resulting device path (e.g., /dev/rfcomm0)
        """
        try:
            # Pair if not already paired
            subprocess.run(
                ['bluetoothctl', 'pair', device_mac],
                timeout=10,
                capture_output=True
            )
            
            # Trust the device
            subprocess.run(
                ['bluetoothctl', 'trust', device_mac],
                timeout=10,
                capture_output=True
            )
            
            # Connect
            result = subprocess.run(
                ['bluetoothctl', 'connect', device_mac],
                timeout=10,
                capture_output=True,
                text=True
            )
            
            if 'Connection successful' in result.stdout or result.returncode == 0:
                print(f"✓ Connected to {device_mac}")
                
                # Create RFCOMM binding
                rfcomm_result = subprocess.run(
                    ['sudo', 'rfcomm', 'bind', '/dev/rfcomm0', device_mac],
                    timeout=10,
                    capture_output=True,
                    text=True
                )
                
                if rfcomm_result.returncode == 0 or 'already' in rfcomm_result.stderr:
                    return '/dev/rfcomm0'
            
            return None
        except Exception as e:
            print(f"Error connecting: {e}")
            return None


def main():
    """Test script for Bluetooth discovery"""
    
    helper = BluetoothHelper()
    
    print(f"Platform: {helper.system}\n")
    
    # Find available ports
    print("Available Bluetooth ports:")
    ports = helper.find_bluetooth_ports()
    if ports:
        for port, desc in ports:
            print(f"  {port}: {desc}")
    else:
        print("  None found")
    
    print("\nDiscovered Bluetooth devices:")
    devices = helper.find_bluetooth_devices()
    if devices:
        for addr, name in devices:
            print(f"  {addr}: {name}")
    else:
        print("  None found")
    
    # Test connection (Linux only)
    if helper.system == 'Linux' and devices:
        print("\nNote: To connect a device on Linux:")
        print("  python bluetooth_helper.py <device_mac>")
        
        if len(sys.argv) > 1:
            device_mac = sys.argv[1]
            print(f"\nConnecting to {device_mac}...")
            port = helper.connect_to_device_linux(device_mac)
            if port:
                print(f"✓ RFCOMM port created: {port}")
                print(f"Use with: python esp_control.py -p {port}")
            else:
                print("✗ Connection failed")


if __name__ == '__main__':
    main()
