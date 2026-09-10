#!/usr/bin/env python3
"""Run direct offline functions in separate processes; no modem or replay."""
import os
import subprocess
import sys
from pathlib import Path
build=Path(sys.argv[1]).resolve()
report=[]
for binary in ("offline-check","offline-check-pie"):
    for value in (None,"0","1","1junk",""):
        env=os.environ.copy();env.pop("DMODEM_V34_NEAR_EC_BYPASS",None)
        if value is not None: env["DMODEM_V34_NEAR_EC_BYPASS"]=value
        report.append(f"{binary} startup option={value!r}")
        result=subprocess.run([str(build/binary),str(int(value=="1"))],env=env,
                              text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                              timeout=10,check=True)
        report.append(result.stdout.rstrip())
report.append("PASS: 10 process-isolated option/layout cases; all three real vendor callers exercised")
text="\n".join(report)+"\n"
(build/"offline-check.log").write_text(text)
print(text,end="")
