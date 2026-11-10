"""The product directory: the workbench finds both plugins, with this Python."""

import os
import subprocess
import sys

from myrcp import launch

import myteam_ui


def test_the_workbench_lists_both_plugins(tmp_path):
    env = dict(os.environ, XDG_STATE_HOME=str(tmp_path), MYRCP_PLUGIN_PATH="", MYRCP_PYTHON=sys.executable)
    out = subprocess.run([launch.launcher(), "--product", myteam_ui.PRODUCT, "--list"],
                         capture_output=True, text=True, env=env, timeout=60)  # fmt: skip
    assert out.returncode == 0, out.stderr
    assert "plugin myteam" in out.stdout, out.stdout
    assert "plugin myorg" in out.stdout, out.stdout
