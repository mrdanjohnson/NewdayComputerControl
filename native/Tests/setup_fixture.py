#!/usr/bin/env python3
"""Loopback-only client-setup API fixture. All credentials are synthetic."""
import json,sys
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
wrong=len(sys.argv)>1 and sys.argv[1]=='wrong-device'
class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def response(self,obj,cookie=None):
        data=json.dumps(obj).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)))
        if cookie:self.send_header('Set-Cookie',cookie)
        self.end_headers();self.wfile.write(data)
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))) or '{}')
        if self.path=='/ui/login':
            assert body=={'password':'synthetic admin password'}
            self.response({'ok':True},'mc_session=synthetic-session; HttpOnly; Path=/');return
        assert self.headers.get('Cookie')=='mc_session=synthetic-session'
        if self.path=='/api/v1/pairing/window':
            assert body=={'duration_s':300};self.response({'pairing_code':'12345678','seconds_remaining':300});return
        assert self.path=='/ui/logout';self.response({'ok':True})
    def do_GET(self):
        assert self.headers.get('Authorization')=='Bearer synthetic-read'
        if self.path=='/api/v1/status':self.response({'device':{'device_id':{'value':'wrong' if wrong else 'mac-fixture','source':'device'}}});return
        assert self.path=='/api/v1/agent/status';self.response({'session_active':True,'agent_instance_id':'ag-fixture'})
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
print(server.server_port,flush=True);server.serve_forever()
