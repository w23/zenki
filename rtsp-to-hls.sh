#!/bin/sh

[ $# -eq 0 ] && { echo "Usage: $0 rtsp://... <name>"; exit 1; }

SOURCE="$1"
NAME=${2:-cctv}
#ARCHIVE=${ARCHIVE:-archive}

HLS_TIME=${HLS_TIME:-5}
HLS_LIST_SIZE=${HLS_LIST_SIZE:-5}

ffmpeg -i "$SOURCE" \
        -f hls \
        -vsync 0 -copyts -vcodec copy -acodec copy \
        -movflags frag_keyframe+empty_moov \
        -hls_flags delete_segments+append_list \
        -hls_time $HLS_TIME \
        -hls_list_size $HLS_LIST_SIZE \
        -hls_base_url "segments/" \
        -hls_segment_filename "segments/$NAME-%d.ts" \
        "$NAME.m3u8"


# append this to also enable archiving
#        \
#        -f segment \
#        -segment_time 300 \
#        -segment_atclocktime 1 \
#        -strftime 1 \
#        -vcodec copy -acodec copy \
#        -map 0 \
#        "$ARCHIVE/$NAME-%F-%H-%M-%S.mp4"
