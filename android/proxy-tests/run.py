import os,glob,subprocess,sys
root=os.path.abspath(os.path.join(os.path.dirname(__file__),'../..'))
tools=os.environ.get('PROXY_TEST_TOOLS',os.path.expanduser('~/AppData/Local/hermes/cache/scratch/android-proxy-tools'))
java=glob.glob(tools+'/*/bin/java.exe')[0]
libs=[f for f in glob.glob(tools+'/*.jar') if not f.endswith('proxy-tests.jar')];cp=os.pathsep.join(libs)
sources=glob.glob(root+'/android/proxy-tests/*.kt')
for n in ['ProxyProfile.kt','ProxyTransport.kt']:
 p=root+'/android/app/src/main/java/org/megavideoprotect/app/'+n
 if os.path.exists(p):sources.append(p)
out=tools+'/proxy-tests.jar'
r=subprocess.run([java,'-cp',cp,'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler','-no-stdlib','-no-reflect','-jvm-target','17','-classpath',cp,'-d',out]+sources)
if r.returncode:sys.exit(r.returncode)
ks=tools+'/fixture.p12'
if not os.path.exists(ks):
 subprocess.run([os.path.dirname(java)+'/keytool.exe','-genkeypair','-alias','fixture','-keyalg','RSA','-keysize','2048','-storetype','PKCS12','-keystore',ks,'-storepass','throwaway-fixture','-keypass','throwaway-fixture','-dname','CN=localhost','-ext','SAN=dns:localhost,ip:127.0.0.1','-validity','2'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
sys.exit(subprocess.run([java,'-cp',out+os.pathsep+cp,'org.megavideoprotect.app.ProxyTransportTestKt'],env=dict(os.environ,PROXY_TEST_KEYSTORE=ks)).returncode)
