#!/usr/bin/env python3
"""Live fuzzing of the iPad mirroring receiver over the network, as someone on the local network could do it before
pairing: thousands of damaged RTSP / HTTP requests (the pairing steps, FairPlay's setup, /info with and without a
plist, SETUP, unknown methods, broken framing), sent on several connections at once, plus random multicast DNS
questions to the receiver's mDNS port. The receiver must keep running and answering, and (built with
-DRELIOS_AIRPLAY_SANITIZE=ON) report no memory or undefined-behaviour error.

    python3 receiver_live_fuzz.py <receiver executable> <work folder> [seconds] [seed]
"""
import os
import plistlib
import random
import socket
import struct
import subprocess
import sys
import threading
import time

EXE, WORK = sys.argv[1], sys.argv[2]
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 120.0
rnd = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
os.makedirs(WORK, exist_ok=True)
KEY = os.path.join(WORK, 'fuzz-key.pem')
subprocess.run([EXE, '--make-key', KEY], check=True)
failures = []


def check(ok, text):
    print(('ok   ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        failures.append(text)


class Receiver:
    def __init__(self, allow_pairing):
        self.p = subprocess.Popen([EXE, '--serve', '--test-allow-loopback'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.status, self.log = [], []
        threading.Thread(target=self._out, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()
        lines = ['name Relios-Fuzz', 'deviceid 02:11:22:33:44:56', f'keyfile {KEY}',
                 f'allow_pairing {1 if allow_pairing else 0}']
        try:
            self.p.stdin.write(('\n'.join(lines) + '\nstart\n').encode())
            self.p.stdin.flush()
        except OSError:
            pass
        for _ in range(150):
            if any(s.startswith('listening') for s in self.status) or self.p.poll() is not None:
                break
            time.sleep(0.1)

    def _out(self):
        while True:
            head = self.p.stdout.read(8)
            if len(head) < 8:
                return
            body = self.p.stdout.read(struct.unpack('<I', head[4:8])[0])
            if head[2] == 1:
                self.status.append(body.decode('utf-8', 'replace'))

    def _err(self):
        for line in self.p.stderr:
            self.log.append(line.decode('utf-8', 'replace').rstrip())

    def stop(self):
        try:
            self.p.stdin.write(b'quit\n')
            self.p.stdin.flush()
        except OSError:
            pass
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            self.p.kill()
        time.sleep(0.3)


def random_bytes(n):
    return bytes(rnd.getrandbits(8) for _ in range(n))


def random_plist():
    def value(depth):
        kind = rnd.randrange(8 if depth < 3 else 5)
        if kind == 0:
            return rnd.randrange(-2**40, 2**40)
        if kind == 1:
            return random_bytes(rnd.randrange(0, 64))
        if kind == 2:
            return ''.join(chr(rnd.randrange(1, 0x2ff)) for _ in range(rnd.randrange(0, 40)))
        if kind == 3:
            return rnd.random() * 1e6
        if kind == 4:
            return rnd.random() < 0.5
        if kind == 5:
            return [value(depth + 1) for _ in range(rnd.randrange(0, 5))]
        keys = ['qualifier', 'streams', 'type', 'ekey', 'eiv', 'timingPort', 'deviceID', 'name', 'model',
                'isScreenMirroringSession', 'timingProtocol', 'et', 'osName', 'macAddress', 'sessionUUID']
        return {rnd.choice(keys): value(depth + 1) for _ in range(rnd.randrange(0, 6))}
    top = value(0)
    if not isinstance(top, dict):
        top = {'qualifier': [top]}
    if rnd.random() < 0.3:
        top['qualifier'] = ['txtAirPlay']
    return plistlib.dumps(top, fmt=plistlib.FMT_BINARY)


def damage(data):
    data = bytearray(data)
    for _ in range(rnd.choice([0, 0, 1, 1, 2, 5, 20])):
        if not data:
            break
        op = rnd.randrange(5)
        at = rnd.randrange(len(data))
        if op == 0:
            data[at] ^= 1 << rnd.randrange(8)
        elif op == 1:
            data[at] = rnd.choice([0, 0xff, 0x7f, 0x80, ord('\r'), ord('\n'), ord(':')])
        elif op == 2:
            del data[at:at + rnd.randrange(1, 16)]
        elif op == 3:
            data[at:at] = random_bytes(rnd.randrange(1, 16))
        else:
            data[at:at] = data[at:at + rnd.randrange(1, 64)]
    return bytes(data)


def make_request():
    method, url = rnd.choice([
        ('GET', '/info'), ('POST', '/pair-setup'), ('POST', '/pair-verify'), ('POST', '/fp-setup'),
        ('POST', '/pair-pin-start'), ('POST', '/pair-setup-pin'), ('SETUP', 'rtsp://127.0.0.1/1234567890'),
        ('RECORD', 'rtsp://127.0.0.1/1234567890'), ('GET_PARAMETER', 'rtsp://127.0.0.1/1'),
        ('SET_PARAMETER', 'rtsp://127.0.0.1/1'), ('TEARDOWN', 'rtsp://127.0.0.1/1'), ('POST', '/feedback'),
        ('POST', '/audioMode'), ('OPTIONS', '*'), ('POST', '/play'), ('GET', '/server-info'), ('FOO', '/bar'),
        ('POST', '/fp-setup2'), ('GET', '/playback-info'), ('POST', '/reverse')])
    kind = rnd.randrange(6)
    if kind == 0:
        body, ctype = random_plist(), 'application/x-apple-binary-plist'
    elif kind == 1:
        size = rnd.choice([0, 1, 4, 16, 32, 37, 64, 68, 132, 142, 164, 173, 256, 1024, 70000])
        body, ctype = random_bytes(size), 'application/octet-stream'
    elif kind == 2:
        # FairPlay's setup messages: the right magic and sizes, damaged insides
        msg = bytearray(b'FPLY\x03\x01\x01\x00\x00\x00\x00\x04\x02\x00' + bytes([rnd.randrange(4)]) + b'\xbb')
        if rnd.random() < 0.5:
            msg = bytearray(b'FPLY\x03\x01\x03\x00\x00\x00\x00\x98') + bytearray(random_bytes(152))
        body, ctype = bytes(msg), 'application/octet-stream'
    elif kind == 3:
        # pair-verify's first message: 1 + 3 bytes, then 32 + 32 bytes of keys
        body, ctype = bytes([1, 0, 0, 0]) + random_bytes(64), 'application/octet-stream'
    elif kind == 4:
        body, ctype = b'', 'application/octet-stream'
    else:
        body, ctype = random_bytes(rnd.randrange(0, 300)), rnd.choice(['text/parameters', 'application/x-dmap-tagged'])
    headers = [f'CSeq: {rnd.randrange(0, 2**31)}', f'Content-Type: {ctype}', 'User-Agent: AirPlay/845.6.1']
    if rnd.random() < 0.3:
        headers.append('X-Apple-Session-ID: ' + rnd.choice(['', '1', 'A' * 300, 'abc-def']))
    if rnd.random() < 0.2:
        headers.append(rnd.choice(['Transfer-Encoding: chunked', 'Upgrade: PTTH/1.0', 'Connection: Upgrade',
                                   'Content-Length: -5', 'Content-Length: 99999999999', 'X-' + 'a' * 9000 + ': 1']))
    if rnd.random() < 0.9:
        headers.append(f'Content-Length: {len(body)}')
    proto = rnd.choice(['RTSP/1.0', 'RTSP/1.0', 'HTTP/1.1', 'HTTP/1.0', 'RTSP/9.9'])
    text = f'{method} {url} {proto}\r\n' + '\r\n'.join(headers) + '\r\n\r\n'
    return damage(text.encode('latin-1') + body)


stats = {'requests': 0, 'answers': 0, 'closed': 0, 'mdns': 0}
lock = threading.Lock()


def tcp_worker(stop_at):
    while time.time() < stop_at:
        try:
            s = socket.create_connection(('127.0.0.1', 7000), timeout=3)
        except OSError:
            time.sleep(0.05)
            continue
        try:
            for _ in range(rnd.randrange(1, 12)):
                data = make_request()
                # sometimes in small pieces, sometimes cut short
                if rnd.random() < 0.2:
                    data = data[:rnd.randrange(len(data) + 1)]
                if rnd.random() < 0.3:
                    for i in range(0, len(data), 7):
                        s.sendall(data[i:i + 7])
                else:
                    s.sendall(data)
                with lock:
                    stats['requests'] += 1
                try:
                    got = s.recv(65536)
                except socket.timeout:
                    got = b''
                if not got:
                    with lock:
                        stats['closed'] += 1
                    break
                with lock:
                    stats['answers'] += 1
        except OSError:
            with lock:
                stats['closed'] += 1
        finally:
            s.close()


def mdns_worker(stop_at):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while time.time() < stop_at:
        q = bytearray(struct.pack('>HHHHHH', rnd.randrange(65536), 0, rnd.randrange(1, 4), 0, 0, 0))
        for _ in range(rnd.randrange(1, 4)):
            for label in rnd.choice([['_airplay', '_tcp', 'local'], ['_raop', '_tcp', 'local'],
                                     ['_services', '_dns-sd', '_udp', 'local'], ['Relios-Fuzz', 'local']]):
                q += bytes([len(label)]) + label.encode()
            q += b'\x00' + struct.pack('>HH', rnd.choice([1, 12, 16, 28, 33, 255]), rnd.choice([1, 0x8001]))
        try:
            u.sendto(damage(bytes(q)), ('127.0.0.1', 5353))
            with lock:
                stats['mdns'] += 1
        except OSError:
            pass
        time.sleep(0.002)
    u.close()


for allow in (False, True):
    r = Receiver(allow_pairing=allow)
    check(any(s.startswith('listening') for s in r.status),
          f'the receiver listens (pairing a new device {"on" if allow else "off"})')
    stop_at = time.time() + SECONDS / 2
    threads = [threading.Thread(target=tcp_worker, args=(stop_at,)) for _ in range(4)]
    threads.append(threading.Thread(target=mdns_worker, args=(stop_at,)))
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    alive = r.p.poll() is None
    check(alive, 'the receiver still runs after the fuzzing' +
          ('' if alive else f' (it ended with {r.p.returncode}; its last lines: ' + ' | '.join(r.log[-4:]) + ')'))
    # still answers a plain request
    answered = False
    try:
        s = socket.create_connection(('127.0.0.1', 7000), timeout=5)
        s.sendall(b'GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n')
        answered = s.recv(4096).startswith(b'RTSP/1.0 ')
        s.close()
    except OSError:
        pass
    check(answered, 'the receiver still answers a plain request')
    r.stop()
    bad = [line for line in r.log if 'AddressSanitizer' in line or 'runtime error:' in line]
    check(not bad, 'no memory or undefined-behaviour error' + (f' ({bad[0]})' if bad else ''))
    print(f'     {stats["requests"]} requests, {stats["answers"]} answered, {stats["closed"]} connections closed '
          f'by the receiver, {stats["mdns"]} mDNS questions', flush=True)

print(f'{len(failures)} failure(s)')
sys.exit(1 if failures else 0)
