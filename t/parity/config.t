use strict;
use warnings;
use Test::More;
use Test::Nginx;

my @cases = (
    ['brotli_window 1k;', 1],
    ['brotli_window 8m;', 1],
    ['brotli_window 3k;', 0],
    ['brotli_window 32m;', 0],
    ['brotli_buffers 0 1k;', 0],
    ['brotli_postpone 64k; brotli_no_buffer on;', 1]
);
for my $case (@cases) {
    my ($directive, $expected) = @$case;
    my $t = Test::Nginx->new()->has(qw/http/);
    $t->write_file_expand('nginx.conf', <<EOF);
%%TEST_GLOBALS%%
error_log %%TESTDIR%%/config.log;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    $directive
    server { listen 127.0.0.1:8080; }
}
EOF
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if (!$pid) {
        open STDOUT, '>', $t->testdir() . '/command.log' or die $!;
        open STDERR, '>&', \*STDOUT or die $!;
        exec $ENV{TEST_NGINX_BINARY}, '-t', '-p', $t->testdir(), '-c', 'nginx.conf';
        die "exec: $!";
    }
    waitpid($pid, 0);
    my $ok = $? == 0 ? 1 : 0;
    is($ok, $expected, $directive);
}
done_testing();
