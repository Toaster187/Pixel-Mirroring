## 2023-10-27 - [Network Scanner Thread Pooling]
**Learning:** Network scanning via repeated sequential thread batching is highly inefficient and suffers from the convoy effect (one slow thread holds up the whole batch) and OS overhead from repeatedly allocating/deallocating hundreds of threads.
**Action:** Use a fixed-size worker pool (e.g., 50 threads) combined with a lock-free `std::atomic<int>` counter to continuously feed work. This eliminates thread churn and keeps all workers fully saturated until the queue is empty.

## 2026-07-27 - [Socket Teardown Order With Reader Threads]
**Learning:** Calling `closesocket()` while another thread is parked in `recv()` on that socket is not just untidy — the OS hands the freed descriptor number to the next socket, so the reader can silently end up on a stranger's connection. The same class of bug hid in `stop()` for both the video and control sockets.
**Action:** Always tear down in the order `shutdown()` → `join()` → `closesocket()`: shutdown wakes every blocked reader, the join proves nobody is inside the socket any more, and only then is it safe to release the descriptor. Related: when two threads can write the same socket, funnel all writes through one mutex-guarded helper that loops until the whole message is out — a partial `send()` corrupts the protocol just as effectively as a race.

## 2026-07-27 - [Measure The Artifact, Not The Source]
**Learning:** The shipped debug APK was ~60 MB while its own code was ~4 MB. The weight came entirely from dependencies nothing referenced (an icon pack for a single icon, WorkManager, a View-toolkit theme library, and Compose tooling pulled in via `debugImplementation`). Nobody noticed because the build succeeded and the source looked small.
**Action:** When a build product is unexpectedly large, open it and rank its entries by size before touching any code — the answer is usually a dependency, not the source. Beware that incremental APK packaging can leave multi-MB zero-filled holes, so measure size only on clean builds, and check content size separately from file size.

## 2026-07-28 - [Pick The Protocol Variant That Removes Failure Modes]
**Learning:** Adding audio via Opus would have required handling codec-config packets and decoder extradata — precisely the parts that fail silently and cannot be verified without a device. scrcpy also offers raw PCM, which removes the decoder, the config handling and an entire dependency at the cost of ~1.5 Mbit/s next to a 20 Mbit/s video stream.
**Action:** When a feature cannot be tested end to end before shipping, prefer the protocol variant with the fewest moving parts even if it is nominally less efficient, and make every failure path degrade instead of abort (here: any audio problem continues video-only). Verify protocol assumptions against the actual binary — inspecting the bundled `scrcpy-server.jar` confirmed the parameters existed instead of trusting documentation.
