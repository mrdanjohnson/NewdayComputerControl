#!/usr/bin/env python3
from pathlib import Path
import hashlib,json,platform,plistlib,subprocess,sys
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
resources=app/'Contents/Resources'
manifest=json.loads((resources/'Firmware/manifest.json').read_text())
assert manifest['chip']=='esp32s3' and manifest['flashBytes']==8*1024*1024
assert [f['name'] for f in manifest['files']]==['bootloader.bin','partitions.bin','boot_app0.bin','firmware.bin']
assert [f['offset'] for f in manifest['files']]==[0,0x8000,0xe000,0x10000]
for f in manifest['files']:
    data=(resources/'Firmware'/f['name']).read_bytes()
    assert len(data)==f['size'] and hashlib.sha256(data).hexdigest()==f['sha256']
provenance=json.loads((resources/'ThirdParty/esptool-provenance.json').read_text())
assert (resources/'ThirdParty/esptool-LICENSE').is_file()
assert hashlib.sha256((resources/'ThirdParty/esptool-v5.5.0-source.tar.gz').read_bytes()).hexdigest()==provenance['source_sha256']
for arch in ['arm64','x86_64']:
    binary=resources/'ESPTools'/arch/'esptool'
    assert subprocess.check_output(['lipo','-archs',str(binary)],text=True).strip()==arch
    subprocess.run(['codesign','--verify','--strict',str(binary)],check=True)
    sign=subprocess.run(['codesign','-dvv',str(binary)],capture_output=True,text=True,check=True).stderr
    assert 'runtime)' in sign or ',runtime,' in sign
    entitlements=subprocess.run(['codesign','-d','--entitlements',':-',str(binary)],capture_output=True,check=True)
    assert plistlib.loads(entitlements.stdout)['com.apple.security.cs.disable-library-validation']
host_arch='arm64' if platform.machine()=='arm64' else 'x86_64'
version=subprocess.check_output([str(resources/'ESPTools'/host_arch/'esptool'),'version'],text=True,timeout=20)
assert provenance['tag'][1:] in version
print(json.dumps({'app':str(app),'version':info['CFBundleShortVersionString'],'architectures':['arm64','x86_64'],'signature':'verified','launchAgent':'embedded','firmware':manifest['version'],'firmwareSHA256':'verified','flasher':provenance['tag'],'hostFlasherExecution':'passed','flasherArchitectures':['arm64','x86_64'],'distribution':'local development; Developer ID/notarization still required for distribution'},indent=2))
