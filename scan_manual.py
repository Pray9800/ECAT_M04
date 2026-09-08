# -*- coding: utf-8 -*-
import fitz, glob, os, sys

# 在工程根目录找M04手册
candidates = glob.glob(r'e:\JT-021\BEIJING_ECAT\*.pdf')
target = None
for c in candidates:
    base = os.path.basename(c)
    if 'M04' in base or 'EC2' in base:
        target = c
        break

if not target:
    print('NO M04 manual found'); sys.exit(1)

print('Using:', target)
doc = fitz.open(target)
print('pages:', len(doc))

keys = ['485', '串口', 'COM', '通道', '接口']
for i in range(len(doc)):
    t = doc[i].get_text()
    hit = [k for k in keys if k in t]
    if hit:
        print(f'--- page {i+1} hits: {hit}')
