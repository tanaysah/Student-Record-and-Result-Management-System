#!/usr/bin/env bash
set -e

mkdir -p data reports

if [ -x ./student_system_web ]; then
  echo "Starting student_system_web (C web server)"
  exec ./student_system_web
fi

echo "Starting Python wrapper"
exec python3 web_wrapper.py
