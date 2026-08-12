# ASCII Webcam Viewer

A native Windows webcam viewer that converts live video into colored ASCII characters. It is a small Win32/DirectShow program with no Electron, browser, or Python runtime.

## Features

- Select a DirectShow camera by index and detect available capture devices.
- Choose a capture preset, ASCII column count, frame cap, display scale, color boost, mirror mode, and inversion.
- Convert each frame to luminance-mapped characters while preserving sampled color.
- Render with GDI in a standard resizable Win32 window.
- Persist safe defaults in `asciicam_native.ini`.

The application is a viewer only. It does **not** implement a virtual-camera output device.

## How it works

```text
DirectShow camera -> Sample Grabber callback -> pixel sampling
                  -> luminance-to-character mapping -> colored GDI text
```

The frame callback copies the current sample into application-owned memory. The paint loop downsamples the image to the selected character grid, chooses a glyph for each brightness bucket, and renders the result.

## Build

Requirements:

- Windows 10 or newer
- Visual Studio 2022 Build Tools with the Desktop development with C++ workload
- A current Windows SDK

Run:

```bat
build_asciicam_exe.bat
```

The build links `comctl32`, `gdi32`, `ole32`, `oleaut32`, and `strmiids`. Compiled executables are intentionally not committed; build locally and use `run_asciicam.bat` or `run_asciicam_console.bat`.

## Configuration

`asciicam_native.ini` contains the committed baseline:

- `CameraIndex`
- `CapturePresetIndex`
- `AsciiCols`
- `FpsCap`
- `DisplayScalePercent`
- `ColorBoostPercent`
- `Mirror`
- `Invert`

Device indices are machine-specific. Change them locally if the default camera is not the desired one.

## Privacy and limitations

Frames stay in the process and are not transmitted or recorded by this code. Other camera drivers or operating-system components may have their own behavior. DirectShow device formats vary, and not every advertised preset is guaranteed to open. Virtual camera output is outside this project's scope.

## License

MIT. Windows APIs and toolchain components retain their respective licenses.
