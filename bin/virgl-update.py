#!/usr/bin/env python3

import pathlib
import requests
import subprocess
import base64

def error(msg: str) -> None:
    print('\033[31m' + msg + '\033[0m')

class Source:
    def __init__(self, filename: str, url: str):
        self.file = pathlib.Path(filename)
        self.url = url

    def sync(self) -> None:
        print(f'Syncing {self.file}...', end=' ', flush=True)
        req = requests.get(self.url)

        if not req.ok:
            error(f'Failed to retrieve file: {req.status_code} {req.reason}')
            return

        content = base64.b64decode(req.content) if 'format=TEXT' in self.url else req.content

        self.file.parent.mkdir(parents=True, exist_ok=True)
        with open(self.file, 'wb') as f:
            f.write(content)

        print('Done')

# Define the sources for virtio and virglrenderer headers
SOURCES = [
    Source('src/virtio/virtio-gpu/drm_hw.h', 'https://gitlab.freedesktop.org/virgl/virglrenderer/raw/main/src/drm_hw.h'),
    Source('src/virtio/virtio-gpu/venus_hw.h', 'https://gitlab.freedesktop.org/virgl/virglrenderer/raw/main/src/venus_hw.h'),
    Source('src/virtio/virtio-gpu/virgl_hw.h', 'https://gitlab.freedesktop.org/virgl/virglrenderer/raw/main/src/virgl_hw.h'),
    Source('src/virtio/virtio-gpu/virgl_protocol.h', 'https://gitlab.freedesktop.org/virgl/virglrenderer/raw/main/src/virgl_protocol.h'),
    Source('src/virtio/virtio-gpu/virgl_video_hw.h', 'https://gitlab.freedesktop.org/virgl/virglrenderer/raw/main/src/virgl_video_hw.h'),
    Source('src/virtio/virtio-gpu/virglrenderer_hw.h', 'https://gitlab.freedesktop.org/virgl/virglrenderer/raw/main/src/virglrenderer_hw.h'),
]

if __name__ == '__main__':
    git_toplevel = subprocess.check_output(['git', 'rev-parse', '--show-toplevel'],
                                           stderr=subprocess.DEVNULL).decode("ascii").strip()
    if not pathlib.Path(git_toplevel).resolve() == pathlib.Path('.').resolve():
        error('Please run this script from the root folder ({})'.format(git_toplevel))
        exit(1)

    for source in SOURCES:
        source.sync()
