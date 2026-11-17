use strict;
use warnings;

use Test::More;
use Test::Nginx qw/:DEFAULT http_content/;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

my $t = Test::Nginx->new()->has(qw/http/);
my $plain = 'feature and static response ' x 4096;
$t->write_file('body.txt', $plain);
is(system('python3', '-B', "$FindBin::Bin/lib/encode.py", 'br',
    $t->testdir() . '/body.txt', $t->testdir() . '/body.txt.br'), 0,
    'precompressed fixture');
my $compressed = $t->read_file('body.txt.br');
my $conditional = $ENV{TEST_NGINX_EXPR} ? <<'EOF' : '';
    expr skip str_eq $arg_skip 1;
    expr small str_eq $arg_small 1;
    expr fast str_eq $arg_fast 1;
    when skip {
        brotli_static off;
        brotli off;
    }
    when small { brotli_max_length 1; }
    when fast { brotli_comp_level 0; }
EOF
my $config = <<'EOF';
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    gzip off;
    gzip_vary on;
    default_type text/plain;
    brotli_min_length 0;
    brotli_types *;
    __CONDITIONAL__
    brotli on;
    brotli_static on;
    brotli_comp_level 3;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location / { brotli_static off; }
        location /old/ {
            brotli_static off;
            alias %%TESTDIR%%/;
            brotli_http_version 1.0;
        }
        location /via/ {
            brotli_static off;
            alias %%TESTDIR%%/;
            brotli_proxied any;
        }
        location /agent/ {
            brotli_static off;
            alias %%TESTDIR%%/;
            brotli_disable BadAgent;
        }
        location /bypass/ {
            brotli_static off;
            alias %%TESTDIR%%/;
            brotli_bypass $arg_skip;
        }
        location /static/ {
            alias %%TESTDIR%%/;
            brotli off;
        }
        location /always/ {
            alias %%TESTDIR%%/;
            brotli off;
            brotli_static always;
        }
        location /direct/ {
            brotli_static off;
            alias %%TESTDIR%%/;
            brotli_comp_level 0;
        }
    }
}
EOF
$config =~ s/__CONDITIONAL__/$conditional/;
write_config($t, $config);
$t->run();
check_response($t, request('/body.txt', 'br'), 'br', $plain,
    'dynamic compression');
check_response($t, request('/body.txt', 'br', '', '1.0'), undef, $plain,
    'default HTTP version restriction');
check_response($t, request('/old/body.txt', 'br', '', '1.0'),
    'br', $plain, 'HTTP version alias');
check_response($t, request('/body.txt', 'br', "Via: 1.1 proxy\r\n"),
    undef, $plain, 'default proxy restriction');
check_response($t, request('/via/body.txt', 'br', "Via: 1.1 proxy\r\n"),
    'br', $plain, 'proxy alias');
check_response($t, request('/agent/body.txt', 'br', "User-Agent: BadAgent\r\n"),
    undef, $plain, 'client restriction alias');
check_response($t, request('/bypass/body.txt?skip=1', 'br'),
    undef, $plain, 'bypass predicate');
my $response = request('/static/body.txt', 'br');
check_response($t, $response, 'br', $plain, 'static file');
is(http_content($response), $compressed, 'static bytes are unchanged');
check_response($t, request('/static/body.txt', 'identity'), undef, $plain,
    'static identity fallback');
check_response($t, request('/always/body.txt', 'identity'), 'br', $plain,
    'static always');
my ($etag) = $response =~ /^ETag: ([^\r\n]+)/mi;
like(request('/static/body.txt', 'br', "If-None-Match: $etag\r\n"),
    qr/^HTTP\/1.1 304 /, 'static conditional response');
$response = request('/static/body.txt', 'br', "Range: bytes=0-9\r\n");
like($response, qr/^HTTP\/1.1 206 /, 'encoded byte range');
is(http_content($response), substr($compressed, 0, 10), 'encoded byte range data');
if ($ENV{TEST_NGINX_EXPR}) {
    check_response($t, request('/body.txt?skip=1', 'br'), undef, $plain,
        'conditional dynamic disable');
    check_response($t, request('/static/body.txt?skip=1', 'br'), undef, $plain,
        'conditional static disable');
    check_response($t, request('/body.txt?small=1', 'br'), undef, $plain,
        'conditional maximum length');
    is(http_content(request('/body.txt?fast=1', 'br')),
        http_content(request('/direct/body.txt', 'br')),
        'conditional compression level matches a direct setting');
}
$t->stop();
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no policy or static errors');
undef $t;
done_testing();
