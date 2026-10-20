#!/usr/bin/env python3
"""FIPC handshake test for Android daemon"""

import socket
import struct
import sys
import time

FIPC_MAGIC = 0x46495043  # "FIPC"
FIPC_VERSION = 1
WIRE_HEADER_SIZE = 32

class InputMessageType:
    Hello = 1
    HelloAck = 2
    TouchFrame = 3
    KeyEvent = 4
    CancelAll = 5
    Ping = 6
    Pong = 7
    Disconnect = 8
    Error = 9

class InputProtocolError:
    None_ = 0
    InvalidMagic = 1
    UnsupportedVersion = 2
    UnknownMessageType = 3
    OversizedPayload = 4
    InvalidCoordinates = 5
    InvalidContactId = 6
    ExceededContactLimit = 7
    StaleGeneration = 8
    SessionMismatch = 9
    QueueOverflow = 10
    ConnectionClosed = 11

def encode_string(s):
    """Encode string as u32le length + UTF-8 bytes"""
    data = s.encode('utf-8')
    return struct.pack('<I', len(data)) + data

def decode_string(data, offset):
    """Decode string from u32le length + UTF-8 bytes"""
    if offset + 4 > len(data):
        return "", offset
    length = struct.unpack('<I', data[offset:offset+4])[0]
    offset += 4
    if offset + length > len(data):
        return "", offset
    s = data[offset:offset+length].decode('utf-8')
    offset += length
    return s, offset

def encode_wire_header(magic, version, msg_type, payload_size, sequence, timestamp, status=0):
    """Encode wire header"""
    # Wire format: magic(4), version(2), type(2), sequence(8), timestamp(8), payload_size(4), status(4) = 32 bytes
    return struct.pack('>IHHQQII', magic, version, msg_type, sequence, timestamp, payload_size, status)

def decode_wire_header(data):
    """Decode wire header"""
    if len(data) < WIRE_HEADER_SIZE:
        return None
    # Wire format: magic(4), version(2), type(2), sequence(8), timestamp(8), payload_size(4), status(4) = 32 bytes
    magic, version, msg_type, sequence, timestamp, payload_size, status = struct.unpack('>IHHQQII', data[:WIRE_HEADER_SIZE])
    return {
        'magic': magic,
        'version': version,
        'type': msg_type,
        'payload_size': payload_size,
        'sequence': sequence,
        'timestamp': timestamp,
        'status': status
    }

def encode_hello(client_version, display_width, display_height, session_id, generation_id):
    """Encode Hello payload"""
    payload = struct.pack('<HII', client_version, display_width, display_height)
    payload += encode_string(session_id)
    payload += encode_string(generation_id)
    return payload

def decode_hello_ack(payload):
    """Decode HelloAck payload"""
    offset = 0
    guest_version = struct.unpack('<H', payload[offset:offset+2])[0]
    offset += 2
    status = struct.unpack('<I', payload[offset:offset+4])[0]
    offset += 4
    max_contacts = struct.unpack('<I', payload[offset:offset+4])[0]
    offset += 4
    supported_caps = struct.unpack('<I', payload[offset:offset+4])[0]
    offset += 4
    session_id, offset = decode_string(payload, offset)
    generation_id, offset = decode_string(payload, offset)
    return {
        'guest_version': guest_version,
        'status': status,
        'max_contacts': max_contacts,
        'supported_caps': supported_caps,
        'session_id': session_id,
        'generation_id': generation_id
    }

def main():
    host = '127.0.0.1'
    port = 24814
    session_id = 'test-fipc'
    generation_id = 'test-gen'
    
    print(f"Connecting to {host}:{port}...")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10.0)  # Increased timeout
    
    try:
        sock.connect((host, port))
        print("Connected!")
    except Exception as e:
        print(f"Connection failed: {e}")
        return 1
    
    # Build Hello message
    hello_payload = encode_hello(
        client_version=FIPC_VERSION,
        display_width=1080,
        display_height=1920,
        session_id=session_id,
        generation_id=generation_id
    )
    
    timestamp = int(time.time() * 1000)
    hello_header = encode_wire_header(
        magic=FIPC_MAGIC,
        version=FIPC_VERSION,
        msg_type=InputMessageType.Hello,
        payload_size=len(hello_payload),
        sequence=1,
        timestamp=timestamp
    )
    
    hello_packet = hello_header + hello_payload
    
    # DEBUG: Print packet bytes
    print(f"[DEBUG] hello_header len={len(hello_header)}: {hello_header.hex()}")
    print(f"[DEBUG] hello_payload len={len(hello_payload)}: {hello_payload.hex()}")
    print(f"[DEBUG] hello_packet len={len(hello_packet)}: {hello_packet.hex()}")
    
    print(f"Sending Hello packet ({len(hello_packet)} bytes)...")
    sock.sendall(hello_packet)
    
    print("Waiting for response...")
    # Receive response
    try:
        response = sock.recv(4096)
        print(f"recv returned {len(response)} bytes")
    except socket.timeout:
        print("Timeout waiting for HelloAck")
        return 1
    
    if not response:
        print("No response received")
        return 1
    
    print(f"Received {len(response)} bytes")
    print(f"[DEBUG] Raw response: {response.hex()}")
    
    # Decode header
    header = decode_wire_header(response)
    if not header:
        print("Failed to decode header")
        return 1
    
    print(f"Response header: magic=0x{header['magic']:08X}, version={header['version']}, "
          f"type={header['type']}, payload_size={header['payload_size']}, seq={header['sequence']}, status={header['status']}")
    
    if header['magic'] != FIPC_MAGIC:
        print(f"Invalid magic: 0x{header['magic']:08X}")
        return 1
    if header['version'] != FIPC_VERSION:
        print(f"Version mismatch: {header['version']}")
        return 1
    if header['type'] != InputMessageType.HelloAck:
        print(f"Expected HelloAck (type 2), got type {header['type']}")
        return 1
    
    if len(response) < WIRE_HEADER_SIZE + header['payload_size']:
        print("Incomplete payload")
        return 1
    
    ack_payload = response[WIRE_HEADER_SIZE:WIRE_HEADER_SIZE + header['payload_size']]
    ack = decode_hello_ack(ack_payload)
    
    print("HelloAck received:")
    print(f"  guest_version: {ack['guest_version']}")
    print(f"  status: {ack['status']}")
    print(f"  max_contacts: {ack['max_contacts']}")
    print(f"  supported_caps: 0x{ack['supported_caps']:08X}")
    print(f"  session_id: {ack['session_id']}")
    print(f"  generation_id: {ack['generation_id']}")
    
    # Validate handshake
    if ack['status'] != InputProtocolError.None_:
        print(f"Handshake rejected with status: {ack['status']}")
        return 1
    if ack['session_id'] != session_id:
        print(f"Session ID mismatch: expected '{session_id}', got '{ack['session_id']}'")
        return 1
    if ack['generation_id'] != generation_id:
        print(f"Generation ID mismatch: expected '{generation_id}', got '{ack['generation_id']}'")
        return 1
    if ack['guest_version'] != FIPC_VERSION:
        print(f"Protocol version mismatch: expected {FIPC_VERSION}, got {ack['guest_version']}")
        return 1
    
    print("\n✓ FIPC handshake SUCCESSFUL - ProtocolReady state achieved!")
    return 0

if __name__ == '__main__':
    sys.exit(main())