// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zircon/system/utest/device-enumeration/common.h"

namespace {

TEST_F(DeviceEnumerationTest, NelsonTest) {
  static const char* kDevicePaths[] = {
      "sys/platform/pt/nelson",
      "sys/platform/pt/nelson/post-init/post-init",
      "sys/platform/05:05:1/aml-gpio/gpio",
      "sys/platform/05:05:1/aml-gpio/gpio-init",
      "sys/platform/gpio-h/aml-gpio/gpio",
      "sys/platform/nelson-buttons/nelson-buttons/buttons",
      "sys/platform/bt-uart/bluetooth-composite-spec/aml-uart/bt-transport-uart",
      "sys/platform/bt-uart/bluetooth-composite-spec/aml-uart/bt-transport-uart/bt-hci-broadcom",
      "sys/platform/i2c-0/i2c-0/aml-i2c",
      "sys/platform/i2c-1/i2c-1/aml-i2c",
      "sys/platform/i2c-2/i2c-2/aml-i2c",
      "sys/platform/aml_gpu/aml-gpu-composite/aml-gpu",
      "sys/platform/aml-usb-phy/aml_usb_phy/aml_usb_phy",
      "sys/platform/nelson-audio-i2s-out/aml_tdm/nelson-audio-i2s-out",
      "sys/platform/nelson-audio-pdm-in/aml_pdm/nelson-audio-pdm-in",
      "sys/platform/registers",  // registers device

      // XHCI driver will not be loaded if we are in USB peripheral mode.
      // "xhci/xhci/usb-bus",

      "sys/platform/i2c-2/i2c-2/aml-i2c/i2c/i2c-2-44/backlight/ti-lp8556",
      "sys/platform/canvas/aml-canvas",
      "sys/platform/tee/tee/optee",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/boot1/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/boot2/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/rpmb",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-000/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-001/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-002/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-003/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-004/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-005/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-006/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-007/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-008/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-009/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-010/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-011/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-012/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-013/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-014/block",
      "sys/platform/nelson-emmc/nelson_emmc/aml-sd-emmc/sdmmc/sdmmc-mmc/user/block/part-015/block",
      "sys/platform/i2c-0/i2c-0/aml-i2c/i2c/i2c-0-57/tcs3400_light/tcs-3400",
      "sys/platform/aml-nna/aml_nna",
      "sys/platform/nelson-clk/clocks",
      "sys/platform/nelson-clk/clocks/clock-init",
      "sys/platform/05:05:a/aml_thermal_pll/thermal",
      "class/thermal/000",
      // "sys/platform/05:03:1e/cpu",
      "sys/platform/aml-secure-mem/aml_securemem/aml-securemem",
      "sys/platform/pwm/aml-pwm-device/pwm-0",
      "sys/platform/pwm/aml-pwm-device/pwm-1",
      "sys/platform/pwm/aml-pwm-device/pwm-2",
      "sys/platform/pwm/aml-pwm-device/pwm-3",
      "sys/platform/pwm/aml-pwm-device/pwm-4",
      "sys/platform/pwm/aml-pwm-device/pwm-5",
      "sys/platform/pwm/aml-pwm-device/pwm-6",
      "sys/platform/pwm/aml-pwm-device/pwm-7",
      "sys/platform/pwm/aml-pwm-device/pwm-8",
      "sys/platform/pwm/aml-pwm-device/pwm-9",
      "sys/platform/aml-sdio/aml_sdio/aml-sd-emmc/sdmmc",
      "sys/platform/aml-sdio/aml_sdio/aml-sd-emmc/sdmmc/sdmmc-sdio",
      "sys/platform/aml-sdio/aml_sdio/aml-sd-emmc/sdmmc/sdmmc-sdio/sdmmc-sdio-1",
      "sys/platform/aml-sdio/aml_sdio/aml-sd-emmc/sdmmc/sdmmc-sdio/sdmmc-sdio-2",

      "sys/platform/display/display/amlogic-display/display-coordinator",
      "sys/platform/i2c-2/i2c-2/aml-i2c/i2c/i2c-2-73/ti_ina231_mlb/ti-ina231",
      "sys/platform/i2c-2/i2c-2/aml-i2c/i2c/i2c-2-64/ti_ina231_speakers/ti-ina231",
      "sys/platform/i2c-0/i2c-0/aml-i2c/i2c/i2c-0-112/shtv3",
      "sys/platform/gt6853-touch/gt6853_touch/gt6853",

      // Amber LED.
      "sys/platform/gpio-light/aml_light",

      "sys/platform/gpio-h/aml-gpio/gpio/gpio-82/spi_1/aml-spi-1/spi/spi-1-0/selina-composite/selina",

      "sys/platform/aml-ram-ctl/ram",

      // Thermistor/ADC
      "sys/platform/03:0a:27/thermistor/thermistor-device/therm-thread",
      "sys/platform/03:0a:27/thermistor/thermistor-device/therm-audio",
      "class/adc/000",
      "class/adc/001",
      "class/adc/002",
      "class/adc/003",
      "class/temperature/000",
      "class/temperature/001",

      "sys/platform/i2c-2/i2c-2/aml-i2c/i2c/i2c-2-45/tas58xx/TAS5805m",
      "sys/platform/i2c-2/i2c-2/aml-i2c/i2c/i2c-2-45/tas58xx/TAS5805m/brownout_protection",

      "sys/platform/gpio-c/aml-gpio/gpio/gpio-50/spi_0/aml-spi-0/spi/spi-0-0",

#ifdef include_packaged_drivers
      // OpenThread
      "sys/platform/gpio-c/aml-gpio/gpio/gpio-50/spi_0/aml-spi-0/spi/spi-0-0/nrf52811_radio/ot-radio",

      // WLAN
      "sys/platform/aml-sdio/aml_sdio/aml-sd-emmc/sdmmc/sdmmc-sdio/sdmmc-sdio-1/wifi/brcmfmac-wlanphyimpl",
      "sys/platform/aml-sdio/aml_sdio/aml-sd-emmc/sdmmc/sdmmc-sdio/sdmmc-sdio-1/wifi/brcmfmac-wlanphyimpl/wlanphy",
#endif

  };

  ASSERT_NO_FATAL_FAILURE(TestRunner(kDevicePaths, std::size(kDevicePaths)));

  static const char* kTouchscreenDevicePaths[] = {
      // One of these touch devices could be on P0/P1 boards.
      "sys/platform/nelson-buttons/nelson-buttons/buttons",
      // This is the only possible touch device for P2 and beyond.
      "sys/platform/gt6853-touch/gt6853-touch/gt6853",
  };
  ASSERT_NO_FATAL_FAILURE(device_enumeration::WaitForOne(
      cpp20::span(kTouchscreenDevicePaths, std::size(kTouchscreenDevicePaths))));

  ASSERT_NO_FATAL_FAILURE(device_enumeration::WaitForClassDeviceCount("class/power-sensor", 2));
}

}  // namespace
