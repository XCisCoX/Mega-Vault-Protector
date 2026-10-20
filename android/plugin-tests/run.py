"""Real standalone JVM policy tests; catalogue/audio inputs are labelled fixtures."""
from pathlib import Path
import argparse, subprocess, os
p=argparse.ArgumentParser(); p.add_argument('--tools',type=Path,required=True); p.add_argument('--live',action='store_true'); a=p.parse_args()
r=Path(__file__).resolve().parents[2]; t=a.tools.resolve()
j=next(t.glob('jdk/**/bin/java.exe'))
cp=os.pathsep.join(map(str,(t/'kotlin/kotlinc/lib').glob('*.jar')))
out=t/'plugin-tests.jar'; dep=t/'json.jar'
subprocess.run([str(j),'-cp',cp,'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler','-kotlin-home',str(t/'kotlin/kotlinc'),'-cp',str(dep),'-include-runtime','-d',str(out),str(r/'android/app/src/main/java/org/megavideoprotect/app/PluginPolicy.kt'),str(r/'android/plugin-tests/PluginPolicyTest.kt'),str(r/'android/plugin-tests/PluginHttpTest.kt'),str(r/'android/plugin-tests/LivePluginTest.kt'),str(r/'android/app/src/main/java/org/megavideoprotect/app/PluginProviders.kt'),str(r/'android/app/src/main/java/org/megavideoprotect/app/PluginHttp.kt')],check=True)
key=t/'plugin-fixture.p12'
if not key.exists():
 subprocess.run([str(j.parent/'keytool.exe'),'-genkeypair','-alias','fixture','-keyalg','RSA','-keysize','2048','-validity','30','-dname','CN=localhost','-ext','SAN=dns:localhost','-storetype','PKCS12','-keystore',str(key),'-storepass','changeit','-keypass','changeit','-noprompt'],check=True)
subprocess.run([str(j),'-Djavax.net.ssl.trustStore='+str(key),'-Djavax.net.ssl.trustStorePassword=changeit','-cp',str(out)+os.pathsep+str(dep),'org.megavideoprotect.app.PluginPolicyTestKt',str(r/'plugins/monochrome/plugin.json'),str(key)],check=True)
if a.live:
 subprocess.run([str(j),'-cp',str(out)+os.pathsep+str(dep),'org.megavideoprotect.app.LivePluginTestKt',str(r/'plugins/monochrome/plugin.json')],check=True)
