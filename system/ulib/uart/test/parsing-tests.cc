// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/acpi_lite/debug_port.h>
#include <lib/uart/all.h>
#include <lib/uart/amlogic.h>
#include <lib/uart/ns8250.h>
#include <lib/zbi-format/driver-config.h>

#include <string>
#include <utility>

#include <zxtest/zxtest.h>

#include "../parse.h"

namespace {

#if defined(__i386__) || defined(__x86_64__)
constexpr bool kX86 = true;
#else
constexpr bool kX86 = false;
#endif

using uart::internal::ParseInts;

template <typename Uint>
void TestOneUint() {
  // No leading comma.
  {
    Uint u{0xe};
    EXPECT_EQ(ParseInts("", &u), 0u);
  }

  // Fewer elements than integers.
  {
    Uint u{0xe};
    EXPECT_EQ(ParseInts(",", &u), 0u);
  }

  {
    Uint u{0xe};
    ASSERT_EQ(ParseInts(",12", &u), 1u);
    EXPECT_EQ(12, u);
  }

  {
    Uint u{0xe};
    ASSERT_EQ(ParseInts(",-12", &u), 1u);
    EXPECT_EQ(static_cast<Uint>(-12), u);
  }

  {
    Uint u{0xe};
    ASSERT_EQ(ParseInts(",0xa", &u), 1u);
    EXPECT_EQ(0xa, u);
  }

  {
    Uint u{0xe};
    ASSERT_EQ(ParseInts(",-0xa", &u), 1u);
    EXPECT_EQ(static_cast<Uint>(-0xa), u);
  }

  {
    Uint u{0xe};
    ASSERT_EQ(ParseInts(",010", &u), 1u);
    EXPECT_EQ(8, u);
  }

  {
    Uint u{0xe};
    ASSERT_EQ(ParseInts(",-010", &u), 1u);
    EXPECT_EQ(static_cast<Uint>(-8), u);
  }

  // More elements than integers.
  {
    Uint u{0xe};
    ASSERT_EQ(ParseInts(",12,34", &u), 1);
    EXPECT_EQ(u, 12);
  }
}

template <typename UintA, typename UintB>
void TestTwoUints() {
  // No leading comma.
  {
    UintA uA{0xe};
    UintB uB{0xe};
    EXPECT_EQ(ParseInts("", &uA, &uB), 0u);
  }

  // Fewer elements than integers: no elements.
  {
    UintA uA{0xe};
    UintB uB{0xe};
    EXPECT_EQ(ParseInts(",", &uA, &uB), 0u);
  }

  // Fewer elements than integers: one element.
  {
    UintA uA{0xe};
    UintB uB{0xe};
    EXPECT_EQ(ParseInts(",12", &uA, &uB), 1u);
    EXPECT_EQ(uA, 12u);
    EXPECT_EQ(uB, 0xe);
  }

  {
    UintA uA{0xe};
    UintB uB{0xe};
    ASSERT_EQ(ParseInts(",12,34", &uA, &uB), 2u);
    EXPECT_EQ(12, uA);
    EXPECT_EQ(34, uB);
  }

  {
    UintA uA{0xe};
    UintB uB{0xe};
    ASSERT_EQ(ParseInts(",0x12,34", &uA, &uB), 2u);
    EXPECT_EQ(0x12, uA);
    EXPECT_EQ(34, uB);
  }

  {
    UintA uA{0xe};
    UintB uB{0xe};
    ASSERT_EQ(ParseInts(",12,0x34", &uA, &uB), 2u);
    EXPECT_EQ(12, uA);
    EXPECT_EQ(0x34, uB);
  }

  {
    UintA uA{0xe};
    UintB uB{0xe};
    ASSERT_EQ(ParseInts(",0x12,0x34", &uA, &uB), 2u);
    EXPECT_EQ(0x12, uA);
    EXPECT_EQ(0x34, uB);
  }

  // More elements than integers.
  {
    UintA uA;
    UintB uB;
    EXPECT_EQ(ParseInts(",12,34,56", &uA, &uB), 2u);
  }
}

TEST(ParsingTests, NoUints) {
  EXPECT_EQ(ParseInts(""), 0u);
  EXPECT_EQ(ParseInts(",12"), 0u);
  EXPECT_EQ(ParseInts(",12,34"), 0u);
}

TEST(ParsingTests, ParsingLargeValues) {
  {
    uint64_t u64{0xe};
    EXPECT_EQ(ParseInts(",0xffffffffffffffff", &u64), 1u);
    EXPECT_EQ(static_cast<uint64_t>(-1), u64);
  }
  {
    uint64_t u64{0xe};
    EXPECT_EQ(ParseInts(",0x0123456789", &u64), 1u);
    EXPECT_EQ(0x0123456789, u64);
  }
}

TEST(ParsingTests, Overflow) {
  {
    uint8_t u8{0xe};
    ASSERT_EQ(ParseInts(",0xabc", &u8), 1u);
    EXPECT_EQ(0xbc, u8);
  }
  {
    uint8_t u8{0xe};
    ASSERT_EQ(uart::internal::ParseInts(",0x100", &u8), 1u);
    EXPECT_EQ(0x00, u8);
  }
}

TEST(ParsingTests, ParsingLongStrings) {
  std::string waylong(",");
  waylong += std::string(100, '0');  // Longer than any integer size needs.
  waylong += "52";
  uint8_t u8{};
  EXPECT_EQ(ParseInts(waylong, &u8), 1u);
  EXPECT_EQ(052, u8);
  waylong[2] = 'x';
  EXPECT_EQ(ParseInts(waylong, &u8), 1u);
  EXPECT_EQ(0x52, u8);

  std::string longoverflow(",");
  longoverflow += std::string(100, '1');  // Extreme overflow.
  uint64_t u64{};
  EXPECT_EQ(ParseInts(longoverflow, &u64), 0u);
}

TEST(ParsingTests, OneUint8) { ASSERT_NO_FATAL_FAILURE(TestOneUint<uint8_t>()); }

TEST(ParsingTests, OneUint16) { ASSERT_NO_FATAL_FAILURE(TestOneUint<uint16_t>()); }

TEST(ParsingTests, OneUint32) { ASSERT_NO_FATAL_FAILURE(TestOneUint<uint32_t>()); }

TEST(ParsingTests, OneUint64) { ASSERT_NO_FATAL_FAILURE(TestOneUint<uint64_t>()); }

TEST(ParsingTests, TwoUint8s) {
  auto test = TestTwoUints<uint8_t, uint8_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, Uint8AndUint16) {
  auto test = TestTwoUints<uint8_t, uint16_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, Uint8AndUint32) {
  auto test = TestTwoUints<uint8_t, uint32_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, Uint8AndUint64) {
  auto test = TestTwoUints<uint8_t, uint32_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, TwoUint16s) {
  auto test = TestTwoUints<uint16_t, uint16_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, Uint16AndUint32) {
  auto test = TestTwoUints<uint16_t, uint32_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, Uint16AndUint64) {
  auto test = TestTwoUints<uint16_t, uint64_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, TwoUint32s) {
  auto test = TestTwoUints<uint32_t, uint32_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, Uint32AndUint64) {
  auto test = TestTwoUints<uint32_t, uint64_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

TEST(ParsingTests, TwoUint64s) {
  auto test = TestTwoUints<uint64_t, uint64_t>;
  ASSERT_NO_FATAL_FAILURE(test());
}

// Currently only these two are supported.
acpi_lite::AcpiDebugPortDescriptor kMmioDebugPort = {
    .type = acpi_lite::AcpiDebugPortDescriptor::Type::kMmio,
    .address = 1234,
    .length = 4,
};

acpi_lite::AcpiDebugPortDescriptor kPioDebugPort = {
    .type = acpi_lite::AcpiDebugPortDescriptor::Type::kPio,
    .address = 4321,
    .length = 2,
};

template <typename T, bool Matches, typename U>
void CheckMaybeCreateFromAcpi(const U& debug_port) {
  auto driver = T::MaybeCreate(debug_port);

  if constexpr (Matches) {
    ASSERT_TRUE(driver);
    if constexpr (std::is_same_v<decltype(driver->config()), zbi_dcfg_simple_t>) {
      EXPECT_EQ(driver->config().mmio_phys, debug_port.address);
    }

    if constexpr (std::is_same_v<decltype(driver->config()), zbi_dcfg_simple_pio_t>) {
      EXPECT_EQ(driver->config().base, debug_port.address);
    }
  } else {
    ASSERT_FALSE(driver);
  }
}

TEST(ParsingTests, Ns8250MmioDriver) {
  {
    auto driver = uart::ns8250::Mmio32Driver::MaybeCreate(kX86 ? "mmio,0xa,0xb" : "ns8250,0xa,0xb");
    ASSERT_TRUE(driver.has_value());
    EXPECT_STREQ(kX86 ? "mmio" : "ns8250", driver->config_name());
    const zbi_dcfg_simple_t& config = driver->config();
    EXPECT_EQ(0xa, config.mmio_phys);
    EXPECT_EQ(0xb, config.irq);
    EXPECT_EQ(0, config.flags);
  }
  {
    auto driver =
        uart::ns8250::Mmio32Driver::MaybeCreate(kX86 ? "mmio,0xa,0xb,0xc" : "ns8250,0xa,0xb,0xc");
    ASSERT_TRUE(driver.has_value());
    EXPECT_STREQ(kX86 ? "mmio" : "ns8250", driver->config_name());
    const zbi_dcfg_simple_t& config = driver->config();
    EXPECT_EQ(0xa, config.mmio_phys);
    EXPECT_EQ(0xb, config.irq);
    EXPECT_EQ(0xc, config.flags);
  }
  CheckMaybeCreateFromAcpi<uart::ns8250::Mmio32Driver, true>(kMmioDebugPort);
  CheckMaybeCreateFromAcpi<uart::ns8250::Mmio32Driver, false>(kPioDebugPort);
}

TEST(ParsingTests, Ns82508BMmioDriver) {
  {
    auto driver = uart::ns8250::Mmio8Driver::MaybeCreate("ns8250-8bit,0xa,0xb");
    ASSERT_TRUE(driver.has_value());
    EXPECT_STREQ("ns8250-8bit", driver->config_name());
    const zbi_dcfg_simple_t& config = driver->config();
    EXPECT_EQ(0xa, config.mmio_phys);
    EXPECT_EQ(0xb, config.irq);
    EXPECT_EQ(0, config.flags);
  }

  {
    auto driver = uart::ns8250::Mmio8Driver::MaybeCreate("ns8250-8bit,0xa,0xb,0xc");
    ASSERT_TRUE(driver.has_value());
    EXPECT_STREQ("ns8250-8bit", driver->config_name());
    const zbi_dcfg_simple_t& config = driver->config();
    EXPECT_EQ(0xa, config.mmio_phys);
    EXPECT_EQ(0xb, config.irq);
    EXPECT_EQ(0xc, config.flags);
  }
}

TEST(ParsingTests, Ns8250PioDriver) {
  auto driver = uart::ns8250::PioDriver::MaybeCreate("ioport,0xa,0xb");
  ASSERT_TRUE(driver.has_value());
  EXPECT_STREQ("ioport", driver->config_name());
  const zbi_dcfg_simple_pio_t& config = driver->config();
  EXPECT_EQ(0xa, config.base);
  EXPECT_EQ(0xb, config.irq);

  CheckMaybeCreateFromAcpi<uart::ns8250::PioDriver, false>(kMmioDebugPort);
  CheckMaybeCreateFromAcpi<uart::ns8250::PioDriver, true>(kPioDebugPort);
}

TEST(ParsingTests, Ns8250LegacyDriver) {
  auto driver = uart::ns8250::PioDriver::MaybeCreate("legacy");
  ASSERT_TRUE(driver.has_value());
  EXPECT_STREQ("ioport", driver->config_name());
  const zbi_dcfg_simple_pio_t& config = driver->config();
  EXPECT_EQ(0x3f8, config.base);
  EXPECT_EQ(4, config.irq);
}

TEST(ParsingTests, Pl011Driver) {
  {
    auto driver = uart::pl011::Driver::MaybeCreate("pl011,0xa,0xb");
    ASSERT_TRUE(driver.has_value());
    EXPECT_STREQ("pl011", driver->config_name());
    const zbi_dcfg_simple_t& config = driver->config();
    EXPECT_EQ(0xa, config.mmio_phys);
    EXPECT_EQ(0xb, config.irq);
    EXPECT_EQ(0, config.flags);
  }
  {
    auto driver = uart::pl011::Driver::MaybeCreate("pl011,0xa,0xb,0xc");
    ASSERT_TRUE(driver.has_value());
    EXPECT_STREQ("pl011", driver->config_name());
    const zbi_dcfg_simple_t& config = driver->config();
    EXPECT_EQ(0xa, config.mmio_phys);
    EXPECT_EQ(0xb, config.irq);
    EXPECT_EQ(0xc, config.flags);
  }

  CheckMaybeCreateFromAcpi<uart::pl011::Driver, false>(kMmioDebugPort);
  CheckMaybeCreateFromAcpi<uart::pl011::Driver, false>(kPioDebugPort);
}

TEST(ParsingTests, Pl011QemuDriver) {
  auto driver = uart::pl011::Driver::MaybeCreate("qemu");
  ASSERT_TRUE(driver.has_value());
  EXPECT_STREQ("pl011", driver->config_name());
  const zbi_dcfg_simple_t& config = driver->config();
  EXPECT_EQ(0x09000000, config.mmio_phys);
  EXPECT_EQ(33, config.irq);
  EXPECT_EQ(ZBI_KERNEL_DRIVER_IRQ_FLAGS_LEVEL_TRIGGERED | ZBI_KERNEL_DRIVER_IRQ_FLAGS_POLARITY_HIGH,
            config.flags);
}

TEST(ParsingTests, AmlogicDriver) {
  {
    auto driver = uart::amlogic::Driver::MaybeCreate("amlogic,0xa,0xb");
    ASSERT_TRUE(driver.has_value());
    EXPECT_STREQ("amlogic", driver->config_name());
    const zbi_dcfg_simple_t& config = driver->config();
    EXPECT_EQ(0xa, config.mmio_phys);
    EXPECT_EQ(0xb, config.irq);
    EXPECT_EQ(0, config.flags);
  }

  {
    auto driver = uart::amlogic::Driver::MaybeCreate("amlogic,0xa,0xb,0xc");
    ASSERT_TRUE(driver.has_value());
    EXPECT_STREQ("amlogic", driver->config_name());
    const zbi_dcfg_simple_t& config = driver->config();
    EXPECT_EQ(0xa, config.mmio_phys);
    EXPECT_EQ(0xb, config.irq);
    EXPECT_EQ(0xc, config.flags);
  }

  CheckMaybeCreateFromAcpi<uart::amlogic::Driver, false>(kMmioDebugPort);
  CheckMaybeCreateFromAcpi<uart::amlogic::Driver, false>(kPioDebugPort);
}

}  // namespace
