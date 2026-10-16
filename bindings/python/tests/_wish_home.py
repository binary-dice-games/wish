"""Points WISH_HOME at a scratch directory for this test process.

Clients connect as the OS user by default and servers keep identified
sessions' files under the wish home (see src/context/DESIGN.md); without
this, a test would write into the developer's real ~/.wish. Import it
before starting any server.
"""

import atexit
import os
import shutil
import tempfile

_home = tempfile.mkdtemp(prefix="wish_py_test_home_")
os.environ["WISH_HOME"] = _home
atexit.register(shutil.rmtree, _home, ignore_errors=True)
