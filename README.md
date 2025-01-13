# Videtronic V-Link Drivers

Welcome to the official Videtronic repository for the **v-link** drivers!  

The **v-link** product line integrates advanced serializer and deserializer technology featuring the **MAX96717** and **MAX96714** to deliver seamless and high-performance video data transmission over distances of up to 15 meters, thanks to the extended range capabilities of **GMSL2**.

## About Videtronic
At Videtronic, we design and manufacture various embedded vision products. Our goal is to create solutions that will be used in cutting-edge vision projects.

Experience our comprehensive range of services, from advanced embedded vision products to full-scale production. We deliver tailored solutions and reliable support to meet all your vision technology needs.

Visit us at https://www.videtronic.com

## Features
- **Serializer**: Based on MAX96717
- **Deserializer**: Based on MAX96714
- **Platform Compatibility**: Raspberry Pi 4, 5, Zero 2
- **Camera Module Support**: Raspberry Pi Camera Modules 1, 2, 3, AI, and HQ

## Why Choose V-Link?
- Seamless integration with Raspberry Pi platforms
- High compatibility with multiple camera modules
- Easy installation process
- Reliable performance for video data transmission

## Versioning
This repository is versioned based on the Raspberry Pi Linux kernel LTS branches. Each branch corresponds to a specific kernel version (e.g., `rpi-6.6.y`). Ensure you use the branch matching your kernel version for optimal compatibility.

You can find the official Raspberry Pi Linux LTS branches [here](https://github.com/raspberrypi/linux).


## Quickstart Guide

### Prerequisites
1. A Raspberry Pi 4, 5, or Zero 2 with **Raspberry Pi OS** installed.  
2. Essential build tools, including ```make``` and a C compiler, should be installed:
   ```bash
   sudo apt install build-essential
   ```
3. Linux kernel headers installed:
   ```bash
   sudo apt update
   sudo apt install raspberrypi-kernel-headers
   ```
4. Git installed (if not already):
   ```bash
   sudo apt install git
   ```

### Step 1: Determine Kernel Version
Check the current kernel version on your Raspberry Pi:
```bash
uname -r
```
For example, a Raspberry Pi 5 might return:
```
6.6.62+rpt-rpi-2712
```
This indicates you are using the **6.6.y** kernel branch.

### Step 2: Clone the Appropriate Branch
Clone the repository branch matching your kernel version. For the **rpi-6.6.y** kernel branch:
```bash
git clone --branch rpi-6.6.y https://github.com/Videtronic/v-link-rpi.git
cd v-link-rpi
```
### Step 3: Enter driver directory
```bash
cd v-link-ser
```
or
```bash
cd v-link-deser
```

### Step 4: Build the Drivers 
Use the provided `Makefile` to build the drivers:
```bash
make
```

### Step 5: Install the Drivers 
Install the drivers using:
```bash
sudo make install
```
**Repeat step 4 and 5 for v-link-deser**  

### Step 6: Configure the Device Tree Overlay
1. Copy the desired overlay file(s) to the `/boot/firmware/overlays` directory.  
Available overlays include (but are not limited to):
   * ```vlink-imx219.dtbo``` (for Raspberry Pi Camera Module 2)
   * ```vlink-imx708.dtbo``` (for Raspberry Pi Camera Module 3)
   * ```vlink-imx477.dtbo``` (for Raspberry Pi HQ Camera Module)  

   To copy a specific overlay, for example, the IMX219 overlay:  
   ```bash
   sudo cp overlays/vlink-imx219.dtbo /boot/firmware/overlays/
   ```  
   Or, to copy all available overlays:
   ```bash
   sudo cp overlays/vlink-*.dtbo /boot/firmware/overlays/
   ```
2. Add the overlay to your `config.txt`:
   ```bash
   sudo nano /boot/firmware/config.txt
   ```
   Add the following line to enable the overlay:
   ```
   dtoverlay=vlink-imx219
   ```
3. Disable camera auto detect feature. Change:
   ```bash
   camera_auto_detect=1
   ```
   to
   ```bash
   camera_auto_detect=0
   ```

**For quick test, one can load an overlay directly from overlays directory:**
```bash
cd overlays
sudo dtoverlay vlink-imx219.dtbo
```

### Step 7: Reboot
Reboot your Raspberry Pi for the changes to take effect:
```bash
sudo reboot
```

---

## Compatibility Matrix

| Raspberry Pi Model | Camera Module 1 | Camera Module 2 | Camera Module 3 | Camera Module AI | Camera Module HQ |  
|---------------------|-----------------|-----------------|-----------------|------------------|------------------|  
| Raspberry Pi 4     | ✅               | ✅               | ✅               | ✅                | ✅                |  
| Raspberry Pi 5     | ✅               | ✅               | ✅               | ✅                | ✅                |  
| Raspberry Pi Zero 2| ✅               | ✅               | ✅               | ✅                | ✅                |  

> **Note**: All features have been thoroughly tested on the listed platforms and camera modules.

---

## Troubleshooting

- **Q: I get an error during `make`**  
  A: Ensure you have installed the Linux kernel headers with:  
  ```bash
  sudo apt install raspberrypi-kernel-headers
  ```  

- **Q: Overlay does not load properly**  
  A: Verify that the overlay file is in `/boot/firmware/overlays` and is referenced in `/boot/firmware/config.txt`.


## Contribution Guidelines
We welcome contributions to improve our drivers and documentation! To contribute:
1. Fork the repository.
2. Create a new branch for your feature or bugfix.
3. Submit a pull request with a clear description of your changes.

## Support
If you encounter issues or have any questions, feel free to [contact us](https://www.videtronic.pl) or open an issue on this repository.

---

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)  
![Raspberry Pi](https://img.shields.io/badge/RaspberryPi-4%2C5%2CZero2-blue)