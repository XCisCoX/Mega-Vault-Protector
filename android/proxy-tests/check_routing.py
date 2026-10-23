"""Static routing coverage; complements (does not replace) real transport fixtures."""
from pathlib import Path
p = Path(__file__).resolve().parents[1] / 'app/src/main/java/org/megavideoprotect/app'
for name in ['PluginHttp.kt','SyncV4Client.kt','SyncHttp.kt','AppUpdate.kt','PeerLink.kt','RemoteVault.kt','RemoteHttpDataSource.kt']:
    text=(p/name).read_text(encoding='utf-8')
    assert '.openConnection()' not in text, f'{name} bypasses app proxy'
    assert 'AppProxy.open(' in text, f'{name} missing proxy route'
print('PASS all seven internet clients use AppProxy')
