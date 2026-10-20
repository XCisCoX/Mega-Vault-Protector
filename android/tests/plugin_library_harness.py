"""Run the real provider model + parser JVM tests; no Android/JNI/device claims."""
import argparse
import os
from pathlib import Path
import subprocess
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    dependencies = {
        'junit.jar': 'https://repo.maven.apache.org/maven2/junit/junit/4.13.2/junit-4.13.2.jar',
        'hamcrest.jar': 'https://repo.maven.apache.org/maven2/org/hamcrest/hamcrest-core/1.3/hamcrest-core-1.3.jar',
        'json.jar': 'https://repo.maven.apache.org/maven2/org/json/json/20240303/json-20240303.jar',
    }
    for name, url in dependencies.items():
        target = args.scratch / name
        if not target.exists():
            urllib.request.urlretrieve(url, target)
    root = Path(__file__).resolve().parents[1] / 'app/src'
    sources = [root / 'main/java/org/megavideoprotect/app' / name for name in
               ('PluginLibraryTarget.kt', 'PluginPolicy.kt', 'PluginProviders.kt')]
    sources.append(root / 'test/java/org/megavideoprotect/app/PluginLibraryTargetTest.kt')
    java = next(args.tools.glob('*/bin/java.exe'))
    compiler_cp = os.pathsep.join(str(p) for p in args.tools.glob('*.jar'))
    runtime_cp = os.pathsep.join(str(p) for p in [args.tools / 'kotlin-stdlib.jar',
        args.tools / 'annotations.jar', *(args.scratch / name for name in dependencies)])
    out = args.scratch / 'classes'
    subprocess.run([str(java), '-cp', compiler_cp, 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
        '-no-stdlib', '-no-reflect', '-jvm-target', '17', '-classpath', runtime_cp,
        '-d', str(out), *(str(p) for p in sources)], check=True)
    subprocess.run([str(java), '-cp', str(out) + os.pathsep + runtime_cp,
        'org.junit.runner.JUnitCore', 'org.megavideoprotect.app.PluginLibraryTargetTest'], check=True)


if __name__ == '__main__':
    main()
