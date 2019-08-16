#!/bin/bash

# run from shenango directory

echo "Runtime"
cloc runtime/* bindings/* inc/runtime/* --exclude-lang=D

echo "IOKernel"
cloc iokernel/* inc/iokernel/* --exclude-lang=D

echo "Ksched"
cloc ksched/*

echo "Base"
cloc base/* net/* inc/base/* inc/asm/* inc/net/* --exclude-lang=D

echo "Spin-server + Loadgen"
cloc apps/synthetic/* --exclude-lang=D
