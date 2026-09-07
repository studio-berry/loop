Category: fixed
Audience: developers
Breaking-Change: no
Summary: Fix the systematic Windows native Quick startup crash and harden its smoke probe. The global log message handler scrubs on whatever thread logs, so the render thread and main thread reached the shared static QRegularExpression matchers concurrently during scene-graph startup; scrub() now compiles those patterns exactly once via std::call_once (observed invalid-regex warning plus 0xC0000005 after scene_graph_initialized on GPU-less runners). The relocated-install smoke probe also retries the native launch up to 3 times; a systematic startup failure still fails every attempt and fails closed.
