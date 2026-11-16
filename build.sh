#!/bin/sh
# Сборка через Docker (ничего ставить не нужно, кроме Docker)
docker run --rm -v "$(pwd)":/src -w /src pspdev/pspdev make
