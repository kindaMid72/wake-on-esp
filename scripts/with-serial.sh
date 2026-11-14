#!/usr/bin/env bash
# Run a command with serial-port access. Right after `make setup` the shell does not yet have the
# new `dialout` group (needs re-login); in that case run the command under `sg dialout`.
if id -nG | tr ' ' '\n' | grep -qx dialout || ! getent group dialout | grep -qw "$(id -un)"; then
  exec "$@"
fi
exec /usr/bin/sg dialout -c "$(printf '%q ' "$@")"
