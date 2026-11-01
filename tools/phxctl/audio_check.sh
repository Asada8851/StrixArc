#!/bin/bash
# audio_check.sh: video_diag.sh's audio part only - does a file with sound stay black because of the audio device?
# Run: bash gpu/audio_check.sh (go.sh does)  ->  logs on the USB stick (nullarc_logs/<time>-audio)
VIDEO_ONLY=audio VIDEO_TAG=audio exec bash "$(cd "$(dirname "$0")" && pwd)/video_diag.sh" "$@"
