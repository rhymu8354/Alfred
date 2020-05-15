# Alfred

This is a server program which collects and distributes data to various
consumers on a network.  Clients may connect via wss (WebSocket over TLS) to
get/set state, or use simple HTTP/HTTPS GET requests for getting state only.
State is in a nested JSON object.  Clients can form any number of
subscriptions.  Each subscription targets a path in the state.  Clients receive
copies of state they have subscribed to, with/without metadata depending on
authorization.

## Usage

    Usage: Alfred

    Launch Alfred, attached to the terminal
    unless -s or --service is specified.

    Options:
      -c|--config PATH
        Use configuration saved in the file at the given PATH.
      -s|--service
        Run Alfred as a service, rather than directly
        in the terminal.  (NOTE: requires separate OS-specific installation steps.)

## Supported platforms / recommended toolchains

This is a portable C++11 application which depends only on the C++11 compiler,
the C and C++ standard libraries, and other C++11 libraries with similar
dependencies, so it should be supported on almost any platform.  The following
are recommended toolchains for popular platforms.

* Windows -- [Visual Studio](https://www.visualstudio.com/) (Microsoft Visual
  C++)
* Linux -- clang or gcc
* MacOS -- Xcode (clang)

## Building

This application may stand alone or be part of a larger project. It supports
[CMake](https://cmake.org/), and when standing alone, uses it to generate the
build system and provide the application with its dependencies.

There are two distinct steps in the build process:

1. Generation of the build system, using CMake
2. Compiling, linking, etc., using CMake-compatible toolchain

### Prerequisites

* [AsyncData](https://github.com/rhymu8354/AsyncData.git) - a library which
  contains classes and templates designed to support asynchronous and lock-free
  communication and data exchange between different components in a larger
  system.
* [Base64](https://github.com/rhymu8354/Base64.git) - a library which
  implements encoding and decoding data using the Base64 algorithm, which
  is defined in [RFC 4648](https://tools.ietf.org/html/rfc4648).
* [CMake](https://cmake.org/) version 3.8 or newer
* C++11 toolchain compatible with CMake for your development platform (e.g.
  [Visual Studio](https://www.visualstudio.com/) on Windows)
* [googletest](https://github.com/google/googletest.git) - The Google C++ Test
  framework (only needed to test the various dependency projects)
* [Hash](https://github.com/rhymu8354/Hash.git) - a library which implements
  various cryptographic hash and message digest functions.
* [Http](https://github.com/rhymu8354/Http.git) - a library which implements
  [RFC 7230](https://tools.ietf.org/html/rfc7230), "Hypertext Transfer Protocol
  (HTTP/1.1): Message Syntax and Routing".
* [HttpNetworkTransport](https://github.com/rhymu8354/HttpNetworkTransport.git) -
  a library which implements the transport interfaces needed by the `Http`
  library, in terms of the network endpoint and connection abstractions
  provided by the `SystemAbstractions` library.
* [Json](https://github.com/rhymu8354/Json.git) - a library which implements
  [RFC 7159](https://tools.ietf.org/html/rfc7159), "The JavaScript Object
  Notation (JSON) Data Interchange Format".
* `libtls`, `libssl`, and `libcrypto`, as provided by packages such as
  [LibreSSL](https://github.com/rhymu8354/LibreSSL.git)
* [MessageHeaders](https://github.com/rhymu8354/MessageHeaders.git) - a library
  which can parse and generate e-mail or web message headers
* [O9KClock](https://github.com/rhymu8354/O9KClock.git) - a library which
  implements a high-precision real-time clock, by combining the separate
  real-time and high-precision clocks provided by the operating system.
* [StringExtensions](https://github.com/rhymu8354/StringExtensions.git) - a
  library containing C++ string-oriented libraries, many of which ought to be
  in the standard library, but aren't.
* [SystemAbstractions](https://github.com/rhymu8354/SystemAbstractions.git) - a
  cross-platform adapter library for system services whose APIs vary from one
  operating system to another
* [Timekeeping](https://github.com/rhymu8354/Timekeeping.git) - a library
  of classes and interfaces dealing with tracking time and scheduling work
* [TlsDecorator](https://github.com/rhymu8354/TlsDecorator.git) - an adapter to
  use `LibreSSL` to encrypt traffic passing through a network connection
  provided by `SystemAbstractions`
* [Uri](https://github.com/rhymu8354/Uri.git) - a library that can parse and
  generate Uniform Resource Identifiers (URIs)
* [Utf8](https://github.com/rhymu8354/Utf8.git) - a library which implements
  [RFC 3629](https://tools.ietf.org/html/rfc3629), "UTF-8 (Unicode
  Transformation Format)".
* [WebSockets](https://github.com/rhymu8354/WebSockets.git) - a library which
  implements [RFC 6455](https://tools.ietf.org/html/rfc6455), "The WebSocket
  Protocol".

### Build system generation

Generate the build system using [CMake](https://cmake.org/) from the solution
root.  For example:

```bash
mkdir build
cd build
cmake -G "Visual Studio 15 2017" -A "x64" ..
```

### Compiling, linking, et cetera

Either use [CMake](https://cmake.org/) or your toolchain's IDE to build.
For [CMake](https://cmake.org/):

```bash
cd build
cmake --build . --config Release
```
