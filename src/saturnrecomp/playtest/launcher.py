"""The playtest launcher: the tester's disc, their tester code, and Play.

    python -m saturnrecomp.playtest.launcher [BUNDLE_DIR]

A release unpacks to a folder holding this launcher, the `saturn` executable, `playtest.json`
(the game's name, product, build, endpoint and arguments) and `disc.json` (the supported disc).
Packaged, the folder is the launcher's own; otherwise BUNDLE_DIR names it.
"""
import json
import os
import queue
import sys
import threading
import tkinter as tk
import urllib.request
import webbrowser
from tkinter import filedialog, messagebox, simpledialog, ttk

from . import disc_image, session, upload
from .paths import game_dir


class Launcher:
    def __init__(self, bundle):
        self.bundle = bundle
        self.info = json.load(open(os.path.join(bundle, "playtest.json")))
        self.manifest = json.load(open(os.path.join(bundle, "disc.json")))
        self.home = game_dir(self.info["product"])
        os.makedirs(self.home, exist_ok=True)
        self.settings = self._load_settings()
        self.events = queue.Queue()
        self.busy = False

        self.root = tk.Tk()
        self.root.title(f"{self.info['name']} playtest")
        self.root.resizable(False, False)
        frame = ttk.Frame(self.root, padding=16)
        frame.grid()
        ttk.Label(frame, text=self.info["name"], font=("TkDefaultFont", 14, "bold")).grid(column=0, row=0, columnspan=3, sticky="w")
        ttk.Label(frame, text=f"Build {self.info['build']}").grid(column=0, row=1, columnspan=3, sticky="w")
        self.update_link = ttk.Label(frame, foreground="#2f6fdb", cursor="hand2")
        self.update_link.grid(column=0, row=2, columnspan=3, sticky="w")

        ttk.Label(frame, text="Your disc").grid(column=0, row=3, sticky="w", pady=(12, 0))
        self.disc_label = ttk.Label(frame, width=52)
        self.disc_label.grid(column=0, row=4, columnspan=3, sticky="w")
        ttk.Button(frame, text="Choose file…", command=self.choose_file).grid(column=0, row=5, sticky="w")
        ttk.Button(frame, text="From a URL…", command=self.choose_url).grid(column=1, row=5, sticky="w")

        ttk.Label(frame, text="Tester code").grid(column=0, row=6, sticky="w", pady=(12, 0))
        self.token = tk.StringVar(value=self.settings.get("token", ""))
        ttk.Entry(frame, textvariable=self.token, width=52, show="•").grid(column=0, row=7, columnspan=3, sticky="w")

        self.play_button = ttk.Button(frame, text="Play", command=self.play)
        self.play_button.grid(column=0, row=8, sticky="w", pady=(16, 0))
        ttk.Button(frame, text="My sessions", command=self.open_sessions).grid(column=1, row=8, sticky="w", pady=(16, 0))
        self.status = ttk.Label(frame, width=52, wraplength=420)
        self.status.grid(column=0, row=9, columnspan=3, sticky="w", pady=(12, 0))

        self._show_disc()
        self.root.after(100, self._drain)
        threading.Thread(target=self._check_for_update, daemon=True).start()
        if self.settings.get("token"):
            threading.Thread(target=self._send_unsent, daemon=True).start()

    def choose_file(self):
        path = filedialog.askopenfilename(title="Your disc image", filetypes=[("Disc images", "*.cue *.iso *.bin"), ("All files", "*")])
        if path:
            self._start(self._use_image, path)

    def choose_url(self):
        url = simpledialog.askstring("Disc from a URL", "A link to your own copy of the disc: a .cue, .iso or .bin, or a .zip holding them.", parent=self.root)
        if url and url.strip():
            self._start(self._download, url.strip())

    def open_sessions(self):
        webbrowser.open(f"{self.info['endpoint']}/#token={self.token.get().strip()}")

    def play(self):
        if not self.settings.get("cue"):
            messagebox.showinfo("No disc", "Choose your disc image first.")
            return
        token = self.token.get().strip()
        self.settings["token"] = token
        self._save_settings()
        self.status.config(text="Playing. The session is sent every 10 minutes and when you quit."
                                if token else "Playing. Without a tester code this session stays on this computer.")
        self._start(self._play, token)

    def run(self):
        self.root.mainloop()

    def _check_for_update(self):
        try:
            request = urllib.request.Request(f"{self.info['endpoint']}/api/games/{self.info['product']}/latest",
                                             headers={"user-agent": upload.USER_AGENT})
            with urllib.request.urlopen(request, timeout=10) as r:
                latest = json.load(r)
        except (OSError, ValueError):
            return
        if latest.get("build") != self.info["build"] and latest.get("release_url"):
            self.events.put(("update", latest))

    def _download(self, url):
        def progress(done, total):
            self.events.put(("status", f"Downloading: {done >> 20} MB" + (f" of {total >> 20} MB" if total else "")))
        image = disc_image.download(url, os.path.join(self.home, "disc"), progress)
        self._use_image(image)

    def _drain(self):
        while not self.events.empty():
            kind, value = self.events.get()
            if kind == "status":
                self.status.config(text=value)
            elif kind == "disc":
                self._show_disc()
            elif kind == "done":
                self.busy = False
                self.play_button.state(["!disabled"])
            elif kind == "update":
                self.update_link.config(text=f"Build {value['build']} is out: download it")
                self.update_link.bind("<Button-1>", lambda _: webbrowser.open(value["release_url"]))
        self.root.after(100, self._drain)

    def _load_settings(self):
        try:
            return json.load(open(os.path.join(self.home, "launcher.json")))
        except (OSError, ValueError):
            return {}

    def _play(self, token):
        saturn = os.path.join(self.bundle, "saturn.exe" if sys.platform == "win32" else "saturn")

        def sent(ok, ended):
            if ended:
                self.events.put(("status", "Session sent. Its review shows under My sessions." if ok
                                 else "Could not send the session. The launcher tries again next time it starts."))
        session.play(saturn, self.settings["cue"], self.info, self.info["endpoint"], token or None, self.info.get("args", ()), sent)
        if not token:
            self.events.put(("status", "Session kept on this computer."))

    def _save_settings(self):
        with open(os.path.join(self.home, "launcher.json"), "w") as f:
            json.dump(self.settings, f, indent=1)

    def _send_unsent(self):
        left = upload.send_unsent(self.info["endpoint"], self.settings["token"], os.path.join(self.home, "sessions"))
        if left:
            self.events.put(("status", f"{len(left)} earlier sessions are still waiting to be sent."))

    def _show_disc(self):
        image = self.settings.get("image")
        self.disc_label.config(text=os.path.basename(image) if image else "None chosen yet")

    def _start(self, work, *args):
        """Runs `work` off the window's thread, one job at a time."""
        if self.busy:
            return
        self.busy = True
        self.play_button.state(["disabled"])

        def run():
            try:
                work(*args)
            except Exception as e:                  # shown to the tester rather than lost with the thread
                self.events.put(("status", f"Something went wrong: {e}"))
            finally:
                self.events.put(("done", None))
        threading.Thread(target=run, daemon=True).start()

    def _use_image(self, image):
        self.events.put(("status", "Checking the disc…"))
        errors, warnings = disc_image.verify(image, self.manifest)
        if errors:
            self.events.put(("status", "This is not the disc the build is for:\n" + "\n".join(errors[:5])))
            return
        self.settings["image"] = image
        self.settings["cue"] = disc_image.cue_for(image, self.home)
        self._save_settings()
        self.events.put(("disc", None))
        self.events.put(("status", "Disc checked." + (" Its music tracks differ, so the music may too." if warnings else "")))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    bundle = os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else (argv[0] if argv else ".")
    Launcher(os.path.abspath(bundle)).run()


if __name__ == "__main__":
    main()
