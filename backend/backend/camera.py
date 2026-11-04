"""Bounded latest-frame relay. Camera credentials never grant household API access."""
import asyncio
import hashlib
import hmac
import secrets
import time
from datetime import datetime, timezone

from fastapi import Depends, Header, HTTPException, Request
from fastapi.responses import Response, StreamingResponse, JSONResponse


MAX_FRAME = 512 * 1024
FRESH_SECONDS = 15
VIEW_SECONDS = 20


class CameraService:
    def __init__(self, store, settings, homes):
        self.store, self.settings, self.homes = store, settings, homes
        self.latest = {}  # One frame per configured camera, no video queue or unbounded disk archive.
        self.last_upload = {}
        self.viewers = {}
        self.uploading = set()
        self.leases = {}
        with store.connection() as db:
            db.execute('''CREATE TABLE IF NOT EXISTS cameras(
                mode TEXT,device TEXT,token_hash TEXT,PRIMARY KEY(mode,device))''')

    def binding(self, device):
        with self.store.connection() as db:
            return db.execute('SELECT token_hash FROM cameras WHERE mode=? AND device=?',
                              (self.settings.mode, device)).fetchone()

    def status(self, device):
        active = self.demand(device)
        frame = self.latest.get(device)
        stamp = frame[0] if frame else None
        return {'configured': self.binding(device) is not None,
                'online': bool(frame and time.time() - stamp <= FRESH_SECONDS),
                'received_at': datetime.fromtimestamp(stamp, timezone.utc).isoformat() if stamp else None,
                'bytes': len(frame[1]) if frame else 0, 'transport': 'jpeg-relay',
                'target_fps': 2, 'max_frame_bytes': MAX_FRAME, 'viewing': active}

    def demand(self, device):
        now = time.monotonic()
        for key, grant in list(self.leases.items()):
            valid = grant['until'] > now and self.store.session(grant['session'])
            if valid:
                try: self.homes.authorize(grant['username'], grant['device'])
                except HTTPException: valid = False
            if not valid: self.leases.pop(key, None)
        active = any(g['device'] == device for g in self.leases.values())
        if not active: self.latest.pop(device, None)
        return active

    def clear_views(self, device):
        for key, grant in list(self.leases.items()):
            if grant['device'] == device: self.leases.pop(key, None)

    def authorize_camera(self, device, token):
        row = self.binding(device)
        hashed = hashlib.sha256((token or '').encode()).hexdigest()
        if device not in self.homes.configured_ids() or not row or not hmac.compare_digest(row['token_hash'], hashed):
            raise HTTPException(401, 'Camera credential invalid')

    def frame(self, device):
        frame = self.latest.get(device)
        if not frame or time.time() - frame[0] > FRESH_SECONDS:
            raise HTTPException(503, 'Camera offline; no fresh frame')
        return frame

    async def frames(self, device, user, disconnected):
        previous = None
        waiting = time.monotonic()
        try:
            while not await disconnected():
                if not self.store.session(user['token_hash']):
                    return
                if not self.demand(device): return
                try:
                    self.homes.authorize(user['username'], device)
                except HTTPException:
                    return
                try: stamp, jpeg = self.frame(device)
                except HTTPException:
                    if previous is not None or time.monotonic() - waiting > 20: return
                    await asyncio.sleep(0.2)
                    continue
                if stamp != previous:
                    previous = stamp
                    yield b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ' + str(len(jpeg)).encode() + b'\r\n\r\n' + jpeg + b'\r\n'
                await asyncio.sleep(0.1)
        finally:
            self.viewers[device] = max(0, self.viewers.get(device, 1) - 1)

    def install(self, app, session, envelope):
        @app.post('/api/v1/devices/{device}/camera/provision', status_code=201)
        async def provision(device, user=Depends(session)):
            self.homes.authorize(user['username'], device, control=True)
            with self.store.connection() as db:
                role = db.execute('SELECT role FROM home_members WHERE username=?', (user['username'],)).fetchone()
                kind = db.execute('SELECT kind FROM home_devices WHERE device=?', (device,)).fetchone()
                if not role or role['role'] != 'owner':
                    raise HTTPException(403, 'Owner required')
                if not kind or kind['kind'] != 'gateway':
                    raise HTTPException(422, 'Bind camera to a gateway')
                token = secrets.token_urlsafe(32)
                db.execute('INSERT OR REPLACE INTO cameras VALUES(?,?,?)',
                           (self.settings.mode, device, hashlib.sha256(token.encode()).hexdigest()))
            self.latest.pop(device, None)
            self.last_upload.pop(device, None)
            self.clear_views(device)
            # Returned once to provisioning tooling; only its hash is stored.
            return envelope({'camera_token': token, 'device_id': device,
                             'upload_path': f'/api/v1/devices/{device}/camera/frame'})

        @app.delete('/api/v1/devices/{device}/camera')
        async def revoke(device, user=Depends(session)):
            self.homes.authorize(user['username'], device, control=True)
            with self.store.connection() as db:
                role = db.execute('SELECT role FROM home_members WHERE username=?', (user['username'],)).fetchone()
                if not role or role['role'] != 'owner': raise HTTPException(403, 'Owner required')
                db.execute('DELETE FROM cameras WHERE mode=? AND device=?', (self.settings.mode, device))
            self.latest.pop(device, None)
            self.last_upload.pop(device, None)
            self.clear_views(device)
            return envelope({'revoked': True})

        @app.post('/api/v1/devices/{device}/camera/viewing')
        async def begin_view(device, user=Depends(session)):
            self.homes.authorize(user['username'], device)
            if not self.binding(device): raise HTTPException(404, 'Camera not configured')
            self.demand(device)
            if sum(g['device'] == device for g in self.leases.values()) >= 4:
                raise HTTPException(429, 'Too many camera viewers')
            key = secrets.token_urlsafe(18)
            self.leases[key] = {'device': device, 'session': user['token_hash'],
                                'username': user['username'], 'until': time.monotonic() + VIEW_SECONDS}
            return envelope({'view_id': key, 'expires_in': VIEW_SECONDS})

        @app.put('/api/v1/devices/{device}/camera/viewing/{view_id}')
        async def renew_view(device, view_id, user=Depends(session)):
            self.homes.authorize(user['username'], device)
            self.demand(device)
            grant = self.leases.get(view_id)
            if not grant or grant['device'] != device or grant['session'] != user['token_hash']:
                raise HTTPException(404, 'Viewing session expired')
            grant['until'] = time.monotonic() + VIEW_SECONDS
            return envelope({'view_id': view_id, 'expires_in': VIEW_SECONDS})

        @app.delete('/api/v1/devices/{device}/camera/viewing/{view_id}')
        async def end_view(device, view_id, user=Depends(session)):
            self.homes.authorize(user['username'], device)
            grant = self.leases.get(view_id)
            if grant and (grant['device'] != device or grant['session'] != user['token_hash']):
                raise HTTPException(404, 'Viewing session not found')
            self.leases.pop(view_id, None)
            self.demand(device)
            return envelope({'stopped': True})

        @app.get('/api/v1/devices/{device}/camera/demand')
        async def camera_demand(device, x_camera_token: str = Header(default='')):
            self.authorize_camera(device, x_camera_token)
            active = self.demand(device)
            return JSONResponse(envelope({'active': active}),
                                headers={'X-Camera-Active': '1' if active else '0', 'Cache-Control': 'no-store'})

        @app.post('/api/v1/devices/{device}/camera/frame')
        async def upload(device, request: Request, x_camera_token: str = Header(default='')):
            self.authorize_camera(device, x_camera_token)
            if not self.demand(device): raise HTTPException(409, 'No active viewers; pause upload')
            if request.headers.get('content-type', '').split(';')[0].strip().lower() != 'image/jpeg':
                raise HTTPException(415, 'JPEG required')
            now = time.monotonic()
            if now - self.last_upload.get(device, -100) < 0.18:
                raise HTTPException(429, 'Camera upload rate exceeded')
            if device in self.uploading: raise HTTPException(429, 'Camera upload already in progress')
            self.uploading.add(device)
            async def receive():
                raw = bytearray()
                async for part in request.stream():
                    if len(raw) + len(part) > MAX_FRAME: raise HTTPException(413, 'Frame too large')
                    raw.extend(part)
                return raw
            try:
                raw = await asyncio.wait_for(receive(), timeout=10)
            except asyncio.TimeoutError:
                raise HTTPException(408, 'Camera upload timed out')
            finally:
                self.uploading.discard(device)
            if len(raw) < 4 or raw[:2] != b'\xff\xd8' or raw[-2:] != b'\xff\xd9':
                raise HTTPException(415, 'Invalid JPEG markers')
            # Recheck after receiving the body: credentials might have been rotated meanwhile.
            self.authorize_camera(device, x_camera_token)
            if not self.demand(device): raise HTTPException(409, 'Viewing ended; pause upload')
            self.last_upload[device] = time.monotonic()
            self.latest[device] = (time.time(), bytes(raw))
            return envelope(self.status(device))

        @app.get('/api/v1/devices/{device}/camera')
        async def status(device, user=Depends(session)):
            self.homes.authorize(user['username'], device)
            return envelope(self.status(device))

        @app.get('/api/v1/devices/{device}/camera/frame')
        async def snapshot(device, user=Depends(session)):
            self.homes.authorize(user['username'], device)
            _, jpeg = self.frame(device)
            return Response(jpeg, media_type='image/jpeg', headers={'Cache-Control': 'no-store'})

        @app.get('/api/v1/devices/{device}/camera/stream')
        async def stream(device, request: Request, user=Depends(session)):
            self.homes.authorize(user['username'], device)
            if not self.binding(device): raise HTTPException(404, 'Camera not configured')
            if not self.demand(device): raise HTTPException(409, 'Start a viewing session first')
            if self.viewers.get(device, 0) >= 4: raise HTTPException(429, 'Too many camera viewers')
            self.viewers[device] = self.viewers.get(device, 0) + 1
            return StreamingResponse(self.frames(device, user, request.is_disconnected),
                                     media_type='multipart/x-mixed-replace; boundary=frame',
                                     headers={'Cache-Control': 'no-store', 'X-Accel-Buffering': 'no'})
