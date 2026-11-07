from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='mqsim-pool-') as directory:
    subprocess.run([str(binary), directory], check=True)
