Category: fixed
Audience: developers
Breaking-Change: no
Summary: Retry the Windows relocated-install native LoopEditor Quick startup probe up to 3 times before failing. The native D3D11 probe can fault during render-thread teardown after the scene graph already reports ready on GPU-less runners; a systematic startup failure still fails every attempt and fails closed.
