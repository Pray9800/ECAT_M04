# -*- coding: utf-8 -*-
import fitz, glob, sys, io

candidates = glob.glob(r'e:\JT-021\BEIJING_ECAT\*.pdf')
target = None
for c in candidates:
    if 'M04' in c:
        target = c
        break

doc = fitz.open(target)
out = io.StringIO()

pages = [2,5,6,7,8,9,12,13,14,15,18,19,21,27] if len(sys.argv) < 2 else [int(x) for x in sys.argv[1:]]
for p in pages:
    if p <= len(doc):
        out.write(f'==================== PAGE {p} ====================\n')
        out.write(doc[p-1].get_text())
        out.write('\n')

with open(r'e:\JT-021\BEIJING_ECAT\test_M04\manual_text.txt', 'w', encoding='utf-8') as f:
    f.write(out.getvalue())
print('saved', len(out.getvalue()), 'chars')
