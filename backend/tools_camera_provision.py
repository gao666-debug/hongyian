"""Provision once, write a private firmware header, and never print tokens/passwords."""
import argparse
import getpass
import json
import os
from pathlib import Path
import ssl
from urllib.parse import quote, urlsplit
import httpx


def provision(base, username, password, ca, output):
    if Path(output).exists():
        raise ValueError('Private configuration already exists; preserve it or move it aside before intentional credential rotation')
    base = base.rstrip('/')
    url = urlsplit(base)
    if url.scheme != 'https' or not url.hostname or url.username or url.password or url.query or url.fragment or url.path:
        raise ValueError('Use HTTPS origin only, e.g. https://server.example.com:8443')
    pem = Path(ca).read_text()
    if '-----BEGIN CERTIFICATE-----' not in pem: raise ValueError('Trusted certificate required')
    context = ssl.create_default_context(cafile=str(ca))
    with httpx.Client(base_url=base, verify=context, timeout=20, follow_redirects=False, trust_env=False) as client:
        reply = client.post('/api/v1/auth/login', json={'username': username, 'password': password})
        reply.raise_for_status()
        client.headers['Authorization'] = 'Bearer ' + reply.json()['data']['access_token']
        try:
            home = client.get('/api/v1/household')
            home.raise_for_status()
            devices = [d for d in home.json()['data']['devices'] if d['kind'] == 'gateway']
            if len(devices) != 1: raise ValueError('Household must contain exactly one gateway')
            device = devices[0]['device_id']
            endpoint = '/api/v1/devices/' + quote(device, safe='') + '/camera/provision'
            reply = client.post(endpoint)
            reply.raise_for_status()
            grant = reply.json()['data']
            header = '#pragma once\n#define CLOUD_ENABLED 1\n'
            for key, value in [('CLOUD_UPLOAD_URL', base + '/api/v1/devices/' + quote(device, safe='') + '/camera/frame'),
                               ('CLOUD_CAMERA_TOKEN', grant['camera_token']), ('CLOUD_CA_PEM', pem)]:
                header += '#define ' + key + ' ' + json.dumps(value, ensure_ascii=True) + '\n'
            target = Path(output)
            target.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'w') as f: f.write(header)
            os.chmod(target, 0o600)
        finally:
            client.post('/api/v1/auth/logout')
    return str(target)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Once-only camera provisioning; rerunning revokes old camera credentials.')
    parser.add_argument('--server', required=True)
    parser.add_argument('--ca', required=True)
    parser.add_argument('--username', default='guardian')
    parser.add_argument('--output', default='../esp32cam/HomeSafetyCamera/cloud_private.h')
    args = parser.parse_args()
    try:
        result = provision(args.server, args.username, getpass.getpass('家庭管理员密码: '), args.ca, args.output)
        print('Private camera configuration written:', result)
        print('Build and flash camera once. Do not send or commit this private file.')
    except Exception as error:
        # Avoid request/response bodies and headers containing credentials.
        print('Provisioning failed:', type(error).__name__, 'Check endpoint, certificate, account and backend version.')
        raise SystemExit(1)
