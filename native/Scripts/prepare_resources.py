#!/usr/bin/env python3
"""Copy pinned firmware/tools into the app before its outer code signature."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,tarfile
root=Path(__file__).resolve().parents[1]
bundle=Path(os.environ['TARGET_BUILD_DIR'])/os.environ['UNLOCALIZED_RESOURCES_FOLDER_PATH']
bundle.mkdir(parents=True,exist_ok=True)
release=json.loads((root/'Vendor/esptool-release.json').read_text())
for arch,asset in release['assets'].items():
    archive=root/'Vendor/ESPTools'/(arch+'.tar.gz')
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==asset['sha256'],'Flasher archive checksum mismatch'
    target=bundle/'ESPTools'/arch;target.mkdir(parents=True,exist_ok=True)
    with tarfile.open(archive) as package:
        member=next(m for m in package.getmembers() if m.name.endswith('/esptool') and m.isfile())
        with package.extractfile(member) as src,(target/'esptool').open('wb') as dst:shutil.copyfileobj(src,dst)
    (target/'esptool').chmod(0o755)
    subprocess.run(['codesign','--force','--sign',os.environ.get('EXPANDED_CODE_SIGN_IDENTITY') or '-','--identifier','dev.maccontrol.flash.'+arch,'--options','runtime','--entitlements',str(root/'Resources/FlashTool.entitlements'),str(target/'esptool')],check=True)
firmware=root.parent/'firmware/dist/esp32-s3'
target=bundle/'Firmware';target.mkdir(exist_ok=True)
files=[]
for name,offset in [('bootloader.bin',0),('partitions.bin',0x8000),('boot_app0.bin',0xe000),('firmware.bin',0x10000)]:
    source=firmware/name;shutil.copy2(source,target/name);data=source.read_bytes()
    files.append({'name':name,'offset':offset,'size':len(data),'sha256':hashlib.sha256(data).hexdigest()})
version=(firmware/'VERSION').read_text()
assert '1.6.1' in version,'Build the native-setup firmware before packaging'
(target/'manifest.json').write_text(json.dumps({'version':'1.6.1-phase6','chip':'esp32s3','flashBytes':8*1024*1024,'files':files},indent=2)+'\n')
shutil.copy2(firmware/'VERSION',target/'VERSION')
notices=bundle/'ThirdParty';notices.mkdir(exist_ok=True)
shutil.copy2(root/'Vendor/ESPTools/arm64/LICENSE',notices/'esptool-LICENSE')
source_archive=root/'Vendor/ESPTools/esptool-source.tar.gz'
assert hashlib.sha256(source_archive.read_bytes()).hexdigest()==release['source_sha256'],'Source archive checksum mismatch'
shutil.copy2(source_archive,notices/'esptool-v5.5.0-source.tar.gz')
shutil.copy2(root/'Vendor/esptool-release.json',notices/'esptool-provenance.json')
