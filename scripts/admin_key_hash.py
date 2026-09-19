#!/usr/bin/env python3
import getpass, hashlib
key=getpass.getpass('FVS admin key: ')
if len(key)<32: raise SystemExit('admin key must be at least 32 characters')
print(hashlib.sha256(key.encode('utf-8')).hexdigest())
