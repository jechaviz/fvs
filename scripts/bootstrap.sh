#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f .env ]; then
  cp .env.example .env
  echo "created .env from .env.example"
else
  echo ".env already exists; leaving it unchanged"
fi
printf '%s\n' "next: review .env, then run 'make dev-python' or 'make dev-php'"
