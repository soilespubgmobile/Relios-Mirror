#!/usr/bin/env python3
"""Checks of the iPad mirroring receiver (relios-airplay-receiver) without an iPad: who may connect and what
is answered before pairing. Runs the receiver, talks RTSP to it on 127.0.0.1 and reads its messages.

    python3 receiver_test.py <receiver executable> <work folder>
"""
import os
import plistlib
import socket
import struct
import subprocess
import sys
import threading
import time

EXE, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)
KEY = os.path.join(WORK, 'receiver-key.pem')
failures = []


def check(ok, text):
    print(('ok   ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        failures.append(text)


class Receiver:
    def __init__(self, extra_args, config):
        self.proc = subprocess.Popen([EXE, '--serve'] + extra_args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.messages, self.log = [], []
        threading.Thread(target=self._read_out, daemon=True).start()
        threading.Thread(target=self._read_err, daemon=True).start()
        self.proc.stdin.write(('\n'.join(config) + '\nstart\n').encode())
        self.proc.stdin.flush()

    def _read_out(self):
        out = self.proc.stdout
        while True:
            head = out.read(8)
            if len(head) < 8:
                return
            if head[:2] != b'RA':
                self.messages.append(('bad header', head))
                return
            length = struct.unpack('<I', head[4:8])[0]
            payload = out.read(length)
            self.messages.append((head[2], payload))

    def _read_err(self):
        for line in self.proc.stderr:
            self.log.append(line.decode('utf-8', 'replace').rstrip())

    def status(self):
        return [p.decode('utf-8', 'replace') for t, p in self.messages if t == 1]

    def wait_status(self, prefix, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            if any(s.startswith(prefix) for s in self.status()):
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.1)
        return False

    def stop(self):
        try:
            self.proc.stdin.write(b'quit\n')
            self.proc.stdin.flush()
        except OSError:
            pass
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        return self.proc.returncode


def rtsp(method, url, body=b'', headers=None, sock=None, keep=False):
    s = sock or socket.create_connection(('127.0.0.1', 7000), timeout=5)
    h = {'CSeq': '1', 'User-Agent': 'AirPlay/650.1', 'Content-Length': str(len(body))}
    h.update(headers or {})
    req = f'{method} {url} RTSP/1.0\r\n' + ''.join(f'{k}: {v}\r\n' for k, v in h.items()) + '\r\n'
    s.sendall(req.encode() + body)
    data = b''
    try:
        while b'\r\n\r\n' not in data:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    if not keep:
        s.close()
    first = data.split(b'\r\n', 1)[0].decode('latin-1')
    return first, s


def rtsp_answer(method, url, body=b'', headers=None):
    """the whole answer: status line, headers (lower-case names), body"""
    s = socket.create_connection(('127.0.0.1', 7000), timeout=5)
    h = {'CSeq': '1', 'User-Agent': 'AirPlay/650.1', 'Content-Length': str(len(body))}
    h.update(headers or {})
    req = f'{method} {url} RTSP/1.0\r\n' + ''.join(f'{k}: {v}\r\n' for k, v in h.items()) + '\r\n'
    s.sendall(req.encode() + body)
    data = b''
    try:
        while b'\r\n\r\n' not in data:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        head, _, rest = data.partition(b'\r\n\r\n')
        fields = {}
        for line in head.split(b'\r\n')[1:]:
            k, _, v = line.partition(b':')
            fields[k.strip().lower().decode('latin-1')] = v.strip().decode('latin-1')
        size = int(fields.get('content-length', '0') or 0)
        while len(rest) < size:
            chunk = s.recv(65536)
            if not chunk:
                break
            rest += chunk
    except socket.timeout:
        head, fields, rest, size = data, {}, b'', 0
    s.close()
    return head.split(b'\r\n', 1)[0].decode('latin-1'), fields, rest[:size]


def info_plist(body=None):
    """GET /info, optionally with a plist; the answer's plist (or None)"""
    if body is None:
        first, fields, data = rtsp_answer('GET', '/info')
    else:
        payload = plistlib.dumps(body, fmt=plistlib.FMT_BINARY)
        first, fields, data = rtsp_answer('GET', '/info', payload,
                                          {'Content-Type': 'application/x-apple-binary-plist'})
    try:
        return first, plistlib.loads(data) if data else None
    except Exception:
        return first, None


# 1. the receiver's key
subprocess.run([EXE, '--make-key', KEY], check=False)
check(os.path.exists(KEY) and b'PRIVATE KEY' in open(KEY, 'rb').read(), '--make-key writes a private key')

base = ['name ReliosOBS-test', 'deviceid 02:11:22:33:44:55', f'keyfile {KEY}', 'allow_pairing 0']

# 2. without the test switch, a connection from 127.0.0.1 (not the local network) is closed at once
r = Receiver([], base)
check(r.wait_status('listening'), 'the receiver listens and announces itself')
first = ''
try:
    first, _ = rtsp('OPTIONS', '*')
except OSError as e:
    first = f'({e.__class__.__name__})'
check(not first.startswith('RTSP/1.0 200'), f'loopback is refused without the test switch (got: {first or "closed"})')
time.sleep(0.5)
check(any('outside the local network' in line for line in r.log), 'the refusal is logged')
check(r.stop() == 0, 'the receiver ends on "quit"')

# 3. with the test switch: what is answered before pairing
r = Receiver(['--test-allow-loopback'], base)
check(r.wait_status('listening'), 'the receiver listens (test switch)')
first, _ = rtsp('OPTIONS', '*')
check(first.startswith('RTSP/1.0 200'), f'OPTIONS is answered ({first})')
first, _ = rtsp('GET', '/info')
check(first.startswith('RTSP/1.0 200'), f'GET /info is answered ({first})')
# the receiver's description: the screen, and H.264 only unless OBS offers H.265 (feature bit 42)
first, info = info_plist()
check(bool(info) and info.get('displays') and info['displays'][0].get('width') == 1920 and
      info['displays'][0].get('maxFPS') == 60, f'GET /info describes the screen (1920x1080, 60 fps) ({first})')
check(bool(info) and info.get('features', 0) & (1 << 7) and not info.get('features', 0) & (1 << 42),
      'mirroring is offered, H.265 not (not asked for)')
first, info = info_plist({'qualifier': ['txtAirPlay']})
check(bool(info) and list(info.keys()) == ['txtAirPlay'], f'GET /info for txtAirPlay gets that record only ({first})')
# (newer iPadOS: a plist without a qualifier gets the full description, not an empty answer)
first, info = info_plist({'relios-test': True})
check(bool(info) and 'displays' in info and 'features' in info,
      f'GET /info with a plist but no qualifier gets the full description ({first})')
first, _ = rtsp('POST', '/pair-setup', os.urandom(32))
check('470' in first, f'pair-setup without the PIN is refused ({first})')
first, _ = rtsp('POST', '/pair-pin-start')
check('470' in first, f'pair-pin-start is refused while pairing a new device is off ({first})')
check(not any(t == 2 for t, _ in r.messages), 'no PIN is shown while pairing is off')
first, _ = rtsp('POST', '/pair-setup-pin', b'bplist00' + os.urandom(40),
                {'Content-Type': 'application/x-apple-binary-plist'})
check('470' in first, f'pair-setup-pin without a PIN shown is refused ({first})')
for method, url in (('SETUP', 'rtsp://127.0.0.1/1'), ('RECORD', 'rtsp://127.0.0.1/1'), ('POST', '/fp-setup'),
                    ('GET_PARAMETER', 'rtsp://127.0.0.1/1'), ('TEARDOWN', 'rtsp://127.0.0.1/1'), ('POST', '/feedback')):
    first, _ = rtsp(method, url, os.urandom(16))
    check('470' in first, f'{method} {url} before pairing is refused ({first})')
def http_first_line(request):
    s = socket.create_connection(('127.0.0.1', 7000), timeout=3)
    s.sendall(request)
    try:
        data = s.recv(4096)
    except socket.timeout:
        data = b''
    s.close()
    return data.split(b'\r\n', 1)[0].decode('latin-1')


# (the video-streaming kind, without CSeq, is ignored by the library; with CSeq it gets ReliosOBS's 501)
first = http_first_line(b'GET /server-info HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n')
check(' 200 ' not in first, f'HTTP AirPlay (HLS) without CSeq is not served ({first or "no answer"})')
first = http_first_line(b'POST /play HTTP/1.1\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n')
check('501' in first, f'HTTP AirPlay with CSeq is refused ({first or "no answer"})')

# 4. pairing switched on: a PIN is shown once; using it wrongly three times turns pairing off
r.proc.stdin.write(b'allow_pairing 1\n')
r.proc.stdin.flush()
time.sleep(0.3)
first, _ = rtsp('POST', '/pair-pin-start')
check(first.startswith('RTSP/1.0 200'), f'pair-pin-start is answered while pairing is on ({first})')
time.sleep(0.3)
pins = [p for t, p in r.messages if t == 2 and p]
check(len(pins) == 1 and len(pins[0]) == 4 and pins[0].isdigit(), f'a 4-digit PIN is shown ({pins})')
check(pins and not any(pins[0].decode() in line for line in r.log), 'the PIN is not in the receiver\'s log')
for i in range(3):
    rtsp('POST', '/pair-pin-start')
    rtsp('POST', '/pair-setup-pin', b'bplist00' + os.urandom(40), {'Content-Type': 'application/x-apple-binary-plist'})
time.sleep(0.5)
check(any(s == 'pairing_off' for s in r.status()), 'three failed PIN pairings turn pairing off')
first, _ = rtsp('POST', '/pair-pin-start')
check('470' in first, f'after that pair-pin-start is refused ({first})')

# 5. rubbish does not stop it
for i in range(300):
    try:
        s = socket.create_connection(('127.0.0.1', 7000), timeout=2)
        s.sendall(os.urandom(1 + (i * 37) % 3000))
        s.close()
    except OSError:
        pass
for i in range(50):
    try:
        rtsp(['POST', 'GET', 'SETUP', 'OPTIONS'][i % 4], '/' + os.urandom(6).hex(), os.urandom(i * 11),
             {'Content-Type': 'application/x-apple-binary-plist'})
    except OSError:
        pass
time.sleep(1)
check(r.proc.poll() is None, 'the receiver still runs after 350 rubbish requests')
first, _ = rtsp('OPTIONS', '*')
check(first.startswith('RTSP/1.0 200'), f'and still answers ({first})')
check(r.stop() == 0, 'the receiver ends on "quit"')

# 5b. "h265 1": H.265 is offered (feature bit 42), in /info and in what the receiver says
r = Receiver(['--test-allow-loopback'], base + ['h265 1'])
check(r.wait_status('listening'), 'the receiver listens (H.265 offered)')
first, info = info_plist()
check(bool(info) and info.get('features', 0) & (1 << 42) and info.get('features', 0) & (1 << 7),
      f'with "h265 1" GET /info offers H.265 and mirroring ({first})')
time.sleep(0.3)
check(any('H.264 or H.265' in line for line in r.log), 'the receiver logs that it offers H.265')
check(r.stop() == 0, 'the receiver ends on "quit"')

# 6. standard input closed (OBS gone): the receiver ends by itself
r = Receiver([], base)
check(r.wait_status('listening'), 'the receiver listens')
r.proc.stdin.close()
try:
    r.proc.wait(10)
except subprocess.TimeoutExpired:
    pass
check(r.proc.poll() is not None, 'the receiver ends when its standard input closes')
if r.proc.poll() is None:
    r.proc.kill()

with open(os.path.join(WORK, 'receiver-log.txt'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(r.log))
print(f'{len(failures)} failure(s)')
sys.exit(1 if failures else 0)
