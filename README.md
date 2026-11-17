# ngx_brotli

Brotli is a generic-purpose lossless compression algorithm that compresses data
using a combination of a modern variant of the LZ77 algorithm, Huffman coding
and 2nd order context modeling, with a compression ratio comparable to the best
currently available general-purpose compression methods. It is similar in speed
with deflate but offers more dense compression.

ngx_brotli is a set of two nginx modules:

- ngx_brotli filter module - used to compress responses on-the-fly,
- ngx_brotli static module - used to serve pre-compressed files.


## Table of Contents

- [ngx\_brotli](#ngx_brotli)
  - [Table of Contents](#table-of-contents)
  - [Status](#status)
  - [Installation](#installation)
    - [Statically compiled](#statically-compiled)
    - [Dynamically loaded](#dynamically-loaded)
  - [Configuration directives](#configuration-directives)
    - [`brotli_static`](#brotli_static)
    - [`brotli`](#brotli)
    - [`brotli_types`](#brotli_types)
    - [`brotli_buffers`](#brotli_buffers)
    - [`brotli_comp_level`](#brotli_comp_level)
    - [`brotli_window`](#brotli_window)
    - [`brotli_min_length`](#brotli_min_length)
    - [`brotli_max_length`](#brotli_max_length)
    - [`brotli_bypass`](#brotli_bypass)
    - [`brotli_postpone`](#brotli_postpone)
    - [`brotli_no_buffer`](#brotli_no_buffer)
    - [`brotli_http_version`](#brotli_http_version)
    - [`brotli_proxied`](#brotli_proxied)
    - [`brotli_disable`](#brotli_disable)
  - [Variables](#variables)
    - [`$brotli_ratio`](#brotli_ratio)
  - [Sample configuration](#sample-configuration)
  - [Contributing](#contributing)
  - [License](#license)

## Status

Both Brotli library and nginx module are under active development.

## Installation

### Statically compiled

Checkout the latest `ngx_brotli` and build the dependencies:

```
git clone --recurse-submodules -j8 https://github.com/google/ngx_brotli
cd ngx_brotli/deps/brotli
mkdir out && cd out
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_FLAGS="-Ofast -m64 -march=native -mtune=native -flto -funroll-loops -ffunction-sections -fdata-sections -Wl,--gc-sections" -DCMAKE_CXX_FLAGS="-Ofast -m64 -march=native -mtune=native -flto -funroll-loops -ffunction-sections -fdata-sections -Wl,--gc-sections" -DCMAKE_INSTALL_PREFIX=./installed ..
cmake --build . --config Release --target brotlienc
cd ../../../..
```


    $ cd nginx-1.x.x
    $ export CFLAGS="-m64 -march=native -mtune=native -Ofast -flto -funroll-loops -ffunction-sections -fdata-sections -Wl,--gc-sections"
    $ export LDFLAGS="-m64 -Wl,-s -Wl,-Bsymbolic -Wl,--gc-sections"
    $ ./configure --add-module=/path/to/ngx_brotli
    $ make && make install
  
This will compile the module directly into Nginx.


### Dynamically loaded

    $ cd nginx-1.x.x
    $ ./configure --with-compat --add-dynamic-module=/path/to/ngx_brotli
    $ make modules

You will need to use **exactly** the same `./configure` arguments as your Nginx configuration and append `--with-compat --add-dynamic-module=/path/to/ngx_brotli` to the end, otherwise you will get a "module is not binary compatible" error on startup. You can run `nginx -V` to get the configuration arguments for your Nginx installation.

`make modules` will result in `ngx_http_brotli_filter_module.so` and `ngx_http_brotli_static_module.so` in the `objs` directory. Copy these to `/usr/lib/nginx/modules/` then add the `load_module` directives to `nginx.conf` (above the http block):
```nginx
load_module modules/ngx_http_brotli_filter_module.so;
load_module modules/ngx_http_brotli_static_module.so;
```



## Configuration directives

### `brotli_static`

- **syntax**: `brotli_static on|off|always`
- **default**: `off`
- **context**: `http`, `server`, `location`, `when`

Enables or disables checking of the existence of pre-compressed files with`.br`
extension. With the `always` value, pre-compressed file is used in all cases,
without checking if the client supports it.

The following directives are also taken into account: `brotli_http_version`, `brotli_proxied`, `brotli_disable`, and `gzip_vary`.

### `brotli`

- **syntax**: `brotli on|off`
- **default**: `off`
- **context**: `http`, `server`, `location`, `if in location`, `when`

Enables or disables on-the-fly compression of responses.

### `brotli_types`

- **syntax**: `brotli_types <mime_type> [..]`
- **default**: `text/html`
- **context**: `http`, `server`, `location`

Enables on-the-fly compression of responses for the specified MIME types
in addition to `text/html`. The special value `*` matches any MIME type.
Responses with the `text/html` MIME type are always compressed.

### `brotli_buffers`

- **syntax**: `brotli_buffers <number> <size>`
- **default**: `32 4k|16 8k`
- **context**: `http`, `server`, `location`

Sets the number and size of output buffers used to compress a response. By default, the buffer size is equal to one memory page. This is either 4K or 8K, depending on a platform.

### `brotli_comp_level`

- **syntax**: `brotli_comp_level <level>`
- **default**: `6`
- **context**: `http`, `server`, `location`, `when`

Sets on-the-fly compression Brotli quality (compression) `level`.
Acceptable values are in the range from `0` to `11`.

### `brotli_window`

- **syntax**: `brotli_window <size>`
- **default**: `512k`
- **context**: `http`, `server`, `location`

Sets Brotli window `size`. Acceptable values are `1k`, `2k`, `4k`, `8k`, `16k`,
`32k`, `64k`, `128k`, `256k`, `512k`, `1m`, `2m`, `4m`, `8m` and `16m`.

### `brotli_min_length`

- **syntax**: `brotli_min_length <length>`
- **default**: `20`
- **context**: `http`, `server`, `location`, `when`

Sets the minimum `length` of a response that will be compressed.
The length is determined only from the `Content-Length` response header field.

### `brotli_max_length`

- **syntax**: `brotli_max_length <length>`
- **default**: `0`
- **context**: `http`, `server`, `location`, `when`

Sets the maximum `length` of a response that will be compressed.
The length is determined only from the `Content-Length` response header field.

### `brotli_bypass`

- **syntax**: `brotli_bypass string ...`
- **default**: `-`
- **context**: `http`, `server`, `location`

Defines conditions under which the response will not be compressed. If at least one value of the string parameters is neither empty nor equal to ‘0’, the response will not be compressed.

### brotli_postpone

**Syntax:** `brotli_postpone size;`\
**Default:** `brotli_postpone 0;`\
**Context:** `http`, `server`, `location`

Postpones encoder initialization until the specified amount of input has been buffered. A flush or the end of the response starts compression earlier.

### brotli_no_buffer

**Syntax:** `brotli_no_buffer on | off;`\
**Default:** `brotli_no_buffer off;`\
**Context:** `http`, `server`, `location`

Passes partially filled output buffers downstream when the currently available input has been consumed. This does not force an encoder flush.

### brotli_http_version

**Syntax:** `brotli_http_version 1.0 | 1.1;`\
**Default:** `brotli_http_version 1.1;`\
**Context:** `http`, `server`, `location`

Alias of [gzip_http_version](https://nginx.org/en/docs/http/ngx_http_gzip_module.html#gzip_http_version).

### brotli_proxied

**Syntax:** `brotli_proxied off | expired | no-cache | no-store | private | no_last_modified | no_etag | auth | any ...;`\
**Default:** `brotli_proxied off;`\
**Context:** `http`, `server`, `location`

Alias of [gzip_proxied](https://nginx.org/en/docs/http/ngx_http_gzip_module.html#gzip_proxied).

### brotli_disable

**Syntax:** `brotli_disable regex ...;`\
**Default:** —\
**Context:** `http`, `server`, `location`

Alias of [gzip_disable](https://nginx.org/en/docs/http/ngx_http_gzip_module.html#gzip_disable).

## Variables

### `$brotli_ratio`

Achieved compression ratio, computed as the ratio between the original
and compressed response sizes.

## Sample configuration

```
brotli on;
brotli_comp_level 6;
brotli_static on;
brotli_types application/atom+xml application/javascript application/json application/vnd.api+json application/rss+xml
             application/vnd.ms-fontobject application/x-font-opentype application/x-font-truetype
             application/x-font-ttf application/x-javascript application/xhtml+xml application/xml
             font/eot font/opentype font/otf font/truetype image/svg+xml image/vnd.microsoft.icon
             image/x-icon image/x-win-bitmap text/css text/javascript text/plain text/xml;
```

## Contributing

See [Contributing](CONTRIBUTING.md).

## License

    Copyright (C) 2002-2015 Igor Sysoev
    Copyright (C) 2011-2015 Nginx, Inc.
    Copyright (C) 2015 Google Inc.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:
    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
    OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
    OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
    SUCH DAMAGE.


## Compatibility

Dynamic compression applies to main requests with status 200, 207, 403, or 404. HEAD responses and responses with an existing nonempty `Content-Encoding` are passed through. `Cache-Control: no-transform` prevents dynamic compression.

The core `gzip_http_version`, `gzip_proxied`, and `gzip_disable` settings also apply. Their defaults restrict compression to HTTP/1.1 or later and disable it for requests with a `Via` header. The `brotli_*` aliases configure the same core settings.

Directives whose context includes `when` support [ngx_expr_module](https://github.com/HanadaLee/ngx_expr_module). The module also builds on unpatched nginx without expr support.

## Testing

`util/run-tests.sh` runs the existing suite and the regressions under `t/parity/`. The latter use [nginx-tests](https://github.com/nginx/nginx-tests) and cover proxy buffering on and off, slow clients, cancellation, streaming, HTTP/2 and HTTP/3, static files, and conditional configuration. Set `TEST_NGINX_BINARY` and `PERL5LIB` to run `prove -v t/parity/*.t` directly.
