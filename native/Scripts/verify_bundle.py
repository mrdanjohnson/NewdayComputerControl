#!/usr/bin/env python3
from pathlib import Path
import json,plistlib,subprocess,sys
app=Path(sys.argv[1]).resolve()
info=plistlib.loads((app/'Contents/Info.plist').read_bytes())
assert info['CFBundleIdentifier']=='dev.maccontrol.native'
assert info['NSAppTransportSecurity']['NSAllowsLocalNetworking']
assert not info['NSAppTransportSecurity'].get('NSAllowsArbitraryLoads',False)
assert '_maccontrol._tcp' in info['NSBonjourServices']
assert (app/'Contents/Resources/MacControl.icns').is_file()
job=plistlib.loads((app/'Contents/Library/LaunchAgents/dev.maccontrol.native.agent.plist').read_bytes())
assert job['BundleProgram']=='Contents/MacOS/MacControlAgentService'
assert job['MachServices']=={'dev.maccontrol.native.agent':True}
assert job['RunAtLoad'] and job['KeepAlive'] and job['ThrottleInterval']==10
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
for name in ['MacControl','MacControlAgentService']:
    binary=app/'Contents/MacOS'/name
    architectures=subprocess.check_output(['lipo','-archs',str(binary)],text=True).split()
    assert set(architectures)=={'arm64','x86_64'}
    sign=subprocess.run(['codesign','-dvv',str(binary)],capture_output=True,text=True,check=True).stderr
    assert 'runtime)' in sign or ',runtime,' in sign
print(json.dumps({'app':str(app),'version':info['CFBundleShortVersionString'],'architectures':['arm64','x86_64'],'signature':'verified','launchAgent':'embedded','distribution':'local development; Developer ID/notarization still required for distribution'},indent=2))
