"""Generate a local development CA and an HTTPS leaf certificate."""
import argparse
import ipaddress
import os
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--ip',action='append',default=[])
args=parser.parse_args()
ips=['127.0.0.1']+[str(ipaddress.ip_address(ip)) for ip in args.ip]
root=Path(__file__).resolve().parent/'.local/tls'
root.mkdir(parents=True,exist_ok=True)
os.chmod(root.parent,0o700)
os.chmod(root,0o700)
def run(*cmd):
    subprocess.run(['openssl',*cmd],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
ca_key=root/'development-ca.key';ca=root/'development-ca.crt'
if not ca.exists():
    run('req','-x509','-newkey','rsa:2048','-nodes','-days','365','-sha256','-keyout',str(ca_key),'-out',str(ca),
        '-subj','/CN=ElderCare Local Development CA','-addext','basicConstraints=critical,CA:TRUE',
        '-addext','keyUsage=critical,keyCertSign,cRLSign')
key=root/'server.key';csr=root/'server.csr';cert=root/'server.crt'
run('req','-new','-newkey','rsa:2048','-nodes','-sha256','-keyout',str(key),'-out',str(csr),'-subj','/CN=ElderCare Local Backend')
ext=root/'server.ext'
ext.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,'+','.join('IP:'+ip for ip in ips)+'\n')
run('x509','-req','-in',str(csr),'-CA',str(ca),'-CAkey',str(ca_key),'-CAcreateserial','-out',str(cert),'-days','90','-sha256','-extfile',str(ext))
for path in (ca_key,key):os.chmod(path,0o600)
print('Development HTTPS certificate generated for:',', '.join(ips))
print('Public CA to trust in the development App:',ca)
