#!/bin/sh

rm -r nginx/sbin nginx/*_temp nginx/logs nginx/conf/*
cp nginx.conf nginx/conf/
