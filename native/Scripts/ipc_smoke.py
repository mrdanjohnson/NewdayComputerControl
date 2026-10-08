#!/usr/bin/env python3
"""Temporarily supervise an UNPAIRED agent, prove signed XPC, then unload it.

Never imports personal configuration or connects to an ESP32. Refuses to
replace an already-loaded native service. All service state is isolated.
"""
from pathlib import Path
import os,plistlib,subprocess,sys,tempfile,shutil
app=Path(sys.argv[1]).resolve();domain=f'gui/{os.getuid()}';label='dev.maccontrol.native.agent'
if subprocess.run(['/bin/launchctl','print',domain+'/'+label],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0:
    raise SystemExit('Native service is already loaded; smoke test will not replace it.')
probe=Path(__file__).resolve().parents[1]/'build/untrusted-ipc-probe'
source=Path(__file__).resolve().parents[1]/'Tests/UntrustedIPC.swift'
subprocess.run(['xcrun','swiftc',str(source),'-o',str(probe)],check=True)
root=Path(tempfile.mkdtemp(prefix='maccontrol-native-smoke-'))
plist=root/'agent.plist'
job={'Label':label,'ProgramArguments':[str(app/'Contents/MacOS/MacControlAgentService')], 'MachServices':{label:True},'RunAtLoad':True,'EnvironmentVariables':{'MACCONTROL_TEST_ROOT':str(root/'state')},'StandardErrorPath':str(root/'stderr.log')}
plist.write_bytes(plistlib.dumps(job))
loaded=False
try:
    subprocess.run(['/bin/launchctl','bootstrap',domain,str(plist)],check=True);loaded=True
    result=subprocess.run([str(app/'Contents/MacOS/MacControl'),'--ipc-smoke'],capture_output=True,text=True,timeout=15)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr.strip());print((root/'stderr.log').read_text() if (root/'stderr.log').exists() else '')
        raise SystemExit(result.returncode)
    subprocess.run(['/bin/launchctl','print',domain+'/'+label],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
    print('Frontend exited; isolated service remains running.')
    denied=subprocess.run([str(probe)],capture_output=True,text=True,timeout=10)
    print(denied.stdout.strip())
    if denied.returncode or 'UNTRUSTED_PEER_REJECTED' not in denied.stdout:raise SystemExit('XPC peer rejection failed')
finally:
    if loaded:subprocess.run(['/bin/launchctl','bootout',domain+'/'+label],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
    shutil.rmtree(root)
