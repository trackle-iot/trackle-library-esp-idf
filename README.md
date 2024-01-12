# trackle-esp-idf-component

This framework enables Trackle cloud connectivity with ESP32 based platforms using [Trackle C++ SDK](https://github.com/trackle-iot/trackle-library).

Documentation for this repository can be found [here](https://trackle-iot.github.io/trackle-library-esp-idf/v4).

## License
Unless stated elsewhere, file headers or otherwise, all files herein are licensed under an LGPLv3 license. For more information, please read the LICENSE file.

## Getting Started
### Hardware

You will basically just need a development host and an [ESP32 development board](https://www.espressif.com/en/products/hardware/development-boards) to get started.

### Development Host Setup

- Please clone this branch of the repository using
    ```
    git clone --recursive https://github.com/trackle-iot/trackle-esp-idf-component.git
    ```
  or you can download last stable [release](https://github.com/trackle-iot/trackle-esp-idf-component/releases)
   
  > Note that if you ever change the branch or the git head of either esp-idf or trackle-esp-idf-component, ensure that all the submodules of the git repo are in sync by executing git submodule update --init --recursive
   
- Copy the folder into components folder of your esp-idf project.


- Please refer to https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html for setting up ESP-IDF
  - ESP-IDF can be downloaded from https://github.com/espressif/esp-idf/
  - Please set your branch to `release/v4.3` or `release/v4.4` and pull in the latest changes.
  - IDF `release/v5.0` is not supported.
- For a production-ready starting point for making applications that connect to Trackle cloud using trackle-esp-idf-component, refer to the [Trackle template project](https://github.com/trackle-iot/trackle-firmware-template-project).

## Creating a Trackle Device
* Create an account on Trackle Cloud (https://trackle.cloud/)
* Open "My Devices" section from the drawer
* Click the button "Claim a device"
* Select the link "I don't have a device id", then Continue
* The Device Id will be shown on the screen and the private key file will be download with name <device_id>.der where <device_id> is Device ID taken from Trackle.
