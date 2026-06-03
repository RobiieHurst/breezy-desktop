import gi
import json
import os
import signal
import subprocess
import time
from pathlib import Path

import logging
logger = logging.getLogger('breezy_ui')

gi.require_version('GLib', '2.0')
from gi.repository import GLib, GObject

from .files import get_bin_home
from .desktopenvironment import hyprctl_path, is_hyprland_available

class VirtualDisplayManager(GObject.GObject):
    __gproperties__ = {
        'displays': (object, 'Displays', 'A list of the displays', GObject.ParamFlags.READWRITE)
    }
    _instance = None

    @staticmethod
    def get_instance():
        if not VirtualDisplayManager._instance:
            VirtualDisplayManager._instance = VirtualDisplayManager()

        return VirtualDisplayManager._instance

    def __init__(self):
        GObject.GObject.__init__(self)

        self.shm_path = Path("/dev/shm/breezy_virtual_displays.json")
        self._load_displays()
        self._prune_dead_display_processes()

        GLib.timeout_add_seconds(15, self._prune_dead_display_processes)

    def _process_dead(self, pid):
        if not isinstance(pid, int):
            return False

        if (not os.path.exists(f"/proc/{pid}")):
            return True

        try:
            if (os.waitpid(pid, os.WNOHANG) == (pid, 0)):
                return True
        except ChildProcessError:
            # process isn't tied to the current process, it's not dead if it's still open
            return False

        return False

    def _prune_dead_display_processes(self):
        new_displays = [disp for disp in self.displays if not self._display_dead(disp)]
        if new_displays != self.displays:
            self.set_property('displays', new_displays)
            self._save_processes()

        return GLib.SOURCE_CONTINUE

    def _display_dead(self, display):
        if display.get('backend') == 'hyprland':
            return not self._hyprland_output_exists(display['pid'])

        return self._process_dead(display['pid'])

    def _hyprland_output_exists(self, name):
        return name in self._hyprland_monitor_names()

    def _hyprland_monitor_names(self):
        hyprctl = hyprctl_path()
        if not hyprctl:
            return []

        try:
            result = subprocess.run(
                [hyprctl, 'monitors', '-j'],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            monitors = json.loads(result.stdout)
            return [monitor.get('name') for monitor in monitors]
        except Exception as e:
            logger.error(f"Failed to list Hyprland monitors: {e}")
            return []
     
    def create_virtual_display(self, width, height, framerate):
        if is_hyprland_available():
            self._create_hyprland_virtual_display(width, height, framerate)
            return

        try:
            process = subprocess.Popen(
                [f"{get_bin_home()}/virtualdisplay", "--width", str(int(round(width))), "--height", str(int(round(height))), "--framerate", str(framerate)],
                start_new_session=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

            if process.returncode is not None:
                logger.error(f"Failed to create virtual display: {process.stderr.read()}")
                return
            
            self.displays.append({
                'pid': process.pid,
                'width': width,
                'height': height,
                'framerate': framerate
            })
            self.set_property('displays', self.displays)
            self._save_processes()
        except Exception as e:
            logger.error(f"Failed to create virtual display: {e}")

    def _create_hyprland_virtual_display(self, width, height, framerate):
        hyprctl = hyprctl_path()
        name = f"breezy-virtual-{int(time.time() * 1000)}"

        try:
            names_before = set(self._hyprland_monitor_names())
            try:
                subprocess.run(
                    [hyprctl, 'output', 'create', 'headless', name],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True
                )
            except subprocess.CalledProcessError:
                subprocess.run(
                    [hyprctl, 'output', 'create', 'headless'],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True
                )
                names_after = set(self._hyprland_monitor_names())
                created_names = names_after - names_before
                if not created_names:
                    raise RuntimeError('Hyprland did not report a new headless output')
                name = sorted(created_names)[0]

            subprocess.run(
                [hyprctl, 'keyword', 'monitor', f"{name},{int(round(width))}x{int(round(height))}@{int(round(framerate))},auto,1"],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

            self.displays.append({
                'backend': 'hyprland',
                'pid': name,
                'width': width,
                'height': height,
                'framerate': framerate
            })
            self.set_property('displays', self.displays)
            self._save_processes()
        except Exception as e:
            logger.error(f"Failed to create Hyprland virtual display: {e}")
            try:
                subprocess.run([hyprctl, 'output', 'remove', name], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            except Exception:
                pass
     
    def destroy_virtual_display(self, pid: str) -> bool:
        display = next((disp for disp in self.displays if disp['pid'] == pid), None)
        if display and display.get('backend') == 'hyprland':
            return self._destroy_hyprland_virtual_display(pid)

        try:
            # Send SIGTERM to allow graceful shutdown
            os.killpg(pid, signal.SIGTERM)
            self.set_property('displays', [disp for disp in self.displays if disp['pid'] != pid])
            self._save_processes()
            return True
        except ProcessLookupError:
            # Process already gone, delete pid from list
            self.set_property('displays', [disp for disp in self.displays if disp['pid'] != pid])
            self._save_processes()
            return True
        except Exception as e:
            logger.error(f"Failed to kill process {pid}: {e}")
            return False

    def _destroy_hyprland_virtual_display(self, name):
        hyprctl = hyprctl_path()
        try:
            subprocess.run(
                [hyprctl, 'output', 'remove', name],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            self.set_property('displays', [disp for disp in self.displays if disp['pid'] != name])
            self._save_processes()
            return True
        except Exception as e:
            logger.error(f"Failed to remove Hyprland virtual display {name}: {e}")
            return False

    def _save_processes(self):
        with open(self.shm_path, 'w') as f:
            json.dump(self.displays, f)
    
    def _load_displays(self):
        displays = []
        if self.shm_path.exists():
            try:
                with open(self.shm_path, 'r') as f:
                    displays = json.load(f)
            except Exception:
                displays = []

        self.set_property('displays', displays)

    def do_set_property(self, prop, value):
        if prop.name == 'displays':
            self.displays = value

    def do_get_property(self, prop):
        if prop.name == 'displays':
            return self.displays
