# Embedded TSscatterfix

Source: `/home/rhardenb/repos/TSscatterfix`, revision
`e2fdd3cc03ed84954d350bb0793a6f9179b7d7ea` (2026-10-29).
Original author: Rob Hardenberg / PE1ITR. See LICENSE (MIT).

Unmodified C source snapshot. Only recovery, media, ranking, PSI and TS packet
sources are compiled into linrad_dvb. The SfRecovery API delivers RGB24 images
and service/progress snapshots directly; no Python helper or external executable
is needed. FFmpeg development libraries are required to build.
