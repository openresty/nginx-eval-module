#!/bin/bash

# this file is mostly meant to be used by the author himself.

root=`pwd`
home=~
version=$1
force=$2
disable_pcre2=--without-pcre2
answer=`../openresty/util/ver-ge "$version" 1.25.1`
if [ "$answer" = "N" ]; then
    disable_pcre2="";
fi

ngx-build $force $version \
          $disable_pcre2 \
          --with-http_addition_module \
          --without-mail_pop3_module \
          --without-mail_imap_module \
          --without-mail_smtp_module \
          --without-http_upstream_ip_hash_module \
          --without-http_empty_gif_module \
          --without-http_memcached_module \
          --without-http_referer_module \
          --without-http_autoindex_module \
          --without-http_auth_basic_module \
          --without-http_userid_module \
          --add-module=$root $opts \
          --add-module=$root/../echo-nginx-module \
          --add-module=$root/../memc-nginx-module \
          --with-debug
          #--add-module=$root/../vallery/eval-nginx-module \
          #--add-module=$root/../ndk-nginx-module \
          #--add-module=$home/work/nginx/nginx_upstream_hash-0.3 \
  #--without-http_ssi_module  # we cannot disable ssi because echo_location_async depends on it (i dunno why?!)

