#!/usr/bin/env python3
"""Validate local Redmi 2 radio resources; never fetch firmware or access a phone."""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import struct

class WifiError(ValueError):
    pass

def profile_bytes(config):
    if not isinstance(config,dict) or set(config)-{'ssid','security','mac','channel','password','pmk'}:
        raise WifiError('Wi-Fi profile contains unknown fields')
    security=config.get('security')
    if security not in ('open','wpa2-psk','wpa3-sae'):
        raise WifiError('select open, wpa2-psk or wpa3-sae explicitly; automatic downgrade is disabled')
    ssid=config.get('ssid')
    if not isinstance(ssid,str) or not 1<=len(ssid.encode('utf-8'))<=32 or '\0' in ssid:
        raise WifiError('SSID must contain 1–32 UTF-8 bytes without NUL')
    text=config.get('mac','')
    if not isinstance(text,str) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',text):
        raise WifiError('profile requires a stable unicast MAC; use this tool to create one')
    mac=bytes.fromhex(text.replace(':',''))
    if mac[0]&1 or not any(mac):raise WifiError('MAC must be a nonzero unicast address')
    channel=config.get('channel',0)
    if type(channel) is not int or not 0<=channel<=11:raise WifiError('channel must be 0 (scan) or 1–11')
    ssid=ssid.encode('utf-8')
    password=b'';pmk=bytes(32)
    if security=='open':
        if 'password' in config or 'pmk' in config:raise WifiError('open profiles cannot contain credentials')
    elif security=='wpa2-psk' and 'pmk' in config:
        if 'password' in config or not isinstance(config['pmk'],str) or not re.fullmatch('[0-9a-fA-F]{64}',config['pmk']):
            raise WifiError('WPA2 PMK must contain exactly 64 hex digits and cannot be combined with a password')
        pmk=bytes.fromhex(config['pmk'])
    else:
        value=config.get('password')
        minimum=8 if security=='wpa2-psk' else 1
        if 'pmk' in config or not isinstance(value,str) or '\0' in value or not minimum<=len(value.encode('utf-8'))<=63:
            raise WifiError(f'{security} password must contain {minimum}–63 UTF-8 bytes without NUL')
        password=value.encode('utf-8')
        if security=='wpa2-psk':
            pmk=hashlib.pbkdf2_hmac('sha1',password,ssid,4096,32)
    mode=('open','wpa2-psk','wpa3-sae').index(security)
    return struct.pack('<4s4B6sBx32s32s64s16x',b'SOWF',2,len(ssid),mode,channel,mac,len(password),ssid,pmk,password)

def read_file(folder,name,limit):
    path=folder/name
    if not path.is_file() or not 1<=path.stat().st_size<=limit:raise WifiError(f'missing or invalid {name}')
    data=path.read_bytes()
    if not 1<=len(data)<=limit:raise WifiError(f'oversized {name}')
    return data

def resources(folder,profile=None):
    folder=Path(folder)
    config=profile_bytes(json.loads(Path(profile).read_bytes())) if profile else None
    mdt=read_file(folder,'wcnss.mdt',6*1024*1024)
    if len(mdt)<52 or mdt[:7]!=b'\x7fELF\x01\x01\x01' or struct.unpack_from('<HHI',mdt,16)!=(2,40,1) or struct.unpack_from('<I',mdt,28)[0]!=52 or struct.unpack_from('<HH',mdt,40)!=(52,32):
        raise WifiError('firmware must be a signed ARM32 MDT image')
    count=struct.unpack_from('<H',mdt,44)[0]
    if not 2<=count<=32 or 52+count*32>len(mdt):raise WifiError('invalid MDT program table')
    headers=[struct.unpack_from('<8I',mdt,52+i*32) for i in range(count)]
    first=headers[0];header=first[4]
    if first[0]==1 or first[1] or not 52+count*32<=header<=len(mdt):raise WifiError('invalid MDT authentication header')
    hashes=[i for i,h in enumerate(headers[1:],1) if h[6]&0x07000000==0x02000000]
    if len(hashes)!=1:raise WifiError('MDT must contain exactly one authentication segment')
    hash_index=hashes[0];hash_size=headers[hash_index][4]
    if not hash_size or header+hash_size>256*1024:raise WifiError('invalid MDT authentication size')
    segments=[(i,h) for i,h in enumerate(headers[1:],1) if i!=hash_index and h[0]==1 and h[5]]
    if not segments or not all(h[6]&0x08000000 for _,h in segments):raise WifiError('all WCNSS load segments must support relocation')
    low=min(h[3] for _,h in segments);high=max((h[3]+h[5]+4095)&~4095 for _,h in segments)
    if low&4095 or high>0xffffffff or high-low>6*1024*1024:raise WifiError('firmware exceeds the reserved 6 MiB arena')
    ranges=[]
    for _,h in segments:
        if h[4]>h[5] or h[3]+h[5]>0xfffff000 or any(h[3]<b and a<h[3]+h[5] for a,b in ranges):raise WifiError('overlapping or overflowing firmware segments')
        ranges.append((h[3],h[3]+h[5]))
    entry=struct.unpack_from('<I',mdt,24)[0]
    if not any(a<=entry<b for a,b in ranges):raise WifiError('firmware entry is outside its load segments')
    result={b'/firmware/wcnss/wcnss.mdt':mdt}
    if config is not None:result[b'/system/wifi-profile']=config
    split=any(h[4] and (h[1]>len(mdt) or h[4]>len(mdt)-h[1]) for _,h in segments)
    needed=[(i,h) for i,h in segments if h[4] and split]
    h=headers[hash_index]
    if header+hash_size!=len(mdt) and (h[1]>len(mdt) or h[4]>len(mdt)-h[1]):needed.append((hash_index,h))
    for i,h in needed:
        name=f'wcnss.b{i:02d}';data=read_file(folder,name,6*1024*1024)
        if len(data)!=h[4]:raise WifiError(f'{name} length differs from its signed header')
        result[b'/firmware/wcnss/'+name.encode()]=data
    nv=read_file(folder,'WCNSS_qcom_wlan_nv.bin',1024*1024)
    if len(nv)<5:raise WifiError('board calibration is too short')
    result[b'/firmware/wcnss/WCNSS_qcom_wlan_nv.bin']=nv
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ssid',required=True)
    parser.add_argument('--security',required=True,choices=('open','wpa2-psk','wpa3-sae'))
    parser.add_argument('--password-file',type=Path,help='read a password from a private file instead of prompting (one trailing newline is removed)')
    parser.add_argument('--mac',help='preserve an existing device MAC when creating a replacement profile')
    parser.add_argument('--channel',type=int,default=0)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    mac=bytearray(secrets.token_bytes(6));mac[0]=(mac[0]&0xfc)|2
    config={'ssid':args.ssid,'security':args.security,'mac':args.mac or ':'.join(f'{b:02x}' for b in mac),'channel':args.channel}
    try:
        if args.security=='open' and args.password_file:raise WifiError('open networks do not use a password')
        if args.security!='open':
            # Passwords are UTF-8 on the air regardless of the host locale.
            config['password']=args.password_file.read_text(encoding='utf-8').removesuffix('\n') if args.password_file else getpass.getpass('Wi-Fi password: ')
        encoded=profile_bytes(config)
        if args.security=='wpa2-psk':
            config.pop('password');config['pmk']=encoded[48:80].hex()
        # Provisioning creates a new profile; existing device identity is preserved.
        with open(os.open(args.output,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600),'w') as stream:
            json.dump(config,stream,indent=2);stream.write('\n')
    except (OSError,UnicodeError,WifiError) as error:parser.error(str(error))
    print(f'Created Wi-Fi profile: {args.output}; no device was accessed.')
if __name__=='__main__':main()
