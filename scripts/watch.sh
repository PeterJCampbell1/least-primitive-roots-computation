#!bin/bash

d=$(ls -td ~/lpr_runs/*/ | head -1); watch -n 10 tail -n 10 -F "$d"/lpr_*.log
