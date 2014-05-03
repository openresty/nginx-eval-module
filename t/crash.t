# vi:filetype=

use lib 'lib';
use Test::Nginx::Socket; # skip_all => 'ngx_memc storage commands do not work with the ngx_eval module';

repeat_each(2);

plan tests => repeat_each() * 1;

run_tests();

__DATA__

=== TEST 1: without location
--- config
    } eval $var { }
--- request
    GET /t
--- must_die
