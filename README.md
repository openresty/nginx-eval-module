Name
====

ngx_eval - Capturing subrequest response bodies into NGINX variables

*This module is not distributed with the Nginx source.*

Table of Contents
=================

* [Name](#name)
* [Status](#status)
* [Synopsis](#synopsis)
* [Description](#description)
    * [Use Lua instead](#use-lua-instead)
* [Limitations](#limitations)
* [Nginx Compatibility](#nginx-compatibility)
* [Community](#community)
    * [English Mailing List](#english-mailing-list)
    * [Chinese Mailing List](#chinese-mailing-list)
* [Bugs and Patches](#bugs-and-patches)
* [Original ngx_eval documentation](#original-ngx_eval-documentation)
* [Copyright and License](#copyright-and-license)
* [See Also](#see-also)

Status
======

This module is experimental and production use is discouraged.
If you want similar (but more powerful) functionalities, see
the [ngx_lua](http://github.com/openresty/lua-nginx-module) module instead.

Synopsis
========

```nginx
# an example for working with the ngx_drizzle + ngx_rds_json
# modules, but you must put ngx_rds_json *after*
# ngx_eval during nginx configure, for example:
#     ./configure --add-module=/path/to/nginx-eval-module \
#           --add-module=/path/to/rds-json-nginx-module \
#           --add-module=/path/to/drizzle-nginx-module
location = /mysql {
    eval_subrequest_in_memory off;
    eval_override_content_type text/plain;
    eval_buffer_size 4k; # default 4k, truncated if overflown
    eval $res {
        drizzle_query "select * from cats";
        drizzle_pass my_mysql_backend;
        rds_json on;
    }
    # now $res holds the JSON formatted result set
    if ($res ~ '"Tom"') {
        echo "Found the Tom cat!";
        break;
    }
    echo "The Tom cat is missing!";
}

# an example for working with the ngx_postgres module
location = /login {
   eval_subrequest_in_memory off;
   eval_override_content_type text/plain;
   eval_buffer_size 1k;
   eval $uid {
       postgres_query "select id
           from users
           where name=$arg_name and pass=$arg_pass";
       postgres_pass pg_backend;
       postgres_output value 0 0;
   }
   if ($uid !~ '^\d+$') {
       rewrite ^ /relogin redirect; break;
   }
   # your content handler settings...
}
```

Description
===========

This fork of ngx_eval can work with any content handlers and
even with filters enabled as long as you put `ngx_eval` *before*
your filter modules during nginx configure, for instance

```
./configure --prefix=/opt/nginx \
        --add-module=/path/to/this/nginx-eval-module \
        --add-module=/path/to/your/filter/module \
        --add-module=/path/to/your/other/filters
```

such that `ngx_eval`'s filter works *after* your filter modules.

Starting from NGINX 1.9.11, you can also compile this module as a dynamic module, by using the `--add-dynamic-module=PATH` option instead of `--add-module=PATH` on the
`./configure` command line above. And then you can explicitly load the module in your `nginx.conf` via the [load_module](http://nginx.org/en/docs/ngx_core_module.html#load_module)
directive, for example,

```nginx
load_module /path/to/modules/ngx_http_eval_module.so;
```

[Back to TOC](#table-of-contents)

Use Lua instead
---------------

Nowadays, however, we prefer [ngx_lua](http://github.com/openresty/lua-nginx-module) to do the tasks originally done by `ngx_eval`
because of various inherent limitations in `ngx_eval`.

Here's a small example using ngx_lua:

```nginx
location = /filter-spam {
    internal;
    proxy_pass http://blah.blah/checker;
}

location / {
    rewrite_by_lua '
        local res = ngx.location.capture("/filter-spam")
        if res.status ~= ngx.HTTP_OK or res.body == nil then
            return
        end
        if string.match(res.body, "SPAM") then
            return ngx.redirect("/terms-of-use")
        end
    ';

    fastcgi_pass ...;
}
```

this is roughly equivalent to

```nginx
location / {
    eval $res {
        proxy_pass http://blah.blah/checker;
    }
    if ($res ~ 'SPAM') {
        rewrite ^ /terms-of-use redirect;
        break;
    }

    fastcgi_pass ...;
}
```

Even though the Lua example is a little longer but is much more flexible and stable.

[Back to TOC](#table-of-contents)


[Back to TOC](#table-of-contents)

Limitations
===========

* The contents of subrequests issued from within the eval block
(like those fired by echo_subrequest) won't be captured properly.

[Back to TOC](#table-of-contents)

Nginx Compatibility
===================

The following versions of Nginx should work with this module:

* 1.29.x (last tested: 1.29.2)
* 1.27.x (last tested: 1.27.1)
* 1.25.x (last tested: 1.25.4)
* 1.19.x (last tested: 1.19.1)
* 1.17.x (last tested: 1.17.3)
* 1.9.x (last tested: 1.9.7)
* 1.8.x
* 1.7.x (last tested: 1.7.7)
* 1.5.x (last tested: 1.5.12)
* 1.4.x (last tested: 1.4.4)
* 1.3.x (last tested: 1.3.11)
* 1.2.x (last tested: 1.2.9)
* 1.1.x (last tested: 1.1.5)
* 1.0.x (last tested: 1.0.15)
* 0.9.x (last tested: 0.9.4)
* 0.8.0 ~ 0.8.41, 0.8.54+ (0.8.42 ~ 0.8.53 requires patching, see below) (last tested: 0.8.54)
* 0.7.x >= 0.7.21 (last tested: 0.7.68)

Note that nginx 0.8.42 ~ 0.8.53 won't work due to a [famous regression](http://forum.nginx.org/read.php?29,103078,103078) appeared
since 0.8.42.

[Back to TOC](#table-of-contents)

Community
=========

[Back to TOC](#table-of-contents)

English Mailing List
--------------------

The [openresty-en](https://groups.google.com/group/openresty-en) mailing list is for English speakers.

[Back to TOC](#table-of-contents)

Chinese Mailing List
--------------------

The [openresty](https://groups.google.com/group/openresty) mailing list is for Chinese speakers.

[Back to TOC](#table-of-contents)

Bugs and Patches
================

Please submit bug reports, wishlists, or patches by

1. creating a ticket on the [GitHub Issue Tracker](https://github.com/openresty/nginx-eval-module/issues),
1. or posting to the [OpenResty community](#community).

[Back to TOC](#table-of-contents)

Original ngx_eval documentation
===============================

Documentation for this module could be found under following URLs:

* English: http://www.grid.net.ru/nginx/eval.en.html

* Russian: http://www.grid.net.ru/nginx/eval.ru.html

This work is commissioned by gadu-gadu.pl

[Back to TOC](#table-of-contents)

Copyright and License
=====================

This module is licensed under the BSD license.

Copyright (C) 2009-2010, by Valery Kholodkov valery+nginx@grid.net.ru

Copyright (C) 2010-2016, by Yichun "agentzh" Zhang (章亦春) <agentzh@gmail.com>, CloudFlare Inc.

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

[Back to TOC](#table-of-contents)

See Also
========
* the [ngx_lua](http://github.com/openresty/lua-nginx-module) module.

[Back to TOC](#table-of-contents)

