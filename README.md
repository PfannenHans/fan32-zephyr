# FAN32 Firmware

Zephyr firmware for local potentiometer controlled fans using my [FAN32 hardware project](https://github.com/PfannenHans/solar-aqi-pod).


## Set up west

- Create new venv and install west
- Install additional dependencies
```bash
west packages pip --install
```


## Clone using west

West is used for about everything in Zephyr projects, also for cloning...
To clone this repository and initialize the west workspace use
```bash
west init -m git@github.com:PfannenHans/fan32-zephyr.git --mr main fan32-zephyr
```
to clone the repo from gitlab using branch `main` and set up the west workspace in the folder `fan32-zephyr`.  
Then use
```bash
west update
```
inside the west workspace to pull the dependencies (e.g. Zephyr).


## Toolchain setup

- Install using west
```bash
west sdk install
west blobs fetch hal_espressif
```

## Compilation


- First time, run from git root (app folder):
```bash
west build --board esp32c3_fan32/esp32c3 -- -DEXTRA_CONF_FILE=debug.conf -DBOARD_ROOT=.
```
- Consecutive
```bash
west build
```

## Recommended reads before diving into development

Zephyr and its meta-tool `west` as well as other underlying tools like `CMake` can be quite much to wrap your head around. Here are some links to get you started:

- Getting started with CMake (the meta build system used by Zephyr) and it's concepts: [Hello CMake](https://coderefinery.github.io/cmake-workshop/hello-cmake/)
  - Short introduction, which should enable you to read, understand and possibly write `CMakeLists.txt`
  - It is also used in a lot of other C-Projects without Zephyr and could be considered an industry standard
- Getting started with Zephyr (the RTOS used in the project) and it's concepts: [Practical Zephyr Series](https://interrupt.memfault.com/tags#practical-zephyr-series)
  - Zephyr is part of the Linux-foundation and thus incorporates a lot of Linux concepts like `Kconfig` and `devicetree`
  - Read at least Part 1 and 2 for a general understanding and part 6 to know how the workspace and dependencies work
  - The devicetree parts are needed if you need to add support for custom boards
