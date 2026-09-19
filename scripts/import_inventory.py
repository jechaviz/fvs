#!/usr/bin/env python3
from pathlib import Path
import json,sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'shim-python'))
from inventory_import import import_bytes
if len(sys.argv)!=2:raise SystemExit('usage: import_inventory.py FILE.csv|xlsx|xlsm')
p=Path(sys.argv[1]);print(json.dumps(import_bytes(p.read_bytes(),p.name),indent=2,ensure_ascii=False))
