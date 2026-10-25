#!/usr/bin/env python3
"""Answer 8 bounded, read-only PixelIOS XNU ECM test requests on Linux AF_PACKET.
No writes to the phone, kernel images, or partitions. Requires CAP_NET_RAW.
"""
import argparse
import pathlib
import socket
import struct
import sys
import time
import zlib
import os
import subprocess
import select

ETHERTYPE = 0x88B5
PX_PHONE = bytes.fromhex('02005e77ee02')
PX_HOST = bytes.fromhex('02005e77ee01')


def find_usb_net():
    result = []
    for p in pathlib.Path('/sys/class/net').iterdir():
        dev = p/'device'
        if not dev.exists():
            continue
        cur = dev.resolve()
        for parent in (cur, *cur.parents):
            try:
                v = (parent/'idVendor').read_text().strip().lower()
                d = (parent/'idProduct').read_text().strip().lower()
            except OSError:
                continue
            if (v,d)==('1209','0001'):
                result.append(p.name)
            break
    return result


def fixture(lba):
    return bytes((((i*37+lba*17)^0xa5)&255) for i in range(512))


def response(seq, lba):
    payload = fixture(lba)
    return (PX_PHONE+PX_HOST+struct.pack('!H',ETHERTYPE)
            + struct.pack('<4sIQII', b'PXBR', seq, lba, 512, zlib.crc32(payload)&0xffffffff)
            + payload)


def parse_request(frame):
    if len(frame) < 14 or frame[12:14] != struct.pack('!H',ETHERTYPE):
        return None
    if frame[:6] != PX_HOST or frame[6:12] != PX_PHONE:
        return None
    if len(frame) != 14+24:
        raise ValueError(f'Wrong PX request frame length {len(frame)}')
    magic,seq,lba,count,reserved=struct.unpack_from('<4sIQII',frame,14)
    if magic!=b'PXBQ' or not 1<=seq<=8 or lba!=((seq-1)*977+293)%4096 or count!=512 or reserved:
        raise ValueError(f'Invalid request seq={seq} lba={lba} count={count}')
    return seq,lba


def self_test():
    for seq in range(1,9):
        lba=((seq-1)*977+293)%4096
        req=PX_HOST+PX_PHONE+struct.pack('!H',ETHERTYPE)+struct.pack('<4sIQII',b'PXBQ',seq,lba,512,0)
        assert parse_request(req)==(seq,lba)
        answer=response(seq,lba)
        assert len(answer)==550 and answer[14:18]==b'PXBR'
        assert struct.unpack_from('<I',answer,34)[0]==(zlib.crc32(answer[38:])&0xffffffff)
        try:parse_request(req[:-1])
        except ValueError:pass
        else:raise AssertionError('Truncated request accepted')
        try:parse_request(req[:14]+b'BAD!'+req[18:])
        except ValueError:pass
        else:raise AssertionError('Bad magic accepted')
    print('PASS: 8 bounded ECM block requests, framing, CRC and negative tests')


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--self-test',action='store_true')
    p.add_argument('--interface',default='')
    p.add_argument('--configure',action='store_true',help='Bring up verified USB ECM interface and assign host 192.168.7.1/24')
    p.add_argument('--log',default='',help='Optional XNU UDP live-log output path')
    p.add_argument('--seconds',type=int,default=180)
    a=p.parse_args()
    self_test()
    if a.self_test:return 0
    if not sys.platform.startswith('linux'):
        raise RuntimeError('Linux AF_PACKET required')
    if not 10<=a.seconds<=600:
        raise RuntimeError('seconds must be 10-600')
    end=time.monotonic()+a.seconds
    iface=''
    while time.monotonic()<end:
        nets=find_usb_net()
        if a.interface and a.interface not in nets:
            time.sleep(0.25)
            continue
        if a.interface:
            iface=a.interface
            break
        if len(nets)==1:
            iface=nets[0]
            break
        if len(nets)>1:
            raise RuntimeError(f'Multiple 1209:0001 USB network interfaces: {nets}; choose --interface')
        time.sleep(0.25)
    if not iface:raise RuntimeError('Timed out waiting for XNU USB ECM 1209:0001 interface')
    mac=bytes.fromhex((pathlib.Path('/sys/class/net')/iface/'address').read_text().strip().replace(':',''))
    if mac!=PX_HOST:
        raise RuntimeError(f'Unexpected host ECM MAC {mac.hex(":")}; expected {PX_HOST.hex(":")}; refusing to send')
    if a.configure:
        if os.geteuid():raise RuntimeError('--configure requires root (run via sudo)')
        subprocess.run(['ip','link','set','dev',iface,'up'],check=True)
        current=subprocess.run(['ip','-4','addr','show','dev',iface],check=True,capture_output=True,text=True)
        if '192.168.7.1/24' not in current.stdout:
            subprocess.run(['ip','addr','add','192.168.7.1/24','dev',iface],check=True)
        print('PASS host-only ECM interface UP with 192.168.7.1/24',flush=True)
    with socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(ETHERTYPE)) as so, socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as logso:
        so.bind((iface,ETHERTYPE))
        so.setblocking(False)
        logso.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        logso.bind(('0.0.0.0',5140))
        logso.setblocking(False)
        log = open(a.log,'ab',buffering=0) if a.log else None
        print('Listening for XNU ECM block requests on',iface,'(host MAC',mac.hex(':'),')',flush=True)
        served=set()
        try:
            while time.monotonic()<end:
                active,_,_=select.select([so,logso],[],[],0.4)
                for ready in active:
                    if ready is logso:
                        data,remote=logso.recvfrom(4096)
                        if remote[0]=='192.168.7.2':
                            if log:log.write(data)
                            sys.stdout.buffer.write(data)
                            sys.stdout.buffer.flush()
                        continue
                    packet,addr=so.recvfrom(4096)
                    req=parse_request(packet)
                    if req is None:continue
                    seq,lba=req
                    so.send(response(seq,lba))
                    served.add(seq)
                    print(f'PXB served seq={seq} lba={lba} bytes=512',flush=True)
                    if len(served)==8:
                        print('PASS HOST: eight sectors served; confirm SWEET USBPX: PASS in XNU kernel log',flush=True)
                        return 0
        finally:
            if log:log.close()
        print(f'INCOMPLETE: only {len(served)}/8 unique reads served; inspect XNU live USB log',flush=True)
        return 1


if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as e:
        print('FAIL:',e,file=sys.stderr)
        raise SystemExit(1)
