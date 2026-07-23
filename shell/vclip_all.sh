#!/bin/bash
set -e  # allow errors
for yaml_file in *.yaml; do
	"vclip.exe -y $yaml_file"
done
