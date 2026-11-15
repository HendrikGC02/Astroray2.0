// SPDX-License-Identifier: Apache-2.0
// The synchronous device checks as pure functions (0001 V6, unit part).
#include <doctest.h>

#include <string>

#include "host/device_probe.h"

using namespace astroray;

TEST_SUITE("fast") {
  TEST_CASE("device/cpu with avx2 accepted") {
    DeviceStatus s = cpu_device_status(true);
    CHECK(s.status == AR_OK);
    CHECK(s.reason.empty());
  }

  TEST_CASE("device/no avx2 refused") {
    DeviceStatus s = cpu_device_status(false);
    CHECK(s.status == AR_ERR_DEVICE);
    CHECK(s.reason == "CPU device needs AVX2");
  }

  TEST_CASE("device/cuda at the driver floor accepted") {
    DeviceStatus s = cuda_device_status(kMinDriverCudaVersion, 12, 0);
    CHECK(s.status == AR_OK);
    CHECK(s.reason.empty());
  }

  TEST_CASE("device/old driver refused") {
    DeviceStatus s = cuda_device_status(kMinDriverCudaVersion - 10, 12, 0);
    CHECK(s.status == AR_ERR_DEVICE);
    CHECK(s.reason.find("driver 13.0 is older than required 13.1") == 0);
    // No driver at all (version 0) is refused the same way, never accepted.
    CHECK(cuda_device_status(0, 12, 0).status == AR_ERR_DEVICE);
  }

  TEST_CASE("device/old compute capability refused") {
    CHECK(cuda_device_status(kMinDriverCudaVersion, 7, 5).status == AR_OK);
    DeviceStatus s = cuda_device_status(kMinDriverCudaVersion, 7, 0);
    CHECK(s.status == AR_ERR_DEVICE);
    CHECK(s.reason.find("7.0") != std::string::npos);
  }
}
