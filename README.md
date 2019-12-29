# Zenki 👀
TLDR let's get rude, say that all existing publicly available surveillance solutions are garbage and make a new lightweight and simple one. Core idea is to get a bunch of cheap RTSP+PoE cameras that encode directly into h264, and just store their already encoded stream as-is without any transcoding. That is essentially just a memcpy from network to disk, so something really cheap and weak can be used as video server for multiple full HD cameras, e.g. Raspberry Pi.

Using ultra-future technology, that surely hasn't been around for a decade, like HTML5 and `<video>` tag, we can dump camera stream into format that any modern browser can understand and play from a simple static html file.

Additionally, if we want to get smart and brash, we can even do some motion/event detection on the same Raspberry Pi (or other device that also has a hardware h264 decoder and shader engine):
1. extract keyframes on live rtsp stream
2. decode them using hardware decoder
3. feed them directly to shader engine as textures (using MMAL/DMA-BUF)
4. use shader engine to compare consecutive frames

I have no idea why hasn't anyone done this already. Probably for the same reason that I hadn't made any progress either in those 7 years since I first thought of it.

# Current status
Some day, when I'm old and have nothing better to do, I will find enough time to make a prototype of this project and then abruptly leave on a Harley bike into the sunset.

So for now there's only a

## Tiny RTSP->HLS ffmpeg script
Example of RTSP to HLS lightweight streaming using ffmpeg without transcoding.
Navigating through many ffmpeg options is rather uncomfortable, so here's a tiny example of how to use ffmpeg to make an hls stream out of essentially any source URL that provides a compatible stream. It does not perform transcoding, so memory and cpu footprint of this script is really small -- you can probably run dozens of them on a Raspberry Pi or something.

The `rtsp-to-hls.sh` script will write HLS segments into `segments/` dir and produce a `cctv.m3u8` file. `index.html` references that file and uses [hls.js](https://github.com/video-dev/hls.js/) to let browsers play HLS stream.

### Testing
1. Run `./rtsp-to-hls.sh rtps://camera_url` (google your camera name + rtsp url for exact url for your model; they're usually something like `rtsp://admin:admin@cam_ip/0` or `rtsp://cam_ip/h264_stream` or some other query string mostrocity; check camera model before buying)
2. Run e.g. `python -m SimpleHTTPServer` from the root dir of this repo in another terminal
3. Open `http://localhost:8000/` in your browser to see the video

### Troubleshooting
Not all cameras produce compatible streams. Some browsers will not accept some cameras streams. Maybe it would be possible to do some stream transformation without reencoding to mitigate that, but I haven't tried.

### License
Uses [hls.js](https://github.com/video-dev/hls.js/) version 0.13.0, which is distributed under Apache 2.0
WTFPL for the rest of this repo (or public domain if you're boring).
