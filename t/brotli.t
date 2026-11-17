#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx qw/:DEFAULT http_content/;

my $t = Test::Nginx->new()->has(qw/http/)->has_daemon('brotli')->plan(6);
my $plain = 'brotli response body ' x 1024;
my $dir = $t->testdir();

$t->write_file('body.txt', $plain);
system('brotli', '-f', '-o', "$dir/body.txt.br", "$dir/body.txt") == 0
    or BAIL_OUT('cannot create Brotli fixture');
$t->write_file_expand('nginx.conf', <<'EOF');
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    default_type text/plain;
    brotli_http_version 1.0;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location / {
            brotli on;
            brotli_types text/plain;
            brotli_min_length 0;
        }
        location /static/ {
            alias %%TESTDIR%%/;
            brotli_static on;
        }
    }
}
EOF
$t->run();

my $identity = http_get('/body.txt');
unlike($identity, qr/Content-Encoding:/i, 'no compression without negotiation');
is(http_content($identity), $plain, 'identity body preserved');

for my $path ('/body.txt', '/static/body.txt') {
    my $response = http("GET $path HTTP/1.0\r\n"
        . "Host: localhost\r\nAccept-Encoding: br\r\n\r\n");
    like($response, qr/Content-Encoding: br\r\n/i, "$path uses Brotli");
    $t->write_file('response.br', http_content($response));
    system('brotli', '-d', '-f', '-o', "$dir/decoded", "$dir/response.br") == 0
        or BAIL_OUT('cannot decode Brotli response');
    is($t->read_file('decoded'), $plain, "$path round-trips byte-for-byte");
}
