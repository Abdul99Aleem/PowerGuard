PowerGuard Remote CLI - Windows to QNX Raspberry Pi

FILES
  PowerGuard-Remote.ps1  - run this on Windows
  pg-remote-menu.sh      - uploaded to /tmp on QNX by the launcher

HOW TO RUN
  1. Extract both files to the same Windows folder.
  2. Open PowerShell in that folder.
  3. Execute:
       powershell -ExecutionPolicy Bypass -File .\PowerGuard-Remote.ps1
  4. Verify the SSH host fingerprint on first use. Authenticate as qnxuser when prompted by SCP and SSH.
  5. Choose menu option 1 to inspect which executable files actually exist.
  6. If PowerGuard is not running, use option 2 to start it; then option 1 again.
  7. Use options 3-12 for simulated inputs through the actual pg-console --live interface.
  8. Use option 13 to inspect the supervisor scorecard. Missing worker and job evidence is NOT a valid scored result.
  9. Option 15-18 reach the separate pgbench daemon, if deployed. They do NOT automatically connect that daemon to PowerGuard.

WHAT MUST EXIST ON PI
  /tmp/PowerGuard and /tmp/pg-console to control supervisor input.
  Optional /tmp/pgbench for standalone load scenarios.
  Optional /tmp/pg-state-leds for state LEDs.

IMPORTANT
  Three source folders / process names do not imply integrated IPC. The current
  GitHub supervisor uses name 'powerguard_k_v1', while the standalone pgbench
  uses 'powerguard_bench'. Manager forwarding, effect acknowledgements, actual
  CPU measurements and critical-job reports must be connected and inspected.
  The script does NOT spoof PG_EFFECT, critical-job records or supervisor states.
  The menu's numeric CPU and power inputs are injected readings, not measured
  real CPU saturation or end-to-end estimated-power verification.
  Do not present synthetic scorecard values as real QNX performance metrics.

SECURITY
  Target: qnxuser@10.166.126.80. Both SSH and SCP force aes128-ctr and hmac-sha2-256. No passwords stored. Avoid placing passwords in PowerShell history.
  Connect only to the board you control and verify SSH host key.
