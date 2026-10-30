#!/usr/bin/env bash
set -euo pipefail
# Toolchain for elixir-bandit: Elixir >= 1.17 + OTP >= 27 (mix.exs requires ~> 1.17).
# Run as root. Idempotent; installs only what's missing.
# - Arch/CachyOS (pacman): repo Erlang/Elixir are current, install directly.
# - Ubuntu 24.04 noble (apt): archive ships Elixir 1.14/OTP 25 which is TOO OLD,
#   so install modern OTP via the RabbitMQ Erlang PPA (launchpad, verified 200;
#   Erlang Solutions host persistently 502s, removed). The PPA provides OTP 27/28
#   and the archive Elixir dependency resolves to PPA Erlang.
# App deps pinned via mix.lock (bandit 1.14, plug 1.20, postgrex 0.22, jason 1.4).
# PPA via add-apt-repository (handles Launchpad key); apt-get update retries 1x. On OTP >= 27 but
# Elixir < 1.17, overlay pinned prebuilt Elixir from GitHub (reliable host).
ELIXIR_FALLBACK_VSN="1.19.5"
ELIXIR_FALLBACK_URL="https://github.com/elixir-lang/elixir/releases/download/v${ELIXIR_FALLBACK_VSN}/elixir-otp-27.zip"
if command -v pacman >/dev/null 2>&1; then
  pacman -Sy --noconfirm 2>&1 | tail -1 || true
  pacman -S --noconfirm erlang elixir base-devel 2>&1 | tail -2 || true
elif command -v apt-get >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update || apt-get update
  apt-get install -y curl gnupg apt-transport-https ca-certificates wget unzip software-properties-common 2>&1 | tail -2 || true
  add-apt-repository -y ppa:rabbitmq/rabbitmq-erlang || add-apt-repository -y ppa:rabbitmq/rabbitmq-erlang
  apt-get update || apt-get update
  apt-get install -y erlang elixir build-essential 2>&1 | tail -3 || true
  # Apt gate (advisory here; hard gate runs below). If the archive Elixir is
  # still < 1.17 while OTP >= 27, overlay pinned prebuilt Elixir from GitHub
  # (reliable host). If OTP itself is < 27 there is no safe overlay, so fail
  # with versions listed.
  gate_state="$(elixir -e 'vsn = System.version() |> Version.parse!(); otp = :erlang.system_info(:otp_release) |> to_string() |> String.to_integer(); IO.puts(if Version.match?(vsn, ">= 1.17.0") and otp >= 27, do: "ok", else: "old")' 2>/dev/null || echo old)"
  if [ "$gate_state" != "ok" ]; then
    otp_major="$(erl -eval 'io:format("~s", [erlang:system_info(otp_release)]), halt().' -noshell 2>/dev/null || echo 0)"
    if [ "${otp_major:-0}" -lt 27 ] 2>/dev/null; then
      echo "FATAL: apt OTP too old (need OTP >= 27, Elixir >= 1.17.0 for mix.exs ~> 1.17)" >&2
      elixir --version >&2 || true
      erl -eval 'erlang:display(erlang:system_info(otp_release)), halt().' -noshell >&2 || true
      exit 1
    fi
    tmp_dir="$(mktemp -d /tmp/elixir-fallback_XXXXXX)"
    tmp_zip="$tmp_dir/elixir-fallback.zip"
    for attempt in 1 2 3; do
      if wget --tries=3 --timeout=30 -O "$tmp_zip" "$ELIXIR_FALLBACK_URL" && [ -s "$tmp_zip" ]; then
        break
      fi
      echo "warning: Elixir fallback download attempt ${attempt}/3 failed (retrying)" >&2
      { [ "$attempt" -lt 3 ] && sleep $((5 * attempt)); } || true
    done
    if [ ! -s "$tmp_zip" ]; then
      echo "FATAL: could not download pinned Elixir ${ELIXIR_FALLBACK_VSN} fallback from ${ELIXIR_FALLBACK_URL}" >&2
      rm -rf "$tmp_dir"
      exit 1
    fi
    unzip -o "$tmp_zip" -d /usr/local
    rm -rf "$tmp_dir"
    hash -r || true
  fi
else
  echo "no supported package manager (pacman/apt) found" >&2; exit 1
fi
elixir --version || true
erl -eval 'erlang:display(erlang:system_info(otp_release)), halt().' -noshell || true
# Hex/Rebar for root (build.sh repeats for the build user)
mix local.hex --force 2>&1 | tail -1 || true
mix local.rebar --force 2>&1 | tail -1 || true
echo "versions:"
elixir --version | tail -1 || true
# Gate: mix.exs needs Elixir ~> 1.17 and OTP >= 27. Fail loudly so the bench box
# errors here instead of with a confusing mix compile error.
elixir -e 'vsn = System.version() |> Version.parse!(); otp = :erlang.system_info(:otp_release) |> to_string() |> String.to_integer(); ok = Version.match?(vsn, ">= 1.17.0") and otp >= 27; IO.puts("gate: elixir #{vsn} otp #{otp} ok=#{ok}"); unless ok, do: exit({:shutdown, 1})'
