import os
import shutil
import subprocess


def is_hyprland():
    return bool(os.environ.get('HYPRLAND_INSTANCE_SIGNATURE'))


def hyprctl_path():
    return shutil.which('hyprctl')


def is_hyprland_available():
    return is_hyprland() and hyprctl_path() is not None


def hyprctl(*args):
    hyprctl = hyprctl_path()
    if not hyprctl:
        raise RuntimeError('hyprctl is not available')

    return subprocess.run(
        [hyprctl, *args],
        check=True,
        capture_output=True,
        text=True,
    )


def hyprland_recenter():
    return hyprctl('breezy-recenter')


def hyprland_set_plugin_option(name, value):
    return hyprctl('keyword', f'plugin:breezy:{name}', str(value))
