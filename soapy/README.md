# SoapySDR module

`libradioberrySupport.so`, a [SoapySDR](https://github.com/pothosware/SoapySDR)
module (driver key `radioberry`) over the driver's `/dev/radioberry`.
Packaged as `soapysdr-module-radioberry`. Using it:
[SoapySDR](../docs/soapy.md).

## Layout

| Path | Contents |
|---|---|
| `src/radio.*` | the SoapySDR device |
| `src/convert.*` | sample formats, drive to gain code |
| `src/settings.*` | config file keys and timings |
| `src/find.*` | device discovery, gateware counter, open errors |
| `src/log.*` | logging into SoapySDR |
| `src/registration.cpp` | module entry points |
| `tests/` | unit tests against the daemon's fake device |
| `tests/hw/` | `rbsoapytest`, tests on the radio through SoapySDR |
| `docs/` | [design](docs/design.md), [hardware tests](docs/hardware-tests.md) |

## Building

With `libsoapysdr-dev` installed, from the repository root:

```
cmake -S . -B build -DRADIOBERRY_BUILD_SOAPY=ON
cmake --build build --target radioberrySupport soapy_tests
build/soapy/soapy_tests
```

Off by default; without it nothing needs SoapySDR.
