In the tools directory there are the following shell scripts for linux:

- Debian_build-deb.sh is an experimental script that builds JS8Call and produces a Debian-style .deb installation file

- Linux-TestBuild-Qt6.sh is primarily for building Qt6 with the required modules for JS8Call from source code. By setting the QT_VERSION variable in the script you can build whatever version of Qt you want to test or experiment with

- Linux-User-Build-JS8Call.sh is an end-user script that will build and install JS8Call for Debian, Redhat/Fedora and Arch Linux systems for either x86_64 or arm64 architectures. This script should also work for Raspberry Pi 5

- tcp.py is a python script that connects to a server running on your own computer (127.0.0.1) at port 2442 to diagnose the JS8Call API for automated digital logging. Once connected, it automatically asks for the station's status, listens continuously for incoming messages from the server, prints those messages out, and ignores minor status updates it doesn't care about

- udp.py is the same thing as above, except using the UDP protocol instead of TCP

- tracking_diag.cpp is a source file that builds a diagnostic harness for JS8 frequency/timing tracking

- whitening_diag.cpp is a source file that builds a commandline harness for JS8 whitening/noise estimation

- llr_frame_benchmark.cpp runs paired-seed complex-bin AWGN trials through the
  actual 174-bit encoder, likelihood, BP, feedback and rescue code (sync and
  waveform extraction are assumed perfect). It reports complete correct-frame
  decodes as CSV. See its build command and arguments at the top of the file.
  Use its "sweep" argument for the 6x6 fixed-scale/erasure search, "final" for
  confirmation, and a final "coherent" argument to enable pilot-based blending.
  Its SNR is complex matched-bin signal power / noise power in dB.

- whitening_diag.cpp --llr-calibration-final TRIALS FIRST LAST STEP sweeps
  the waveform decoder around its 50% point with paired AWGN seeds; the
  --llr-calibration-gate variant compares the original phase RMS gate with
  0.20/0.60 and 0.25/0.75 rad. Its SNR is the real-sample synthesizer's dB
  setting (not the matched-bin SNR used by llr_frame_benchmark). Both report
  only decoded frames whose payload matches the transmitted message.

- decoder_cpu_benchmark.cpp replays a deterministic 90-second 12-kHz ring
  containing tiled A/E WAV fixtures from media/tests, weak competing carriers,
  and fixed-seed noise through one persistent five-mode JS8 decoder worker.
  Its "noise" input tests ghost candidates without known transmissions; the
  optional fourth argument "autosync" exercises all five modes once per
  second (stress case). Build once normally and once with the five reference
  switches listed at the top of decoder_cpu_benchmark.cpp to compare all
  optimizations without changing any other decoder features. Both builds
  should include -DJS8_CPU_BENCHMARK for
  per-hotspot timing; the printed stage times include nested calls (e.g.,
  coherent scoring is part of tone-bin formation), while bpParts samples about
  one in 64 BP calls. Subtract nested stages rather than summing them. Its
  "diag" third argument counts attempted aided searches
  but enables debug formatting and must not be used for the CPU comparison.
  Output CPU duty is process CPU during decode calls divided by the virtual
  input duration, NOT a live receive-thread sample. The input is generated in
  advance; its virtual lag estimate does not simulate the UI busy queue.
  To isolate the BP edge lookup alone, compare two otherwise identical builds,
  one with -DJS8_BENCHMARK_LINEAR_BP_EDGES and one without.
  To isolate the check-centric BP loop, use
  -DJS8_BENCHMARK_ORIGINAL_CHECK_TO_BIT_LOOP in the reference build.
  To isolate timing-batched candidate scoring, use
  -DJS8_BENCHMARK_ORIGINAL_RANK_SCAN in the reference build;
  -DJS8_BENCHMARK_VERIFY_RANK_SCAN verifies every score against the original
  bit-for-bit and must not be used for CPU timings.
  Passing "pressure" as the fifth argument after "autosync" injects a queued
  frame shortly after each decode begins, exercising inline cancellation.
  This is a correctness/stress scenario, not a Pi performance estimate.

- optional_work_test.cpp covers deadline and pending-queue admission without
  Qt. JS8_DISABLE_ADAPTIVE_OPTIONAL=1 disables the runtime optional-work gate
  for paired decode comparisons.

Decoder calibration overrides: JS8_LLR_SCALE (positive fixed multiplier,
default 2), JS8_LLR_ERASURE_THRESH (nonnegative, default 0),
JS8_COHERENT_GOOD_RMS_RAD / JS8_COHERENT_POOR_RMS_RAD (default 0.20/0.60).
For the previous normalized-likelihood baseline set JS8_LLR_SCALE=1,
JS8_LLR_ERASURE_THRESH=0.25, JS8_COHERENT_GOOD_RMS_RAD=0.12,
JS8_COHERENT_POOR_RMS_RAD=0.35, and JS8_LLR_FRAME_NORMALIZATION=1.
