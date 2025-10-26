# Building Wireless Android Auto Dongle

This repository contains a docker setup to make the build process easy.

If you choose to build without Docker, refer [the Buildroot user manual
](https://buildroot.org/downloads/manual/manual.html) for more details on dependencies and setup.

## Clone
```shell
$ git clone --recurse-submodules https://github.com/nisargjhaveri/WirelessAndroidAutoDongle
```

## Build with Docker
```shell
$ docker compose run --rm rpi4 # See docker-compose.yml for available options.
```

You can use `rpi0w`, `rpi02w`, `rpi3a`, `rpi4`, or `rpi5` to build and generate an sdcard image. Once the build is successful, it'll copy the generated sdcard image in `images/` directory.

The Docker build now uses the improved `build.sh` script that automatically handles patch changes and provides better error messages.

You can also use the `bash` service for more control over the build process and experimentation.

```shell
$ docker compose run --rm bash
```

## Build with manual setup
Once you have a recursive clone, you can manually build using the following methods.

### Using the build script (Recommended)
```shell
$ ./build.sh <board-name>
```

Available board names:
- `raspberrypi0w` - Raspberry Pi Zero W
- `raspberrypizero2w` - Raspberry Pi Zero 2 W
- `raspberrypi3a` - Raspberry Pi 3A+
- `raspberrypi4` - Raspberry Pi 4
- `raspberrypi5` - Raspberry Pi 5

The build script automatically detects patch changes and cleans build artifacts when necessary.

**Options:**
```shell
$ ./build.sh raspberrypi4 --linux-clean    # Clean only Linux build artifacts
$ ./build.sh raspberrypi4 --clean          # Clean entire build
$ ./build.sh raspberrypi4 --force-rebuild  # Force complete rebuild
```

### Traditional Buildroot method
```shell
$ cd buildroot
$ make BR2_EXTERNAL=../aa_wireless_dongle/ O=output/rpi0w raspberrypi0w_defconfig # Change output and defconfig for your board
$ cd output/rpi0w
$ make
```

When successful, this should generate the sd card image at `images/sdcard.img` in your output directory. See the "Install and Run" instructions above to use this image.

## Troubleshooting Build Issues

### Patch application errors
If you see errors about patches failing to apply, especially after modifying patch files, this means the patches are being applied to already-patched source code.

**Solution:**
```shell
# Using the build script (it should auto-detect, but you can force it):
$ ./build.sh <board-name> --linux-clean

# Or manually:
$ cd buildroot/output/<board-name>
$ make linux-dirclean
$ make
```

### Build fails after updating patches
The build script now automatically detects when patch files have changed and cleans the Linux build directory. If you're using Docker, the new compose configuration uses this script automatically.

If issues persist:
```shell
# For Docker builds - remove persistent volumes:
$ docker compose down -v
$ docker compose run --rm rpi4

# For manual builds - force rebuild:
$ ./build.sh raspberrypi4 --force-rebuild
```
