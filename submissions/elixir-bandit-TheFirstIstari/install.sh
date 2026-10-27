#!/usr/bin/env bash
set -euo pipefail
# Toolchain for elixir-bandit: Elixir >= 1.17 + OTP >= 27 (mix.exs requires ~> 1.17).
# Run as root. Idempotent; installs only what's missing.
# - Arch/CachyOS (pacman): repo Erlang/Elixir are current, install directly.
# - Ubuntu 24.04 noble (apt): archive ships Elixir 1.14/OTP 25 which is TOO OLD,
#   so pin via the Erlang Solutions repo (esl-erlang 1:28+ + elixir 1.19+).
# App deps pinned via mix.lock (bandit 1.14, plug 1.20, postgrex 0.22, jason 1.4).
if command -v pacman >/dev/null 2>&1; then
  pacman -Sy --noconfirm 2>&1 | tail -1 || true
  pacman -S --noconfirm erlang elixir base-devel 2>&1 | tail -2 || true
elif command -v apt-get >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get install -y wget gnupg2 ca-certificates 2>&1 | tail -2 || true
  if ! dpkg -l erlang-solutions >/dev/null 2>&1; then
    tmp_deb="$(mktemp /tmp/erlang-solutions_XXXXXX.deb)"
    trap 'rm -f "$tmp_deb"' EXIT
    wget -O "$tmp_deb" https://packages.erlang-solutions.com/erlang-solutions_2.0_all.deb
    dpkg -i "$tmp_deb"
    rm -f "$tmp_deb"
    trap - EXIT
  fi
  apt-get update
  apt-get install -y esl-erlang elixir build-essential 2>&1 | tail -3 || true
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
