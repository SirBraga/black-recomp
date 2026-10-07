#!/bin/bash
cd "$(dirname "$0")/../.."
/usr/bin/env python3 ps2recomp/diagnostics/path3_gate_test.py "$@"
echo "Pode fechar esta janela."
