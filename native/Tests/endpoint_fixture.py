#!/usr/bin/env python3
"""Loopback-only v2 fixture. No hardware, pairing credentials, or Mac actions."""
import base64,hashlib,json,struct,sys,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
capture=sys.argv[1]; ignore_pings=len(sys.argv)>2 and sys.argv[2]=='ignore-pings'; events=[]; lock=threading.Lock()
def record(event):
    assert set(event)=={'event_id','agent_instance_id','session_id','seq','timestamp','type','command_id','payload'}
    assert len(json.dumps(event).encode())<=4096
    with lock:
        events.append(event)
        with open(capture,'w') as f:json.dump(events,f)
def frame(connection,obj):
    data=json.dumps(obj).encode(); connection.sendall(bytes([0x81,126])+struct.pack('!H',len(data))+data if len(data)>=126 else bytes([0x81,len(data)])+data)
def exact(stream,n):
    data=stream.read(n)
    if len(data)!=n:raise EOFError()
    return data
def receive(stream,connection):
    a,b=exact(stream,2); size=b&127
    if size==126:size=struct.unpack('!H',exact(stream,2))[0]
    if size==127:size=struct.unpack('!Q',exact(stream,8))[0]
    assert size<=4096
    mask=exact(stream,4) if b&128 else None
    data=exact(stream,size)
    if mask:data=bytes(x^mask[i%4] for i,x in enumerate(data))
    if a&15==8:raise EOFError()
    if a&15==9:
        if not ignore_pings:connection.sendall(bytes([0x8a,len(data)])+data)
        frame(connection,command)
        return receive(stream,connection)
    return json.loads(data)
command={'action':'launch_app','command_id':'fixture-command','bundle_id':'com.test.App'}
class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def response(self,status,obj):
        data=json.dumps(obj).encode();self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_POST(self):
        assert self.headers.get('Authorization')=='Bearer synthetic-secret'
        event=json.loads(exact(self.rfile,int(self.headers['Content-Length'])));record(event)
        if event['type']=='agent_hello':
            assert event['seq']==1 and event['session_id'] is None and event['payload']['protocol_version']==2
            self.response(200,{'ok':True,'session_id':'s-poll','heartbeat_interval_s':5})
        else:
            assert self.headers.get('X-Session-Id')=='s-poll' and event['session_id']=='s-poll'
            self.response(200,{'ok':True})
    def do_GET(self):
        assert self.headers.get('Authorization')=='Bearer synthetic-secret'
        if self.path=='/agent/v1/commands/pending':
            assert self.headers.get('X-Session-Id')=='s-poll';self.response(200,{'commands':[command]});return
        assert self.path=='/agent/v1/ws'
        key=self.headers['Sec-WebSocket-Key']
        accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101);self.send_header('Upgrade','websocket');self.send_header('Connection','Upgrade');self.send_header('Sec-WebSocket-Accept',accept);self.end_headers()
        try:
            event=receive(self.rfile,self.connection);record(event);assert event['seq']==1 and event['payload']['protocol_version']==2
            frame(self.connection,{'type':'hello_ack','session_id':'s-ws','protocol_version':2,'heartbeat_interval_s':5})
            event=receive(self.rfile,self.connection);record(event);assert event['session_id']=='s-ws' and event['seq']==2
            while True:record(receive(self.rfile,self.connection))
        except (EOFError,ConnectionError):pass
        self.close_connection=True
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
print(server.server_port,flush=True);server.serve_forever()
