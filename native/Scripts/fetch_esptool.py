#!/usr/bin/env python3
"""Pin and verify Espressif's standalone tools; clients need no Python install."""
from pathlib import Path
import concurrent.futures,hashlib,json,tarfile,urllib.request,shutil
root=Path(__file__).resolve().parents[1]
lock=json.loads((root/'Vendor/esptool-release.json').read_text())
cache=root/'Vendor/ESPTools'
cache.mkdir(parents=True,exist_ok=True)
def download(url,path,expected=None):
    if path.is_file() and (expected is None or hashlib.sha256(path.read_bytes()).hexdigest()==expected):return
    temporary=path.with_suffix('.download')
    with urllib.request.urlopen(url,timeout=60) as response,temporary.open('wb') as output:shutil.copyfileobj(response,output)
    if expected and hashlib.sha256(temporary.read_bytes()).hexdigest()!=expected:
        temporary.unlink();raise RuntimeError('Espressif archive checksum mismatch')
    temporary.replace(path)
def fetch(item):
    architecture,asset=item
    archive=cache/(architecture+'.tar.gz')
    download(asset['url'],archive,asset['sha256'])
    destination=cache/architecture
    if not (destination/'esptool').is_file():
        destination.mkdir(exist_ok=True)
        with tarfile.open(archive) as package:
            for name in ['esptool','LICENSE','README.md']:
                member=next(m for m in package.getmembers() if m.name.endswith('/'+name) and m.isfile())
                with package.extractfile(member) as source,(destination/name).open('wb') as target:shutil.copyfileobj(source,target)
        (destination/'esptool').chmod(0o755)
    print('Verified Espressif',lock['tag'],architecture,flush=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(fetch,lock['assets'].items()))
download(lock['source'],cache/'esptool-source.tar.gz',lock['source_sha256'])
