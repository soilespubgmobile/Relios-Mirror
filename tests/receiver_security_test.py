#!/usr/bin/env python3
"""Security checks of the iPad mirroring receiver (relios-airplay-receiver), without an iPad: the pairing (PIN and
pair-verify, as an iPad does them, and the ways around them that were found and closed) and what someone on the
network can do to it before pairing (crash it, fill its memory, keep others out). Runs the receiver and talks to it on
127.0.0.1 (with its test switch). Needs Python's "cryptography" package.

    python3 receiver_security_test.py <receiver executable> <work folder>
"""
import base64
import hashlib
import os
import plistlib
import socket
import struct
import subprocess
import sys
import threading
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

EXE, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)
KEY = os.path.join(WORK, 'security-key.pem')
subprocess.run([EXE, '--make-key', KEY], check=True)
BP = 'application/x-apple-binary-plist'
failures = []


def check(ok, text):
    print(('ok   ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        failures.append(text)


def raw(public_key):
    return public_key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


class Receiver:
    def __init__(self, allow_pairing=False, registered=()):
        self.p = subprocess.Popen([EXE, '--serve', '--test-allow-loopback'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.status, self.pins, self.log = [], [], []
        threading.Thread(target=self._out, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()
        lines = ['name Relios-Test', 'deviceid 02:11:22:33:44:55', f'keyfile {KEY}',
                 f'allow_pairing {1 if allow_pairing else 0}']
        lines += ['register ' + base64.b64encode(pk).decode() for pk in registered]
        self.send('\n'.join(lines) + '\nstart\n')
        for _ in range(150):
            if any(s.startswith('listening') for s in self.status) or self.p.poll() is not None:
                break
            time.sleep(0.1)

    def send(self, text):
        self.p.stdin.write(text.encode())
        self.p.stdin.flush()

    def _out(self):
        while True:
            head = self.p.stdout.read(8)
            if len(head) < 8:
                return
            body = self.p.stdout.read(struct.unpack('<I', head[4:8])[0])
            if head[2] == 1:
                self.status.append(body.decode('utf-8', 'replace'))
            elif head[2] == 2 and body:
                self.pins.append(body.decode())

    def _err(self):
        for line in self.p.stderr:
            self.log.append(line.decode('utf-8', 'replace').rstrip())

    def running(self):
        return self.p.poll() is None

    def stop(self):
        try:
            self.send('quit\n')
        except OSError:
            pass
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()
        time.sleep(0.2)
        bad = [line for line in self.log if 'AddressSanitizer' in line or 'runtime error:' in line]
        check(not bad, 'no memory or undefined-behaviour error in this receiver\'s run' + (f' ({bad[0]})' if bad else ''))


def connect():
    return socket.create_connection(('127.0.0.1', 7000), timeout=5)


def request(s, method, url, body=b'', ctype='application/octet-stream', cseq=[0]):
    """one request; (status line or None if the connection was closed, body)"""
    cseq[0] += 1
    head = (f'{method} {url} RTSP/1.0\r\nCSeq: {cseq[0]}\r\nContent-Type: {ctype}\r\n'
            f'Content-Length: {len(body)}\r\n\r\n')
    data = b''
    try:
        s.sendall(head.encode() + body)
        while b'\r\n\r\n' not in data:
            chunk = s.recv(65536)
            if not chunk:
                return None, b''
            data += chunk
        head, rest = data.split(b'\r\n\r\n', 1)
        size = 0
        for line in head.split(b'\r\n'):
            if line.lower().startswith(b'content-length:'):
                size = int(line.split(b':')[1])
        while len(rest) < size:
            chunk = s.recv(65536)
            if not chunk:
                break
            rest += chunk
    except OSError:
        return None, b''
    return head.split(b'\r\n')[0].decode('latin-1'), rest


def answered(first):
    return bool(first) and first.startswith('RTSP/1.0 200')


def is_trusted(s):
    """a request only a paired device gets answered"""
    first, _ = request(s, 'GET_PARAMETER', 'rtsp://127.0.0.1/1', b'volume\r\n', 'text/parameters')
    return bool(first) and not first.startswith('RTSP/1.0 470')


def verify_step1(s, ed_public):
    eph = x25519.X25519PrivateKey.generate()
    first, body = request(s, 'POST', '/pair-verify', b'\x01\x00\x00\x00' + raw(eph.public_key()) + ed_public)
    return eph, first, body


def verify_step0(s, eph, server_body, signer):
    server_ecdh = server_body[:32]
    shared = eph.exchange(x25519.X25519PublicKey.from_public_bytes(server_ecdh))
    key = hashlib.sha512(b'Pair-Verify-AES-Key' + shared).digest()[:16]
    iv = hashlib.sha512(b'Pair-Verify-AES-IV' + shared).digest()[:16]
    enc = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    enc.update(b'\x00' * 64)  # (the receiver's "fake round")
    signature = signer.sign(raw(eph.public_key()) + server_ecdh)
    return request(s, 'POST', '/pair-verify', b'\x00\x00\x00\x00' + enc.update(signature))


# SRP-6a as Apple's PIN pairing uses it (2048-bit group, SHA-1, 40-byte session key)
N = int('AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4A099ED8193E0757767A13DD52312AB4B0'
        '3310DCD7F48A9DA04FD50E8083969EDB767B0CF6095179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740A'
        'DBF4FF747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B9078717461A5B9D32E688F87748544523'
        'B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB3786160279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271A'
        'E35F8E9DBFBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73', 16)
G = 2


def sha1(*parts):
    return hashlib.sha1(b''.join(parts)).digest()


def padded(n):
    return n.to_bytes(256, 'big')


def minimal(n):
    return n.to_bytes((n.bit_length() + 7) // 8, 'big')


def pin_pairing(r, me, wrong_pin=False):
    """the whole first pairing of an iPad; None, or where it failed"""
    r.pins.clear()
    s = connect()
    first, _ = request(s, 'POST', '/pair-pin-start')
    if not answered(first):
        return f'pair-pin-start: {first}'
    for _ in range(100):
        if r.pins:
            break
        time.sleep(0.02)
    if not r.pins:
        return 'no PIN shown'
    pin = r.pins[-1]
    if wrong_pin:
        pin = str((int(pin) + 1) % 10000).zfill(4)
    pin = pin.encode()
    user = b'AA:BB:CC:DD:EE:FF'
    first, body = request(s, 'POST', '/pair-setup-pin', plistlib.dumps({'method': 'pin', 'user': user.decode()},
                          fmt=plistlib.FMT_BINARY), BP)
    if not answered(first):
        return f'pair-setup-pin 1: {first}'
    d = plistlib.loads(body)
    salt, B = d['salt'], int.from_bytes(d['pk'], 'big')
    a = int.from_bytes(os.urandom(32), 'big')
    A = pow(G, a, N)
    k = int.from_bytes(sha1(padded(N), padded(G)), 'big')
    u = int.from_bytes(sha1(padded(A), padded(B)), 'big')
    x = int.from_bytes(sha1(salt, sha1(user, b':', pin)), 'big')
    S = pow((B - k * pow(G, x, N)) % N, a + u * x, N)
    K = sha1(minimal(S), b'\0\0\0\0') + sha1(minimal(S), b'\0\0\0\1')
    hxor = bytes(p ^ q for p, q in zip(sha1(minimal(N)), sha1(minimal(G))))
    M = sha1(hxor, sha1(user), minimal(int.from_bytes(salt, 'big')), minimal(A), minimal(B), K)
    first, body = request(s, 'POST', '/pair-setup-pin', plistlib.dumps({'pk': minimal(A), 'proof': M},
                          fmt=plistlib.FMT_BINARY), BP)
    if not answered(first):
        return f'pair-setup-pin 2: {first} (salt {len(salt)} bytes)'
    if plistlib.loads(body).get('proof') != sha1(minimal(A), M, K):
        return 'pair-setup-pin 2: the receiver\'s proof is wrong'
    key = hashlib.sha512(b'Pair-Setup-AES-Key' + K).digest()[:16]
    iv = bytearray(hashlib.sha512(b'Pair-Setup-AES-IV' + K).digest()[:16])
    iv[15] = (iv[15] + 1) & 0xff
    sealed = AESGCM(key).encrypt(bytes(iv), raw(me.public_key()), None)
    first, _ = request(s, 'POST', '/pair-setup-pin', plistlib.dumps({'epk': sealed[:32], 'authTag': sealed[32:]},
                       fmt=plistlib.FMT_BINARY), BP)
    if not answered(first):
        return f'pair-setup-pin 3: {first}'
    eph, first, body = verify_step1(s, raw(me.public_key()))
    if not answered(first):
        return f'pair-verify 1: {first}'
    first, _ = verify_step0(s, eph, body, me)
    if not answered(first):
        return f'pair-verify 0: {first}'
    ok = is_trusted(s)
    s.close()
    return None if ok else 'not trusted after pairing'


ipad = ed25519.Ed25519PrivateKey.generate()      # a device paired before
stranger = ed25519.Ed25519PrivateKey.generate()  # not paired

# 1. pair-verify of a device paired before
r = Receiver(registered=[raw(ipad.public_key())])
check(r.running() and any(x.startswith('listening') for x in r.status), 'the receiver listens')
s = connect()
eph, first, body = verify_step1(s, raw(ipad.public_key()))
check(answered(first) and len(body) == 96, f'pair-verify step 1 with the paired key is answered ({first})')
first, _ = verify_step0(s, eph, body, ipad)
check(answered(first) and is_trusted(s), 'pair-verify signed with the paired key: trusted')
s.close()
s = connect()
eph, first, body = verify_step1(s, raw(stranger.public_key()))
check(bool(first) and '470' in first, f'pair-verify with a key that is not paired is refused ({first})')
s.close()
# the way around it that was closed: step 1 with the paired key, step 1 again with another, signed with the other
s = connect()
verify_step1(s, raw(ipad.public_key()))
eph2, first2, body2 = verify_step1(s, raw(stranger.public_key()))
trusted = False
if answered(first2) and len(body2) == 96:
    first3, _ = verify_step0(s, eph2, body2, stranger)
    trusted = answered(first3) and is_trusted(s)
check(not trusted, 'a second step 1 with another key does not inherit the paired key\'s result')
s.close()
# the same key, signed by someone without its private key
s = connect()
eph, first, body = verify_step1(s, raw(ipad.public_key()))
first, _ = verify_step0(s, eph, body, stranger)
check(not answered(first) and not is_trusted(s), f'the paired key with a signature it did not make is refused ({first})')
s.close()
# keys that are not points of use: all zeros (no shared secret can be made)
s = connect()
first, _ = request(s, 'POST', '/pair-verify', b'\x01\x00\x00\x00' + b'\x00' * 32 + raw(ipad.public_key()))
time.sleep(0.3)
check(r.running() and bool(first) and '470' in first, f'pair-verify with an all-zero key: refused, still running ({first})')
s.close()
s = connect()
check(answered(request(s, 'OPTIONS', '*')[0]), 'and it still answers')
s.close()
r.stop()

# 2. PIN pairing: the real one works (many times: one salt in 256 starts with a zero byte)
r = Receiver(allow_pairing=True)
errors = []
for i in range(60):
    r.send('allow_pairing 1\n')
    time.sleep(0.03)
    err = pin_pairing(r, ed25519.Ed25519PrivateKey.generate())
    if err:
        errors.append(err)
check(not errors, f'60 PIN pairings as an iPad does them all succeed {errors[:3]}')
r.send('allow_pairing 1\n')
time.sleep(0.05)
err = pin_pairing(r, ed25519.Ed25519PrivateKey.generate(), wrong_pin=True)
check(err is not None and 'pair-setup-pin 2' in err, f'with a wrong PIN it fails at the proof ({err})')
r.stop()

# 3. the ways around the PIN that were closed
for name, steps in (('2 without 1', [{'pk': b'\x01' * 256, 'proof': b'\x02' * 20}]),
                    ('3 without 1', [{'epk': b'\x03' * 32, 'authTag': b'\x04' * 16}])):
    r = Receiver(allow_pairing=False)
    s = connect()
    for step in steps:
        first, _ = request(s, 'POST', '/pair-setup-pin', plistlib.dumps(step, fmt=plistlib.FMT_BINARY), BP)
    time.sleep(0.3)
    check(r.running() and bool(first) and '470' in first, f'pair-setup-pin step {name}: refused, still running ({first})')
    s.close()
    r.stop()
r = Receiver(allow_pairing=True)
s = connect()
request(s, 'POST', '/pair-pin-start')
first, _ = request(s, 'POST', '/pair-setup-pin', plistlib.dumps({'method': 'pin', 'user': 'x'}, fmt=plistlib.FMT_BINARY), BP)
zero = b'\x00' * 40  # the session key was all zeros until step 2 succeeded
key = hashlib.sha512(b'Pair-Setup-AES-Key' + zero).digest()[:16]
iv = bytearray(hashlib.sha512(b'Pair-Setup-AES-IV' + zero).digest()[:16])
iv[15] = (iv[15] + 1) & 0xff
sealed = AESGCM(key).encrypt(bytes(iv), raw(stranger.public_key()), None)
first, _ = request(s, 'POST', '/pair-setup-pin', plistlib.dumps({'epk': sealed[:32], 'authTag': sealed[32:]},
                   fmt=plistlib.FMT_BINARY), BP)
check(not answered(first), f'step 3 right after step 1 (no proof of the PIN) is refused ({first})')
s.close()
s = connect()
request(s, 'POST', '/pair-pin-start')
request(s, 'POST', '/pair-setup-pin', plistlib.dumps({'method': 'pin', 'user': 'x'}, fmt=plistlib.FMT_BINARY), BP)
first, _ = request(s, 'POST', '/pair-setup-pin', plistlib.dumps({'pk': b'\x00' * 256, 'proof': b'\x00' * 20},
                   fmt=plistlib.FMT_BINARY), BP)
time.sleep(0.3)
check(r.running() and not answered(first), f'step 2 with A = 0 (the SRP check) is refused, still running ({first})')
s.close()
r.stop()

# 4. what someone on the network can do before pairing
r = Receiver()
s = connect()
try:
    s.sendall(b'OPTIONS /' + b'a' * 20000)
    s.settimeout(5)
    closed = s.recv(100) == b''
except OSError:
    closed = True
check(closed, 'an endless address is not kept: the connection is closed')
s.close()
s = connect()
try:
    s.sendall(b'POST /x RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 2000000000\r\n\r\n')
    chunk = b'\x00' * 65536
    for _ in range(9 * 16):  # 9 MB
        s.sendall(chunk)
    s.settimeout(5)
    closed = s.recv(100) == b''
except OSError:
    closed = True
check(closed and r.running(), 'a body over 8 MB is not kept: the connection is closed')
s.close()
idle = [connect() for _ in range(12)]
time.sleep(0.5)
s = connect()
first, _ = request(s, 'OPTIONS', '*')
check(answered(first), f'with 12 idle connections open a new device is still answered ({first})')
s.close()
for c in idle:
    c.close()
time.sleep(0.5)
a = connect()
check(answered(request(a, 'OPTIONS', '*')[0]), 'an unpaired device asks something')
b = connect()
first, _ = request(b, 'OPTIONS', '*')
check(bool(first) and '409' in first, f'right after it another device gets "Conflict" ({first})')
b.close()
time.sleep(11)
b = connect()
first, _ = request(b, 'OPTIONS', '*')
check(answered(first), f'after 10 s of silence the unpaired one gives way ({first})')
a.close()
b.close()
r.stop()
r = Receiver(registered=[raw(ipad.public_key())])
a = connect()
eph, first, body = verify_step1(a, raw(ipad.public_key()))
verify_step0(a, eph, body, ipad)
time.sleep(11)
b = connect()
first, _ = request(b, 'OPTIONS', '*')
check(bool(first) and '409' in first and is_trusted(a), f'a paired device keeps its place, even when silent ({first})')
a.close()
b.close()
r.stop()

print(f'{len(failures)} failure(s)')
sys.exit(1 if failures else 0)
