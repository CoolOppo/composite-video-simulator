# ffmpeg_ntsc

## In `main()`:

- Process input file(s):
	- Test if they have audio/video streams
	- Scale and resample input

For each video frame:
1. Run `frame_copy_scale()`
2. Run `composite_layer()`, which runs all the processing and adds the new frame to the output stream

- Encode output

## `composite_layer()`
